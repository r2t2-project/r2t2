#include <cstdlib>
#include <iomanip>

#include "lambda-master.hh"
#include "messages/utils.hh"

using namespace std;
using namespace chrono;
using namespace r2t2;

static const bool R2T2_POWERLINE = ( getenv( "R2T2_POWERLINE" ) != nullptr );

void LambdaMaster::record_enqueue( const WorkerId worker_id,
                                   const RayBagInfo& info )
{
  auto& worker = workers.at( worker_id );
  worker.rays.enqueued += info.ray_count;

  treelets[info.treelet_id].last_stats.first = true;

  if ( info.sample_bag ) {
    worker.stats.samples.rays += info.ray_count;
    worker.stats.samples.bytes += info.bag_size;
    worker.stats.samples.count++;

    aggregated_stats.samples.rays += info.ray_count;
    aggregated_stats.samples.bytes += info.bag_size;
    aggregated_stats.samples.count++;

    last_finished_ray = steady_clock::now();
  } else {
    worker.stats.enqueued.rays += info.ray_count;
    worker.stats.enqueued.bytes += info.bag_size;
    worker.stats.enqueued.count++;

    treelet_stats[info.treelet_id].enqueued.rays += info.ray_count;
    treelet_stats[info.treelet_id].enqueued.bytes += info.bag_size;
    treelet_stats[info.treelet_id].enqueued.count++;

    aggregated_stats.enqueued.rays += info.ray_count;
    aggregated_stats.enqueued.bytes += info.bag_size;
    aggregated_stats.enqueued.count++;
  }
}

void LambdaMaster::record_assign( const WorkerId worker_id,
                                  const RayBagInfo& info )
{
  auto& worker = workers.at( worker_id );
  worker.rays.dequeued += info.ray_count;

  worker.outstanding_ray_bags.insert( info );
  worker.outstanding_bytes += info.bag_size;

  worker.stats.assigned.rays += info.ray_count;
  worker.stats.assigned.bytes += info.bag_size;
  worker.stats.assigned.count++;

  aggregated_stats.assigned.rays += info.ray_count;
  aggregated_stats.assigned.bytes += info.bag_size;
  aggregated_stats.assigned.count++;
}

void LambdaMaster::record_dequeue( const WorkerId worker_id,
                                   const RayBagInfo& info )
{
  auto& worker = workers.at( worker_id );

  worker.outstanding_ray_bags.erase( info );
  worker.outstanding_bytes -= info.bag_size;

  treelets[info.treelet_id].last_stats.first = true;

  worker.stats.dequeued.rays += info.ray_count;
  worker.stats.dequeued.bytes += info.bag_size;
  worker.stats.dequeued.count++;

  treelet_stats[info.treelet_id].dequeued.rays += info.ray_count;
  treelet_stats[info.treelet_id].dequeued.bytes += info.bag_size;
  treelet_stats[info.treelet_id].dequeued.count++;

  aggregated_stats.dequeued.rays += info.ray_count;
  aggregated_stats.dequeued.bytes += info.bag_size;
  aggregated_stats.dequeued.count++;
}

void LambdaMaster::handle_worker_stats()
{
  worker_stats_write_timer.read_event();

  const auto t
    = duration_cast<milliseconds>( steady_clock::now() - start_time );

  const double T = ( t - last_workers_logged ).count() / 1e3;
  last_workers_logged = t;

  constexpr double ALPHA = 2.0 / ( 7 + 1 );

  for ( size_t treelet_id = 0; treelet_id < treelets.size(); treelet_id++ ) {
    auto& treelet = treelets[treelet_id];
    auto& stats = treelet_stats[treelet_id];

    if ( !treelet.last_stats.first ) {
      stats.enqueue_rate *= ( 1 - ALPHA );
      stats.dequeue_rate *= ( 1 - ALPHA );

      continue; /* nothing new to log */
    }

    const TreeletStats diff = stats - treelet.last_stats.second;

    treelet.last_stats.second = stats;
    treelet.last_stats.first = false;

    stats.enqueue_rate
      = ( 1 - ALPHA ) * stats.enqueue_rate + ALPHA * diff.enqueued.bytes;

    stats.dequeue_rate
      = ( 1 - ALPHA ) * stats.dequeue_rate + ALPHA * diff.dequeued.bytes;

    if ( config.write_stat_logs ) {
      /* timestamp,treeletId,raysEnqueued,raysDequeued,bytesEnqueued,
         bytesDequeued,bagsEnqueued,bagsDequeued,enqueueRate,dequeueRate,
         cpuUsage */
      tl_stream << t.count() << ',' << treelet_id << ',' << fixed
                << diff.enqueued.rays << ',' << diff.dequeued.rays << ','
                << diff.enqueued.bytes << ',' << diff.dequeued.bytes << ','
                << diff.enqueued.count << ',' << diff.dequeued.count << ','
                << stats.enqueue_rate << ',' << stats.dequeue_rate << ','
                << fixed << setprecision( 2 ) << ( 100 * stats.cpu_usage )
                << '\n';
    }
  }

  if ( not config.write_stat_logs ) {
    return;
  }

  for ( Worker& worker : workers ) {
    if ( !worker.is_logged )
      continue;

    if ( worker.state == Worker::State::Terminated )
      worker.is_logged = false;

    const auto diff = worker.stats - worker.last_stats;
    worker.last_stats = worker.stats;

    /* timestamp,workerId,pathsFinished,raysEnqueued,raysAssigned,raysDequeued,
       bytesEnqueued,bytesAssigned,bytesDequeued,bagsEnqueued,bagsAssigned,
       bagsDequeued,numSamples,bytesSamples,bagsSamples,cpuUsage,bytesDownloaded
     */

    ws_stream << t.count() << ',' << worker.id << ',' << fixed
              << diff.finished_paths << ',' << diff.enqueued.rays << ','
              << diff.assigned.rays << ',' << diff.dequeued.rays << ','
              << diff.enqueued.bytes << ',' << diff.assigned.bytes << ','
              << diff.dequeued.bytes << ',' << diff.enqueued.count << ','
              << diff.assigned.count << ',' << diff.dequeued.count << ','
              << diff.samples.rays << ',' << diff.samples.bytes << ','
              << diff.samples.count << ',' << fixed << setprecision( 2 )
              << ( 100 * diff.cpu_usage ) << ',' << diff.bytes_downloaded
              << '\n';

    estimated_cost += T;
  }
}

protobuf::JobSummary LambdaMaster::get_job_summary() const
{
  protobuf::JobSummary proto;

  double generation_time
    = duration_cast<milliseconds>( last_generator_done - start_time ).count()
      / 1000.0;

  generation_time = ( generation_time < 0 ) ? 0 : generation_time;

  double initialization_time
    = duration_cast<milliseconds>( scene_initialization_done
                                   - last_generator_done )
        .count()
      / 1000.0;

  initialization_time = ( initialization_time < 0 ) ? 0 : initialization_time;

  double ray_time = duration_cast<milliseconds>( last_finished_ray
                                                 - scene_initialization_done )
                      .count()
                    / 1000.0;

  ray_time = ( ray_time < 0 ) ? 0 : ray_time;

  const double total_time = ray_time + initialization_time + generation_time;

  const double avg_ray_throughput
    = ( total_time > 0 )
        ? ( 10 * aggregated_stats.samples.rays / max_workers / total_time )
        : 0;

  // FIXME cost estimation
  /* constexpr static double LAMBDA_UNIT_COST = 0.00004897; // $/lambda/sec
  const double estimated_cost
    = LAMBDA_UNIT_COST * max_workers * ceil( total_time ); */

  proto.set_job_id( job_id );
  proto.set_num_lambdas( max_workers );
  proto.set_num_generators( ray_generators );
  proto.set_treelet_count( scene.base.GetTreeletCount() );
  proto.mutable_output_size()->set_x( scene.sample_extent.x );
  proto.mutable_output_size()->set_y( scene.sample_extent.y );
  proto.set_spp( scene.base.samplesPerPixel );
  proto.mutable_tile_size()->set_x( tiles.tile_size );
  proto.mutable_tile_size()->set_y( tiles.tile_size );
  proto.set_max_depth( config.max_path_depth );
  proto.set_bagging_delay( config.bagging_delay.count() );
  proto.set_memcached_servers( config.memcached_servers.size() );
  proto.set_storage_backend( storage_backend_uri );

  for ( const auto& [k, v] : alternative_object_names ) {
    ( *proto.mutable_alt_scene_objects() )[pbrt::scene::GetObjectName( k, 0 )]
      = v;
  }

  proto.set_total_time( total_time );
  proto.set_generation_time( generation_time );
  proto.set_initialization_time( initialization_time );
  proto.set_tracing_time( ray_time );
  proto.set_total_paths( scene.total_paths );
  proto.set_finished_paths( aggregated_stats.finished_paths );
  proto.set_finished_rays( aggregated_stats.samples.rays );
  proto.set_num_enqueues( aggregated_stats.enqueued.rays );
  proto.set_ray_throughput( avg_ray_throughput );
  proto.set_total_upload( aggregated_stats.enqueued.bytes );
  proto.set_total_download( aggregated_stats.dequeued.bytes );
  proto.set_total_samples( aggregated_stats.samples.bytes );
  proto.set_estimated_cost( estimated_cost );

  *proto.mutable_pbrt_stats() = to_protobuf( pbrt_stats );

  return proto;
}

void LambdaMaster::dump_job_summary( const string& path ) const
{
  protobuf::JobSummary proto = get_job_summary();
  ofstream fout { path };
  fout << protoutil::to_json( proto ) << endl;
}

template<class T>
class Value
{
private:
  T value;

public:
  Value( T v )
    : value( v )
  {}
  T get() const { return value; }
};

template<class T>
ostream& operator<<( ostream& o, const Value<T>& v )
{
  o << "\x1B[1m" << v.get() << "\x1B[0m";
  return o;
}

pair<string_view, string_view> get_category_and_title( const string_view s )
{
  auto pos = s.find( '/' );

  if ( pos == string::npos ) {
    return { {}, s };
  }

  return { s.substr( 0, pos ), s.substr( pos + 1 ) };
}

void LambdaMaster::print_pbrt_stats() const
{
  map<string_view, vector<string>> to_print;

  constexpr size_t FIELD_WIDTH = 25;

  for ( auto& [k, v] : pbrt_stats.counters ) {
    if ( !v )
      continue;

    auto [category, title] = get_category_and_title( k );

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << Value<int64_t>( v );
    to_print[category].push_back( oss.str() );
  }

  for ( auto& [k, v] : pbrt_stats.memoryCounters ) {
    if ( !v )
      continue;

    auto [category, title] = get_category_and_title( k );

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << Value<string>( format_bytes( v ) );
    to_print[category].push_back( oss.str() );
  }

  for ( auto& [k, count] : pbrt_stats.intDistributionCounts ) {
    if ( !count )
      continue;

    const auto sum = pbrt_stats.intDistributionSums.at( k );
    const auto minimum = pbrt_stats.intDistributionMins.at( k );
    const auto maximum = pbrt_stats.intDistributionMaxs.at( k );

    auto [category, title] = get_category_and_title( k );

    const double average
      = static_cast<double>( sum ) / static_cast<double>( count );

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << fixed << setprecision( 3 ) << Value<double>( average )
        << " avg [range " << minimum << " - " << maximum << "]";
    to_print[category].push_back( oss.str() );
  }

  for ( auto& [k, count] : pbrt_stats.floatDistributionCounts ) {
    if ( !count )
      continue;

    const auto sum = pbrt_stats.floatDistributionSums.at( k );
    const auto minimum = pbrt_stats.floatDistributionMins.at( k );
    const auto maximum = pbrt_stats.floatDistributionMaxs.at( k );

    auto [category, title] = get_category_and_title( k );

    const double average
      = static_cast<double>( sum ) / static_cast<double>( count );

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << fixed << setprecision( 3 ) << Value<double>( average )
        << " avg [range " << minimum << " - " << maximum << "]";
    to_print[category].push_back( oss.str() );
  }

  for ( auto& [k, v] : pbrt_stats.percentages ) {
    if ( !v.second )
      continue;

    auto [category, title] = get_category_and_title( k );

    const double percentage = 100.0 * v.first / v.second;

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << fixed << setprecision( 2 ) << Value<double>( percentage ) << "% ["
        << v.first << " / " << v.second << "]";
    to_print[category].push_back( oss.str() );
  }

  for ( auto& [k, v] : pbrt_stats.ratios ) {
    if ( !v.second )
      continue;

    auto [category, title] = get_category_and_title( k );

    const double ratio
      = static_cast<double>( v.first ) / static_cast<double>( v.second );

    ostringstream oss;
    oss << setw( FIELD_WIDTH - 4 ) << left << title.substr( 0, FIELD_WIDTH - 6 )
        << fixed << setprecision( 2 ) << Value<double>( ratio ) << "x ["
        << v.first << " / " << v.second << "]";
    to_print[category].push_back( oss.str() );
  }

  cout << "PBRT statistics:" << endl;

  for ( const auto& [category, items] : to_print ) {
    cout << "  " << ( category.empty() ? "No category"s : category ) << endl;

    for ( const auto& item : items ) {
      cout << "    " << item << endl;
    }
  }

  cout << endl;
}

void LambdaMaster::print_job_summary() const
{
  auto percent = []( const uint64_t n, const uint64_t total ) -> double {
    return total ? ( ( static_cast<uint64_t>( 100 * ( 100.0 * n / total ) ) )
                     / 100.0 )
                 : 0.0;
  };

  auto box_start = []( const string& color ) {
    if ( R2T2_POWERLINE ) {
      return "\x1B[38;5;" + color + "m\ue0b6\x1B[0m\x1B[1;48;5;" + color + "m";
    } else {
      return "\x1B[48;5;" + color + "m ";
    }
  };

  auto box_end = []( const string& color ) {
    if ( R2T2_POWERLINE ) {
      return "\x1B[0m\x1B[38;5;" + color + "m\ue0b4\x1B[0m";
    } else {
      return " \x1B[0m"s;
    }
  };

  auto print_title = []( const string& title ) {
    constexpr size_t FIELD_WIDTH = 25;
    cout << "  " << setw( FIELD_WIDTH - 2 ) << left
         << title.substr( 0, FIELD_WIDTH - 4 );
  };

  const protobuf::JobSummary proto = get_job_summary();

  const float completion_percent
    = percent( proto.finished_paths(), proto.total_paths() );

  // color (red | green)
  const string c = ( completion_percent >= 100.0 ) ? "028" : "9";

  cout << "Job summary:" << endl;
  print_title( "Ray throughput" );
  cout << fixed << setprecision( 2 ) << Value<double>( proto.ray_throughput() )
       << " rays/worker/s" << endl;

  print_title( "Total paths" );
  cout << Value<uint64_t>( proto.total_paths() ) << endl;

  print_title( "Finished paths" );
  cout << Value<uint64_t>( proto.finished_paths() ) << " " << box_start( c )
       << fixed << setprecision( 2 ) << completion_percent << "%"
       << box_end( c ) << endl;

  print_title( "Finished rays" );
  cout << Value<uint64_t>( proto.finished_rays() ) << endl;

  print_title( "Total transfers" );
  cout << Value<uint64_t>( proto.num_enqueues() );

  if ( aggregated_stats.samples.rays > 0 ) {
    cout << " (" << fixed << setprecision( 2 )
         << ( 1.0 * proto.num_enqueues() / proto.finished_rays() )
         << " transfers/ray)";
  }

  cout << endl;

  print_title( "Total upload" );
  cout << Value<string>( format_bytes( proto.total_upload() ) ) << endl;

  print_title( "Total download" );
  cout << Value<string>( format_bytes( proto.total_download() ) ) << endl;

  print_title( "Total sample size" );
  cout << Value<string>( format_bytes( proto.total_samples() ) ) << endl;

  print_title( "Total time" );
  cout << fixed << setprecision( 2 ) << Value<double>( proto.total_time() )
       << " seconds" << endl;

  print_title( "  Camera rays" );
  cout << Value<double>( proto.generation_time() ) << " seconds" << endl;

  print_title( "  Initialization" );
  cout << Value<double>( proto.initialization_time() ) << " seconds" << endl;

  print_title( "  Ray tracing" );
  cout << Value<double>( proto.tracing_time() ) << " seconds" << endl;

  print_title( "Estimated CPU-seconds" );
  cout << Value<double>( proto.estimated_cost() ) << endl;
}
