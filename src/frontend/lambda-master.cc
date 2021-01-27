#include "master/lambda-master.hh"

#include <getopt.h>
#include <glog/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "messages/message.hh"
#include "messages/utils.hh"
#include "net/lambda.hh"
#include "net/requests.hh"
#include "net/socket.hh"
#include "net/transfer_mcd.hh"
#include "net/util.hh"
#include "schedulers/adaptive.hh"
#include "schedulers/dynamic.hh"
#include "schedulers/null.hh"
#include "schedulers/rootonly.hh"
#include "schedulers/static.hh"
#include "schedulers/uniform.hh"
#include "util/digest.hh"
#include "util/exception.hh"
#include "util/random.hh"
#include "util/status_bar.hh"
#include "util/temp_file.hh"
#include "util/tokenize.hh"
#include "util/uri.hh"
#include "util/util.hh"

using namespace std;
using namespace chrono;
using namespace meow;
using namespace r2t2;
using namespace pbrt;

using OpCode = Message::OpCode;

constexpr milliseconds EXIT_GRACE_PERIOD { 30'000 };

map<LambdaMaster::Worker::Role, size_t> LambdaMaster::Worker::active_count = {};
WorkerId LambdaMaster::Worker::next_id = 0;

LambdaMaster::~LambdaMaster()
{
  try {
    if ( not scene_dir.name().empty() ) {
      filesystem::remove_all( scene_dir.name() );
    }
  } catch ( exception& ex ) {
  }
}

LambdaMaster::LambdaMaster( const uint16_t listen_port,
                            const uint16_t client_port,
                            const uint32_t max_workers_,
                            const uint32_t ray_generators_,
                            const string& public_address_,
                            const string& storage_backend_uri_,
                            const string& aws_region_,
                            unique_ptr<Scheduler>&& scheduler_,
                            const MasterConfiguration& user_config )
  : config( user_config )
  , job_id(
      digest::sha256_base58( to_string( time( nullptr ) ) ).substr( 0, 6 ) )
  , public_address( public_address_ )
  , storage_backend_uri( storage_backend_uri_ )
  , storage_backend_info( storage_backend_uri )
  , scene_storage_backend( aws_credentials,
                           storage_backend_info.bucket,
                           storage_backend_info.region,
                           storage_backend_info.path )
  , job_storage_backend( aws_credentials,
                         storage_backend_info.bucket,
                         storage_backend_info.region )
  , aws_region( aws_region_ )
  , aws_address( LambdaInvocationRequest::endpoint( aws_region ), "https" )
  , max_workers( max_workers_ )
  , ray_generators( ray_generators_ )
  , scheduler( move( scheduler_ ) )
  , worker_rule_categories( { loop.add_category( "Socket" ),
                              loop.add_category( "Message read" ),
                              loop.add_category( "Message write" ),
                              loop.add_category( "Process message" ) } )
  , https_rule_categories( { loop.add_category( "HTTPS" ),
                             loop.add_category( "HTTPResponse read" ),
                             loop.add_category( "HTTPRequest write" ),
                             loop.add_category( "Process HTTPResponse" ) } )
{
  signals.set_as_mask();

  const string scene_path = scene_dir.name();
  filesystem::create_directories( scene_path );

  if ( config.alt_scene_file ) {
    // read camera description
    const string alt_scene_data = [f = *config.alt_scene_file] {
      ifstream fin { f };
      stringstream ss;
      ss << fin.rdbuf();
      return ss.str();
    }();

    const auto output_path = filesystem::path( scene_path );
    scene::DumpSceneObjects( alt_scene_data, output_path );

    auto get_file_hash = []( const filesystem::path& p ) {
      ifstream fin { p };
      stringstream ss;
      ss << fin.rdbuf();
      return digest::sha256_base58( ss.str() );
    };

    vector<storage::PutRequest> upload_requests;

    for ( const auto scene_obj : SceneData::base_object_types ) {
      const auto base_name = scene::GetObjectName( scene_obj, 0 );
      const auto p = output_path / base_name;
      if ( filesystem::exists( p ) ) {
        const auto object_hash = get_file_hash( p ).substr( 0, 8 );
        const auto alt_name = base_name + "_" + object_hash;
        alternative_object_names[scene_obj] = alt_name;
        upload_requests.emplace_back( p, alt_name );
      }
    }

    if ( not upload_requests.empty() ) {
      cout << "\u2197 Uploading alternative scene "
           << pluralize( "object", upload_requests.size() ) << " (";

      for ( size_t i = 0; i < upload_requests.size(); i++ ) {
        cout << upload_requests[i].object_key;
        if ( i != upload_requests.size() - 1 ) {
          cout << ", ";
        }
      }

      cout << ")... ";
      scene_storage_backend.put( upload_requests );
      cout << "done." << endl;
    }
  }

  /* download required scene objects from the bucket */
  auto get_scene_object_request = [&scene_path]( const ObjectType type ) {
    return storage::GetRequest { scene::GetObjectName( type, 0 ),
                                 filesystem::path( scene_path )
                                   / scene::GetObjectName( type, 0 ) };
  };

  vector<storage::GetRequest> scene_obj_reqs;
  for ( const auto scene_obj : SceneData::base_object_types ) {
    if ( alternative_object_names.count( scene_obj ) == 0 ) {
      scene_obj_reqs.push_back( get_scene_object_request( scene_obj ) );
    }
  }

  if ( not scene_obj_reqs.empty() ) {
    cout << "\u2198 Downloading scene data... ";
    scene_storage_backend.get( scene_obj_reqs );
    cout << "done." << endl;
  }

  /* now we can initialize the scene */
  scene = { scene_path, config.samples_per_pixel, config.crop_window };

  /* create the invocation payload */
  protobuf::InvocationPayload invocation_proto;
  invocation_proto.set_storage_backend( storage_backend_uri );
  invocation_proto.set_coordinator( public_address );
  invocation_proto.set_samples_per_pixel( config.samples_per_pixel );
  invocation_proto.set_max_path_depth( config.max_path_depth );
  invocation_proto.set_bagging_delay( config.bagging_delay.count() );
  invocation_proto.set_ray_log_rate( config.ray_log_rate );
  invocation_proto.set_bag_log_rate( config.bag_log_rate );
  invocation_proto.set_directional_treelets( PbrtOptions.directionalTreelets );

  for ( const auto& server : config.memcached_servers ) {
    *invocation_proto.add_memcached_servers() = server;
  }

  invocation_payload = protoutil::to_json( invocation_proto );

  /* initializing the treelets array */
  const size_t treelet_count = scene.base.GetTreeletCount();
  treelets.reserve( treelet_count );
  treelet_stats.reserve( treelet_count );

  for ( size_t i = 0; i < treelet_count; i++ ) {
    treelets.emplace_back( i );
    treelet_stats.emplace_back();
    unassigned_treelets.insert( i );
  }

  queued_ray_bags.resize( treelet_count );
  pending_ray_bags.resize( treelet_count );

  tiles = Tiles { config.tile_size,
                  scene.sample_bounds,
                  scene.base.samplesPerPixel,
                  ray_generators ? ray_generators : max_workers };

  if ( config.auto_name_log_dir_tag ) {
    // setting the directory name based on job info
    string dir_name = ParsedURI( storage_backend_uri ).path + "_w"
                      + to_string( max_workers ) + "_"
                      + to_string( scene.base.samplesPerPixel ) + "spp_d"
                      + to_string( config.max_path_depth );

    if ( not config.auto_name_log_dir_tag->empty() ) {
      dir_name += "_" + ( *config.auto_name_log_dir_tag );
    }

    config.logs_directory /= dir_name;
  }

  /* are we logging anything? */
  if ( config.collect_debug_logs || config.write_stat_logs
       || config.ray_log_rate > 0 || config.bag_log_rate > 0 ) {
    filesystem::create_directories( config.logs_directory );
  }

  if ( config.write_stat_logs ) {
    ws_stream.open( config.logs_directory / "workers.csv", ios::trunc );
    tl_stream.open( config.logs_directory / "treelets.csv", ios::trunc );
    alloc_stream.open( config.logs_directory / "allocations.csv", ios::trunc );
    summary_stream.open( config.logs_directory / "summary.csv", ios::trunc );

    ws_stream
      << "timestamp,workerId,pathsFinished,"
         "raysEnqueued,raysAssigned,raysDequeued,"
         "bytesEnqueued,bytesAssigned,bytesDequeued,"
         "bagsEnqueued,bagsAssigned,bagsDequeued,"
         "numSamples,bytesSamples,bagsSamples,cpuUsage,bytesDownloaded\n";

    tl_stream << "timestamp,treeletId,raysEnqueued,raysDequeued,"
                 "bytesEnqueued,bytesDequeued,bagsEnqueued,bagsDequeued,"
                 "enqueueRate,dequeueRate,cpuUsage\n";

    alloc_stream << "workerId,treeletId,action\n";

    summary_stream << "workerId,treeletId,trace,shade,nodes,visited\n";
  }

  auto print_info = []( const string& key, auto value ) {
    constexpr size_t FIELD_WIDTH = 25;
    cout << "  " << setw( FIELD_WIDTH - 2 ) << left
         << key.substr( 0, FIELD_WIDTH - 4 ) << "    \x1B[1m" << value
         << "\x1B[0m" << endl;
  };

  cout << endl << "Job info:" << endl;
  print_info( "Job ID", job_id );
  print_info( "Working directory", scene_path );
  print_info( "Public address", public_address );
  print_info( "Maximum workers", max_workers );
  print_info( "Ray generators", ray_generators );
  print_info( "Treelet count", treelet_count );
  print_info( "Tile size",
              to_string( tiles.tile_size ) + "\u00d7"
                + to_string( tiles.tile_size ) );
  print_info( "Output dimensions",
              to_string( scene.sample_extent.x ) + "\u00d7"
                + to_string( scene.sample_extent.y ) );
  print_info( "Samples per pixel", scene.base.samplesPerPixel );
  print_info( "Total paths", scene.total_paths );
  print_info( "Logs directory", config.logs_directory.c_str() );

  cout << endl;

  loop.add_rule(
    "Signals",
    Direction::In,
    signal_fd,
    [&] { handle_signal( signal_fd.read_signal() ); },
    [] { return true; } );

  loop.add_rule(
    "Reschedule",
    Direction::In,
    reschedule_timer,
    bind( &LambdaMaster::handle_reschedule, this ),
    [this] { return finished_ray_generators == this->ray_generators; } );

  loop.add_rule(
    "Job exit",
    Direction::In,
    job_exit_timer,
    [&] {
      job_exit_timer.read_event();
      terminate( "Job timeout has been exceeded." );
    },
    [] { return true; } );

  loop.add_rule( "Worker stats",
                 Direction::In,
                 worker_stats_write_timer,
                 bind( &LambdaMaster::handle_worker_stats, this ),
                 [this] { return true; } );

  loop.add_rule( "Worker invocation",
                 Direction::In,
                 worker_invocation_timer,
                 bind( &LambdaMaster::handle_worker_invocation, this ),
                 [this] {
                   return !treelets_to_spawn.empty()
                          && ( Worker::active_count[Worker::Role::Tracer]
                               < this->max_workers );
                 } );

  loop.add_rule( "Status",
                 Direction::In,
                 status_print_timer,
                 bind( &LambdaMaster::handle_status_message, this ),
                 [] { return true; } );

  listener_socket.set_blocking( false );
  listener_socket.set_reuseaddr();
  listener_socket.bind( { "0.0.0.0", listen_port } );
  listener_socket.listen();

  loop.add_rule(
    "Listener",
    Direction::In,
    listener_socket,
    [this] {
      TCPSocket socket = listener_socket.accept();

      /* do we want this worker? */
      if ( Worker::next_id >= this->ray_generators
           && treelets_to_spawn.empty() ) {
        socket.close();
        return;
      }

      const WorkerId worker_id = Worker::next_id++;

      auto connection_close_handler = [this, worker_id] {
        auto& worker = workers[worker_id];
        worker.client.uninstall_rules();

        if ( worker.state == Worker::State::Terminating ) {
          if ( worker.role == Worker::Role::Generator ) {
            last_generator_done = steady_clock::now();
            finished_ray_generators++;
          }

          /* it's okay for this worker to go away,
             let's not panic! */
          worker.state = Worker::State::Terminated;
          Worker::active_count[worker.role]--;

          if ( !worker.outstanding_ray_bags.empty() ) {
            throw runtime_error( "worker died without finishing its work: "
                                 + to_string( worker_id ) );
          }

          return;
        }

        cerr << "Worker info: " << worker.to_string() << endl;

        throw runtime_error(
          "worker died unexpectedly: " + to_string( worker_id )
          + ( worker.aws_log_stream.empty()
                ? ""s
                : ( " ("s + worker.aws_log_stream + ")"s ) ) );
      };

      if ( worker_id < this->ray_generators ) {
        /* This worker is a ray generator
           Let's (1) say hi, (2) tell the worker to fetch the scene,
           (3) generate rays for its tile */

        /* (0) create the entry for the worker */
        auto& worker = workers.emplace_back(
          worker_id, Worker::Role::Generator, move( socket ) );

        assign_base_objects( worker );

        /* (1) saying hi, assigning id to the worker */
        protobuf::Hey hey_proto;
        hey_proto.set_worker_id( worker_id );
        hey_proto.set_job_id( job_id );
        worker.client.push_request(
          { 0, OpCode::Hey, protoutil::to_string( hey_proto ) } );

        /* (2) tell the worker to get the necessary scene objects */
        protobuf::GetObjects objs_proto;
        for ( const SceneObject& obj : worker.objects ) {
          *objs_proto.add_objects() = to_protobuf( obj );
        }

        worker.client.push_request(
          { 0, OpCode::GetObjects, protoutil::to_string( objs_proto ) } );

        /* (3) Tell the worker to generate rays */
        if ( tiles.camera_rays_remaining() ) {
          tiles.send_worker_tile( worker );
        } else {
          /* Too many ray launchers for tile size,
           * so just finish immediately */
          worker.client.push_request( { 0, OpCode::FinishUp, "" } );
          worker.state = Worker::State::FinishingUp;
        }
      } else {
        /* this is a normal worker */
        if ( !treelets_to_spawn.empty() ) {
          const TreeletId treelet_id = treelets_to_spawn.front();
          treelets_to_spawn.pop_front();

          auto& treelet = treelets[treelet_id];
          treelet.pending_workers--;

          /* (0) create the entry for the worker */
          auto& worker = workers.emplace_back(
            worker_id, Worker::Role::Tracer, move( socket ) );

          assign_base_objects( worker );
          assign_treelet( worker, treelet );

          if ( config.write_stat_logs ) {
            alloc_stream << worker_id << ',' << treelet_id << ",add\n";
          }

          /* (1) saying hi, assigning id to the worker */
          protobuf::Hey hey_proto;
          hey_proto.set_worker_id( worker_id );
          hey_proto.set_job_id( job_id );
          worker.client.push_request(
            { 0, OpCode::Hey, protoutil::to_string( hey_proto ) } );

          /* (2) tell the worker to get the scene objects necessary */
          protobuf::GetObjects objs_proto;
          for ( const SceneObject& obj : worker.objects ) {
            *objs_proto.add_objects() = to_protobuf( obj );
          }

          worker.client.push_request(
            { 0, OpCode::GetObjects, protoutil::to_string( objs_proto ) } );

          free_workers.push_back( worker.id );
        } else {
          throw runtime_error( "we accepted a useless worker" );
        }
      }

      workers.back().client.install_rules(
        loop,
        worker_rule_categories,
        [worker_id, this]( Message&& msg ) {
          process_message( worker_id, move( msg ) );
        },
        connection_close_handler );
    },
    [] { return true; },
    [] { throw runtime_error( "listener socket closed" ); } );

  if ( client_port != 0 ) {
    throw runtime_error( "client support is disabled" );
  }
}

string LambdaMaster::Worker::to_string() const
{
  ostringstream oss;

  size_t oustanding_size = 0;
  for ( const auto& bag : outstanding_ray_bags )
    oustanding_size += bag.bag_size;

  oss << "id=" << id << ",state=" << static_cast<int>( state )
      << ",role=" << static_cast<int>( role ) << ",awslog=" << aws_log_stream
      << ",treelets=";

  for ( auto it = treelets.begin(); it != treelets.end(); it++ ) {
    if ( it != treelets.begin() )
      oss << ",";
    oss << *it;
  }

  oss << ",outstanding-bags=" << outstanding_ray_bags.size()
      << ",outstanding-bytes=" << oustanding_size
      << ",rays={.camera:" << rays.camera << ",.generated:" << rays.generated
      << ",.dequeued:" << rays.dequeued << ",.terminated:" << rays.terminated
      << ",.enqueued:" << rays.enqueued << "},active-rays=" << active_rays()
      << ",enqueued=" << stats.enqueued.rays
      << ",assigned=" << stats.assigned.rays
      << ",dequeued=" << stats.dequeued.rays
      << ",samples=" << stats.samples.rays;

  return oss.str();
}

void LambdaMaster::run()
{
  StatusBar::get();

  if ( !config.memcached_servers.empty() ) {
    cout << "\u2192 Flushing " << config.memcached_servers.size()
         << " memcached "
         << pluralize( "server", config.memcached_servers.size() ) << "... ";

    vector<Address> servers;

    for ( auto& server : config.memcached_servers ) {
      string host;
      uint16_t port = 11211;
      tie( host, port ) = Address::decompose( server );
      servers.emplace_back( host, port );
    }

    EventLoop memcached_loop;
    unique_ptr<memcached::TransferAgent> agent
      = make_unique<memcached::TransferAgent>( servers );

    size_t flushed_count = 0;

    memcached_loop.add_rule(
      "eventfd",
      Direction::In,
      agent->eventfd(),
      [&] {
        if ( !agent->eventfd().read_event() )
          return;

        vector<pair<uint64_t, string>> actions;
        agent->try_pop_bulk( back_inserter( actions ) );

        flushed_count += actions.size();
      },
      [&] { return flushed_count < config.memcached_servers.size(); } );

    agent->flush_all();

    while ( memcached_loop.wait_next_event( -1 ) != EventLoop::Result::Exit ) {
      continue;
    }

    cout << "done." << endl;
  }

  if ( ray_generators > 0 ) {
    /* let's invoke the ray generators */
    cout << "\u2192 Launching " << ray_generators << " ray "
         << pluralize( "generator", ray_generators ) << "... ";

    invoke_workers( ray_generators
                    + static_cast<size_t>( 0.1 * ray_generators ) );
    cout << "done." << endl;
  }

  while ( state_ != State::Terminated
          && loop.wait_next_event( -1 ) != EventLoop::Result::Exit ) {
    // cleaning up finished http clients
    if ( not finished_https_clients.empty() ) {
      for ( auto& it : finished_https_clients ) {
        https_clients.erase( it );
      }

      finished_https_clients.clear();
    }

    /* XXX What's happening here? */
    if ( initialized_workers >= max_workers + ray_generators
         && !free_workers.empty()
         && ( tiles.camera_rays_remaining() || queued_ray_bags_count > 0 ) ) {
      handle_queued_ray_bags();
    }
  }

  vector<storage::GetRequest> get_requests;
  const string log_prefix = "jobs/" + job_id + "/logs/";

  ws_stream.close();
  tl_stream.close();
  alloc_stream.close();
  summary_stream.close();

  for ( auto& worker : workers ) {
    if ( worker.state != Worker::State::Terminated ) {
      worker.client.session().socket().shutdown( SHUT_RDWR );
      worker.client.session().socket().close();
    }

    if ( config.collect_debug_logs || config.ray_log_rate
         || config.bag_log_rate ) {
      get_requests.emplace_back( log_prefix + to_string( worker.id ) + ".INFO",
                                 config.logs_directory
                                   / ( to_string( worker.id ) + ".INFO" ) );
    }
  }

  cout << endl;

  cout << loop.summary() << endl;
  cout << global_timer().summary() << endl;

  print_pbrt_stats();
  print_job_summary();

  if ( not config.job_summary_path.empty() ) {
    dump_job_summary( config.job_summary_path );
  } else if ( config.write_stat_logs ) {
    dump_job_summary( config.logs_directory / "info.json" );
  }

  if ( !get_requests.empty() ) {
    cout << "\n\u2198 Downloading " << get_requests.size() << " log file(s)... "
         << flush;
    this_thread::sleep_for( 10s );
    job_storage_backend.get( get_requests );
    cout << "done." << endl;
  }

  cout << endl;
}

void LambdaMaster::handle_signal( const signalfd_siginfo& sig )
{
  switch ( sig.ssi_signo ) {
    case SIGHUP:
    case SIGTERM:
    case SIGQUIT:
      throw runtime_error( "interrupted by signal" );

    case SIGINT:
      cout << endl;
      terminate( "Interrupted by signal." );
      break;

    default:
      throw runtime_error( "unhandled signal" );
  }
}

void LambdaMaster::terminate( const string& why )
{
  if ( state_ != State::Active ) {
    return;
  }

  cout << "\u2192 " << why << endl;
  cout << "\u2192 Winding down workers, will forcefully terminate in "
       << duration_cast<seconds>( EXIT_GRACE_PERIOD ).count() << "s..." << endl;

  state_ = State::WindingDown;
  scheduler = make_unique<NullScheduler>();

  job_timeout_timer.set( 0s, EXIT_GRACE_PERIOD );

  loop.add_rule(
    "Graceful termination",
    [this] { state_ = State::Terminated; },
    [this] {
      return state_ == State::WindingDown
             && Worker::active_count[Worker::Role::Tracer] == 0;
    } );

  loop.add_rule(
    "Forceful termination",
    Direction::In,
    job_timeout_timer,
    [this] {
      job_timeout_timer.read_event();
      state_ = State::Terminated;
    },
    [] { return true; } );
}

void usage( const char* argv0, int exit_code )
{
  cerr << "Usage: " << argv0 << " [OPTION]..." << endl
       << endl
       << "Options:" << endl
       << "  -p --port PORT             port to use" << endl
       << "  -P --client-port PORT      port for clients to connect" << endl
       << "  -i --ip IPSTRING           public ip of this machine" << endl
       << "  -r --aws-region REGION     region to run lambdas in" << endl
       << "  -b --storage-backend NAME  storage backend URI" << endl
       << "  -m --max-workers N         maximum number of workers" << endl
       << "  -G --ray-generators N      number of ray generators" << endl
       << "  -g --debug-logs            collect worker debug logs" << endl
       << "  -w --worker-stats N        log worker stats every N seconds"
       << endl
       << "  -L --log-rays RATE         log ray actions" << endl
       << "  -B --log-bags RATE         log bag actions" << endl
       << "  -D --logs-dir DIR          set logs directory (default: logs/)"
       << endl
       << "  -S --samples N             number of samples per pixel" << endl
       << "  -M --max-depth N           maximum path depth" << endl
       << "  -s --bagging-delay N       bagging delay" << endl
       << "  -a --scheduler TYPE        indicate scheduler type:" << endl
       << "                               - uniform (default)" << endl
       << "                               - static" << endl
       << "                               - all" << endl
       << "                               - none" << endl
       << "  -F --scheduler-file FILE   set the allocation file" << endl
       << "  -c --crop-window X,Y,Z,T   set render bounds to [(X,Y), (Z,T))"
       << endl
       << "  -C --pbrt-scene FILE       specify alternative scene file" << endl
       << "  -T --pix-per-tile N        pixels per tile (default=44)" << endl
       << "  -n --new-tile-send N       threshold for sending new tiles" << endl
       << "  -t --timeout T             exit after T seconds of inactivity"
       << endl
       << "  -j --job-summary FILE      output the job summary in JSON format"
       << endl
       << "  -A --auto-name             set log directory name automatically"
       << endl
       << "  -d --memcached-server      address for memcached" << endl
       << "                             (can be repeated)" << endl
       << "  -h --help                  show help information" << endl;

  exit( exit_code );
}

optional<Bounds2i> parse_crop_window_optarg( const string& arg )
{
  vector<string> args = split( arg, "," );
  if ( args.size() != 4 )
    return {};

  Point2i p_min, p_max;
  p_min.x = stoi( args[0] );
  p_min.y = stoi( args[1] );
  p_max.x = stoi( args[2] );
  p_max.y = stoi( args[3] );

  return make_optional<Bounds2i>( p_min, p_max );
}

int main( int argc, char* argv[] )
{
  if ( argc <= 0 ) {
    abort();
  }

  google::InitGoogleLogging( argv[0] );

  uint16_t listen_port = 50000;
  uint16_t client_port = 0;
  int32_t max_workers = -1;
  int32_t ray_generators = 0;
  string public_ip;
  string storage_backend_uri;
  string region { "us-west-2" };
  bool collect_debug_logs = false;
  bool write_stat_logs = false;
  float ray_log_rate = 0.0;
  float bag_log_rate = 0.0;
  string logs_directory = "logs/";
  optional<string> auto_name_log_dir_tag = nullopt;
  optional<Bounds2i> crop_window;
  uint32_t timeout = 0;
  uint32_t pixels_per_tile = 0;
  uint64_t new_tile_threshold = 10000;
  string job_summary_path;

  optional<filesystem::path> alt_scene_file = nullopt;

  unique_ptr<Scheduler> scheduler = nullptr;
  string scheduler_name;
  optional<string> scheduler_file = nullopt;

  int samples_per_pixel = 0;
  int max_path_depth = 5;
  int tile_size = 0;
  milliseconds bagging_delay = DEFAULT_BAGGING_DELAY;

  uint32_t max_jobs_on_engine = 1;
  vector<string> memcached_servers;
  vector<pair<string, uint32_t>> engines;

  struct option long_options[] = {
    { "port", required_argument, nullptr, 'p' },
    { "client-port", required_argument, nullptr, 'P' },
    { "ip", required_argument, nullptr, 'i' },
    { "aws-region", required_argument, nullptr, 'r' },
    { "storage-backend", required_argument, nullptr, 'b' },
    { "max-workers", required_argument, nullptr, 'm' },
    { "ray-generators", required_argument, nullptr, 'G' },
    { "scheduler", required_argument, nullptr, 'a' },
    { "scheduler-file", required_argument, nullptr, 'F' },
    { "debug-logs", no_argument, nullptr, 'g' },
    { "worker-stats", no_argument, nullptr, 'w' },
    { "log-rays", required_argument, nullptr, 'L' },
    { "log-bags", required_argument, nullptr, 'B' },
    { "logs-dir", required_argument, nullptr, 'D' },
    { "samples", required_argument, nullptr, 'S' },
    { "max-depth", required_argument, nullptr, 'M' },
    { "bagging-delay", required_argument, nullptr, 's' },
    { "crop-window", required_argument, nullptr, 'c' },
    { "pbrt-scene", required_argument, nullptr, 'C' },
    { "timeout", required_argument, nullptr, 't' },
    { "job-summary", required_argument, nullptr, 'j' },
    { "pix-per-tile", required_argument, nullptr, 'T' },
    { "new-tile-send", required_argument, nullptr, 'n' },
    { "directional", no_argument, nullptr, 'I' },
    { "jobs", required_argument, nullptr, 'J' },
    { "memcached-server", required_argument, nullptr, 'd' },
    { "engine", required_argument, nullptr, 'E' },
    { "auto-name", required_argument, nullptr, 'A' },
    { "help", no_argument, nullptr, 'h' },
    { nullptr, 0, nullptr, 0 },
  };

  while ( true ) {
    const int opt
      = getopt_long( argc,
                     argv,
                     "p:P:i:r:b:m:G:D:a:F:S:M:s:L:c:C:t:j:T:n:J:d:E:B:A:wgh",
                     long_options,
                     nullptr );

    if ( opt == -1 ) {
      break;
    }

    switch ( opt ) {
      // clang-format off
      case 'p': listen_port = stoi(optarg); break;
      case 'P': client_port = stoi(optarg); break;
      case 'i': public_ip = optarg; break;
      case 'r': region = optarg; break;
      case 'b': storage_backend_uri = optarg; break;
      case 'm': max_workers = stoi(optarg); break;
      case 'G': ray_generators = stoi(optarg); break;
      case 'a': scheduler_name = optarg; break;
      case 'F': scheduler_file = optarg; break;
      case 'g': collect_debug_logs = true; break;
      case 'w': write_stat_logs = true; break;
      case 'D': logs_directory = optarg; break;
      case 'S': samples_per_pixel = stoi(optarg); break;
      case 'M': max_path_depth = stoi(optarg); break;
      case 's': bagging_delay = milliseconds{stoul(optarg)}; break;
      case 'L': ray_log_rate = stof(optarg); break;
      case 'B': bag_log_rate = stof(optarg); break;
      case 't': timeout = stoul(optarg); break;
      case 'j': job_summary_path = optarg; break;
      case 'n': new_tile_threshold = stoull(optarg); break;
      case 'I': PbrtOptions.directionalTreelets = true; break;
      case 'J': max_jobs_on_engine = stoul(optarg); break;
      case 'd': memcached_servers.emplace_back(optarg); break;
      case 'E': engines.emplace_back(optarg, max_jobs_on_engine); break;
      case 'A': auto_name_log_dir_tag = optarg; break;
      case 'h': usage(argv[0], EXIT_SUCCESS); break;
      case 'C': alt_scene_file = optarg; break;
        // clang-format on

      case 'T': {
        if ( strcmp( optarg, "auto" ) == 0 ) {
          tile_size = numeric_limits<typeof( tile_size )>::max();
          pixels_per_tile = numeric_limits<typeof( pixels_per_tile )>::max();
        } else {
          pixels_per_tile = stoul( optarg );
          tile_size = ceil( sqrt( pixels_per_tile ) );
        }
        break;
      }

      case 'c':
        crop_window = parse_crop_window_optarg( optarg );

        if ( not crop_window.has_value() ) {
          cerr << "Error: bad crop window (" << optarg << ")." << endl;
          usage( argv[0], EXIT_FAILURE );
        }

        break;

      default:
        usage( argv[0], EXIT_FAILURE );
        break;
    }
  }

  TempFile static_file { "/tmp/r2t2-lambda-master.STATIC0" };

  if ( scheduler_name == "uniform" ) {
    scheduler = make_unique<UniformScheduler>();
  } else if ( scheduler_name == "static" || scheduler_name == "adaptive" ) {
    if ( not scheduler_file ) {
      auto storage = StorageBackend::create_backend( storage_backend_uri );

      cout << "\u2198 Downloading static assignment file to "
           << static_file.name() << "... ";

      storage->get( { { scene::GetObjectName( ObjectType::StaticAssignment, 0 ),
                        static_file.name() } } );
      cout << "done." << endl;

      if ( scheduler_name == "static" ) {
        scheduler = make_unique<StaticScheduler>( static_file.name() );
      } else {
        scheduler = make_unique<AdaptiveScheduler>( static_file.name() );
      }
    } else {
      cout << "\u2192 Using " << ( *scheduler_file )
           << " as static assignment file." << endl;

      if ( scheduler_name == "static" ) {
        scheduler = make_unique<StaticScheduler>( *scheduler_file );
      } else {
        scheduler = make_unique<AdaptiveScheduler>( *scheduler_file );
      }
    }
  } else if ( scheduler_name == "dynamic" ) {
    scheduler = make_unique<DynamicScheduler>();
  } else if ( scheduler_name == "rootonly" ) {
    scheduler = make_unique<RootOnlyScheduler>();
  } else if ( scheduler_name == "null" ) {
    scheduler = make_unique<NullScheduler>();
  } else {
    usage( argv[0], EXIT_FAILURE );
  }

  if ( scheduler == nullptr || listen_port == 0 || max_workers <= 0
       || ray_generators < 0 || samples_per_pixel < 0 || max_path_depth < 0
       || bagging_delay <= 0s || ray_log_rate < 0 || ray_log_rate > 1.0
       || bag_log_rate < 0 || bag_log_rate > 1.0 || public_ip.empty()
       || storage_backend_uri.empty() || region.empty()
       || new_tile_threshold == 0
       || ( crop_window.has_value() && pixels_per_tile != 0
            && pixels_per_tile
                 != numeric_limits<typeof( pixels_per_tile )>::max()
            && pixels_per_tile
                 > static_cast<uint32_t>( crop_window->Area() ) ) ) {
    usage( argv[0], 2 );
  }

  ostringstream public_address;
  public_address << public_ip << ":" << listen_port;

  unique_ptr<LambdaMaster> master;

  // TODO clean this up
  MasterConfiguration config = { samples_per_pixel, max_path_depth,
                                 bagging_delay,     collect_debug_logs,
                                 write_stat_logs,   ray_log_rate,
                                 bag_log_rate,      auto_name_log_dir_tag,
                                 logs_directory,    crop_window,
                                 tile_size,         seconds { timeout },
                                 job_summary_path,  new_tile_threshold,
                                 alt_scene_file,    move( memcached_servers ),
                                 move( engines ) };

  try {
    master = make_unique<LambdaMaster>( listen_port,
                                        client_port,
                                        max_workers,
                                        ray_generators,
                                        public_address.str(),
                                        storage_backend_uri,
                                        region,
                                        move( scheduler ),
                                        config );

    master->run();
  } catch ( const exception& e ) {
    print_exception( argv[0], e );
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
