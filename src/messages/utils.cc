#include "utils.hh"

#include <chrono>
#include <memory>
#include <stdexcept>

#include "util/util.hh"

using namespace std;

namespace r2t2 {

protobuf::SceneObject to_protobuf( const SceneObject& object )
{
  protobuf::SceneObject proto;
  proto.set_type( to_underlying( object.key.type ) );
  proto.set_id( object.key.id );
  proto.set_alt_name( object.alt_name );
  return proto;
}

protobuf::RayBagInfo to_protobuf( const RayBagInfo& info )
{
  protobuf::RayBagInfo proto;
  proto.set_tracked( info.tracked );
  proto.set_worker_id( info.worker_id );
  proto.set_treelet_id( info.treelet_id );
  proto.set_bag_id( info.bag_id );
  proto.set_ray_count( info.ray_count );
  proto.set_bag_size( info.bag_size );
  proto.set_sample_bag( info.sample_bag );
  return proto;
}

protobuf::WorkerStats to_protobuf( const WorkerStats& stats )
{
  protobuf::WorkerStats proto;
  proto.set_finished_paths( stats.finished_paths );
  proto.set_cpu_usage( stats.cpu_usage );
  proto.set_bytes_downloaded( stats.bytes_downloaded );
  return proto;
}

protobuf::AccumulatedStats to_protobuf( const pbrt::AccumulatedStats& stats )
{
  protobuf::AccumulatedStats proto;

  auto set_item = []( auto& proto_item, const auto& title, const auto& value ) {
    proto_item.set_title( title );
    proto_item.set_value( value );
  };

  auto set_pair
    = []( auto& proto_item, const auto& t, const auto& a, const auto& b ) {
        proto_item.set_title( t );
        proto_item.set_first( a );
        proto_item.set_second( b );
      };

  for ( auto& [k, v] : stats.counters ) {
    if ( v ) {
      set_item( *proto.add_counters(), k, v );
    }
  }

  for ( auto& [k, v] : stats.memoryCounters ) {
    if ( v ) {
      set_item( *proto.add_memory_counters(), k, v );
    }
  }

  for ( auto& [k, v] : stats.intDistributionCounts ) {
    if ( v ) {
      const auto count = v;
      const auto min = stats.intDistributionMins.at( k );
      const auto max = stats.intDistributionMaxs.at( k );
      const auto sum = stats.intDistributionSums.at( k );

      set_item( *proto.add_int_distribution_sums(), k, sum );
      set_item( *proto.add_int_distribution_counts(), k, count );
      set_item( *proto.add_int_distribution_mins(), k, min );
      set_item( *proto.add_int_distribution_maxs(), k, max );
    }
  }

  for ( auto& [k, v] : stats.floatDistributionCounts ) {
    if ( v ) {
      const auto count = v;
      const auto min = stats.floatDistributionMins.at( k );
      const auto max = stats.floatDistributionMaxs.at( k );
      const auto sum = stats.floatDistributionSums.at( k );

      set_item( *proto.add_float_distribution_sums(), k, sum );
      set_item( *proto.add_float_distribution_counts(), k, count );
      set_item( *proto.add_float_distribution_mins(), k, min );
      set_item( *proto.add_float_distribution_maxs(), k, max );
    }
  }

  for ( auto& [k, v] : stats.percentages ) {
    if ( v.second ) {
      set_pair( *proto.add_percentages(), k, v.first, v.second );
    }
  }

  for ( auto& [k, v] : stats.ratios ) {
    if ( v.second ) {
      set_pair( *proto.add_ratios(), k, v.first, v.second );
    }
  }

  return proto;
}

SceneObject from_protobuf( const protobuf::SceneObject& object )
{
  return { { static_cast<pbrt::ObjectType>( object.type() ), object.id() },
           object.alt_name() };
}

RayBagInfo from_protobuf( const protobuf::RayBagInfo& proto )
{
  RayBagInfo res { proto.worker_id(), proto.treelet_id(), proto.bag_id(),
                   proto.ray_count(), proto.bag_size(),   proto.sample_bag() };

  res.tracked = proto.tracked();
  return res;
}

WorkerStats from_protobuf( const protobuf::WorkerStats& proto )
{
  return { proto.finished_paths(), proto.cpu_usage(), proto.bytes_downloaded() };
}

pbrt::AccumulatedStats from_protobuf( const protobuf::AccumulatedStats& proto )
{
  pbrt::AccumulatedStats obj;

  for ( auto& item : proto.counters() ) {
    obj.counters[item.title()] += item.value();
  }

  for ( auto& item : proto.memory_counters() ) {
    obj.memoryCounters[item.title()] += item.value();
  }

  for ( auto& item : proto.percentages() ) {
    obj.percentages[item.title()].first += item.first();
    obj.percentages[item.title()].second += item.second();
  }

  for ( auto& item : proto.ratios() ) {
    obj.ratios[item.title()].first += item.first();
    obj.ratios[item.title()].second += item.second();
  }

  for ( auto& item : proto.int_distribution_counts() ) {
    obj.intDistributionCounts[item.title()] += item.value();
    obj.intDistributionMins[item.title()] = numeric_limits<int64_t>::max();
    obj.intDistributionMaxs[item.title()] = numeric_limits<int64_t>::lowest();
    obj.intDistributionSums[item.title()] = 0;
  }

  for ( auto& item : proto.int_distribution_maxs() ) {
    obj.intDistributionMaxs[item.title()]
      = max( obj.intDistributionMaxs[item.title()], item.value() );
  }

  for ( auto& item : proto.int_distribution_mins() ) {
    obj.intDistributionMins[item.title()]
      = min( obj.intDistributionMins[item.title()], item.value() );
  }

  for ( auto& item : proto.int_distribution_sums() ) {
    obj.intDistributionSums[item.title()] += item.value();
  }

  for ( auto& item : proto.float_distribution_counts() ) {
    obj.floatDistributionCounts[item.title()] += item.value();
    obj.floatDistributionMins[item.title()] = numeric_limits<double>::max();
    obj.floatDistributionMaxs[item.title()] = numeric_limits<double>::lowest();
    obj.floatDistributionSums[item.title()] = 0;
  }

  for ( auto& item : proto.float_distribution_maxs() ) {
    obj.floatDistributionMaxs[item.title()]
      = max( obj.floatDistributionMaxs[item.title()], item.value() );
  }

  for ( auto& item : proto.float_distribution_mins() ) {
    obj.floatDistributionMins[item.title()]
      = min( obj.floatDistributionMins[item.title()], item.value() );
  }

  for ( auto& item : proto.float_distribution_sums() ) {
    obj.floatDistributionSums[item.title()] += item.value();
  }

  return obj;
}

} // namespace r2t2
