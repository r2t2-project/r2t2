#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <stdexcept>

class Timer
{
public:
  static inline uint64_t timestamp_ns()
  {
    static_assert( std::is_same<std::chrono::steady_clock::duration,
                                std::chrono::nanoseconds>::value );

    return std::chrono::steady_clock::now().time_since_epoch().count();
  }

  static std::string pp_ns( const uint64_t duration_ns );

  struct Record
  {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;

    void log( const uint64_t time_ns )
    {
      count++;
      total_ns += time_ns;
      max_ns = std::max( max_ns, time_ns );
    }
  };

  enum class Category
  {
    DNS,
    Nonblock,
    WaitingForEvent,
    LoadingTreelet,
    count
  };

  constexpr static size_t num_categories
    = static_cast<size_t>( Category::count );

  constexpr static std::array<const char*, num_categories> _category_names {
    { "DNS", "Nonblocking operations", "Waiting for event", "Loading treelet" }
  };

private:
  uint64_t _beginning_timestamp = timestamp_ns();
  std::array<Record, num_categories> _records {};
  std::optional<Category> _current_category {};
  uint64_t _start_time {};

public:
  template<Category category>
  void start( const uint64_t now = timestamp_ns() )
  {
    if ( _current_category.has_value() ) {
      throw std::runtime_error( "timer started when already running" );
    }

    _current_category = category;
    _start_time = now;
  }

  template<Category category>
  void stop( const uint64_t now = timestamp_ns() )
  {
    if ( not _current_category.has_value()
         or _current_category.value() != category ) {
      throw std::runtime_error(
        "timer stopped when not running, or with mismatched category" );
    }

    _records[static_cast<size_t>( category )].log( now - _start_time );
    _current_category.reset();
  }

  std::string summary() const;
};

inline Timer& global_timer()
{
  static thread_local Timer the_global_timer;
  return the_global_timer;
}

template<Timer::Category category>
class GlobalScopeTimer
{
public:
  GlobalScopeTimer() { global_timer().start<category>(); }
  ~GlobalScopeTimer() { global_timer().stop<category>(); }
};

template<Timer::Category category>
class RecordScopeTimer
{
  Timer::Record* _timer;
  uint64_t _start_time;

public:
  RecordScopeTimer( Timer::Record& timer )
    : _timer( &timer )
    , _start_time( Timer::timestamp_ns() )
  {
    global_timer().start<category>( _start_time );
  }

  ~RecordScopeTimer()
  {
    const uint64_t now = Timer::timestamp_ns();
    _timer->log( now - _start_time );
    global_timer().stop<category>( now );
  }

  RecordScopeTimer( const RecordScopeTimer& ) = delete;
  RecordScopeTimer& operator=( const RecordScopeTimer& ) = delete;
};
