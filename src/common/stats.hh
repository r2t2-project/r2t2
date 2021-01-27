#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "lambda.hh"

namespace r2t2 {

/* timing utility functions */
using timepoint_t = std::chrono::time_point<std::chrono::system_clock>;
inline timepoint_t now()
{
  return std::chrono::system_clock::now();
}

struct TreeletStats
{
  struct
  {
    uint64_t rays { 0 };
    uint64_t bytes { 0 };
    uint64_t count { 0 };
  } enqueued {}, dequeued {};

  uint64_t enqueue_rate { 0 }; // EWMA of bytes enqueued per second
  uint64_t dequeue_rate { 0 }; // EWMA of bytes dequeued per second
  double cpu_usage { 0 };      // EWMA of this treelet's workers CPU usages

  TreeletStats operator-( const TreeletStats& other ) const;
};

struct WorkerStats
{
  uint64_t finished_paths { 0 };
  double cpu_usage { 0.0 };
  uint64_t bytes_downloaded { 0 };

  struct
  {
    uint64_t rays { 0 };
    uint64_t bytes { 0 };
    uint64_t count { 0 };
  } enqueued {}, assigned {}, dequeued {}, samples {};

  WorkerStats operator-( const WorkerStats& other ) const;
};

} // namespace r2t2
