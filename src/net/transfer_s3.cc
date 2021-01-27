#include "transfer_s3.hh"

#include "net/http_response_parser.hh"

using namespace std;
using namespace chrono;

const static std::string UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

S3TransferAgent::S3Config::S3Config( const S3StorageBackend& backend )
{
  credentials = backend.client().credentials();
  region = backend.client().config().region;
  bucket = backend.bucket();
  prefix = backend.prefix();
  endpoint = S3::endpoint( region, bucket );
  address.store( Address { endpoint, "http" } );
}

S3TransferAgent::S3TransferAgent( const S3StorageBackend& backend,
                                  const size_t thread_count /* NOT USED? */,
                                  const bool upload_as_public,
                                  std::atomic<uint64_t>* byte_counter )
  : TransferAgent()
  , _client_config( backend )
  , _upload_as_public( upload_as_public )
  , _byte_counter( byte_counter )
{
  _thread_count = thread_count;

  if ( _thread_count == 0 ) {
    throw runtime_error( "thread count cannot be zero" );
  }

  for ( size_t i = 0; i < _thread_count; i++ ) {
    _threads.emplace_back( &S3TransferAgent::worker_thread, this, i );
  }
}

S3TransferAgent::~S3TransferAgent()
{
  {
    unique_lock<mutex> lock { _outstanding_mutex };
    _outstanding.emplace( _next_id++, Task::Terminate, "", "" );
  }

  _cv.notify_all();
  for ( auto& t : _threads )
    t.join();
}

HTTPRequest S3TransferAgent::get_request( const Action& action )
{
  switch ( action.task ) {
    case Task::Upload:
      return S3PutRequest( _client_config.credentials,
                           _client_config.endpoint,
                           _client_config.region,
                           action.key,
                           action.data,
                           UNSIGNED_PAYLOAD,
                           _upload_as_public )
        .to_http_request();

    case Task::Download:
      return S3GetRequest( _client_config.credentials,
                           _client_config.endpoint,
                           _client_config.region,
                           action.key )
        .to_http_request();

    default:
      throw runtime_error( "Unknown action task" );
  }
}

#define TRY_OPERATION( x, y )                                                  \
  try {                                                                        \
    x;                                                                         \
  } catch ( exception & ex ) {                                                 \
    try_count++;                                                               \
    connection_okay = false;                                                   \
    s3.close();                                                                \
    y;                                                                         \
  }

void S3TransferAgent::worker_thread( const size_t thread_id )
{
  constexpr milliseconds backoff { 50 };
  size_t try_count = 0;

  deque<Action> actions;

  auto last_addr_update = steady_clock::now() + seconds { thread_id };
  Address s3_address = _client_config.address.load();

  while ( true ) {
    TCPSocket s3;
    auto parser = make_unique<HTTPResponseParser>();
    bool connection_okay = true;
    size_t request_count = 0;

    if ( try_count > 0 ) {
      try_count = min<size_t>( try_count, 7u ); // caps at 3.2s
      this_thread::sleep_for( backoff * ( 1 << ( try_count - 1 ) ) );
    }

    if ( steady_clock::now() - last_addr_update >= ADDR_UPDATE_INTERVAL ) {
      s3_address = _client_config.address.load();
      last_addr_update = steady_clock::now();
    }

    TRY_OPERATION( s3.connect( s3_address ), continue );

    while ( connection_okay ) {
      /* make sure we have an action to perfom */
      if ( actions.empty() ) {
        unique_lock<mutex> lock { _outstanding_mutex };

        _cv.wait( lock, [this]() { return !_outstanding.empty(); } );

        if ( _outstanding.front().task == Task::Terminate )
          return;

        const auto capacity = MAX_REQUESTS_ON_CONNECTION - request_count;

        do {
          actions.push_back( move( _outstanding.front() ) );
          _outstanding.pop();
        } while ( !_outstanding.empty()
                  && _outstanding.size() / _thread_count >= 1
                  && actions.size() < capacity );
      }

      for ( const auto& action : actions ) {
        string headers;
        HTTPRequest request = get_request( action );
        parser->new_request_arrived( request );
        request.serialize_headers( headers );
        TRY_OPERATION( s3.write_all( headers ), break );

        if ( not request.body().empty() ) {
          TRY_OPERATION( s3.write_all( request.body() ), break );
        }

        request_count++;
      }

      char buffer[1024 * 1024];
      simple_string_span buffer_span { buffer, sizeof( buffer ) };

      while ( connection_okay && !actions.empty() ) {
        size_t read_count;
        TRY_OPERATION( read_count = s3.read( buffer_span ), break );

        if ( read_count == 0 ) {
          // connection was closed by the other side
          try_count++;
          connection_okay = false;
          s3.close();
          break;
        }

        if ( _byte_counter != nullptr ) {
          _byte_counter->operator+=( read_count );
        }

        parser->parse( buffer_span.substr( 0, read_count ) );

        while ( !parser->empty() ) {
          const string_view status = parser->front().status_code();
          const string data = move( parser->front().body() );

          switch ( status[0] ) {
            case '2': // successful
            {
              unique_lock<mutex> lock { _results_mutex };
              _results.emplace( actions.front().id, move( data ) );
            }

              try_count = 0;
              actions.pop_front();
              _event_fd.write_event();
              break;

            case '5': // we need to slow down
              connection_okay = false;
              try_count++;
              break;

            default: // unexpected response, like 404 or something
              throw runtime_error( "s3 transfer failed: "
                                   + string( parser->front().status_code() ) );
          }

          parser->pop();
        }
      }

      if ( request_count >= MAX_REQUESTS_ON_CONNECTION ) {
        connection_okay = false;
      }
    }
  }
}

void S3TransferAgent::do_action( Action&& action )
{
  if ( steady_clock::now() - _last_addr_update >= ADDR_UPDATE_INTERVAL ) {
    _client_config.address.store( Address { _client_config.endpoint, "http" } );
    _last_addr_update = steady_clock::now();
  }

  TransferAgent::do_action( move( action ) );
}
