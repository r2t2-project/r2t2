#include "stats.hh"

#include <cmath>
#include <cstring>
#include <iomanip>

using namespace std;
using namespace chrono;

namespace r2t2 {

TreeletStats TreeletStats::operator-( const TreeletStats& other ) const
{
  TreeletStats res;

  res.enqueue_rate = enqueue_rate;
  res.dequeue_rate = dequeue_rate;
  res.cpu_usage = cpu_usage;

  res.enqueued.rays = enqueued.rays - other.enqueued.rays;
  res.dequeued.rays = dequeued.rays - other.dequeued.rays;
  res.enqueued.bytes = enqueued.bytes - other.enqueued.bytes;
  res.dequeued.bytes = dequeued.bytes - other.dequeued.bytes;
  res.enqueued.count = enqueued.count - other.enqueued.count;
  res.dequeued.count = dequeued.count - other.dequeued.count;

  return res;
}

WorkerStats WorkerStats::operator-( const WorkerStats& other ) const
{
  WorkerStats res;

  res.bytes_downloaded = bytes_downloaded;
  res.finished_paths = finished_paths - other.finished_paths;
  res.cpu_usage = cpu_usage;
  res.enqueued.rays = enqueued.rays - other.enqueued.rays;
  res.assigned.rays = assigned.rays - other.assigned.rays;
  res.dequeued.rays = dequeued.rays - other.dequeued.rays;
  res.samples.rays = samples.rays - other.samples.rays;
  res.enqueued.bytes = enqueued.bytes - other.enqueued.bytes;
  res.assigned.bytes = assigned.bytes - other.assigned.bytes;
  res.dequeued.bytes = dequeued.bytes - other.dequeued.bytes;
  res.samples.bytes = samples.bytes - other.samples.bytes;
  res.enqueued.count = enqueued.count - other.enqueued.count;
  res.assigned.count = assigned.count - other.assigned.count;
  res.dequeued.count = dequeued.count - other.dequeued.count;
  res.samples.count = samples.count - other.samples.count;

  return res;
}

} // namespace r2t2
