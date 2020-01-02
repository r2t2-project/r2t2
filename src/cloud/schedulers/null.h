#ifndef PBRT_CLOUD_SCHEDULERS_NULL_H
#define PBRT_CLOUD_SCHEDULERS_NULL_H

#include "cloud/scheduler.h"

namespace pbrt {

class NullScheduler : public Scheduler {
  public:
    Optional<Schedule> schedule(
        const size_t maxWorkers,
        const std::vector<TreeletStats> &treelets) override {
        return {false};
    }
};

}  // namespace pbrt
#endif /* PBRT_CLOUD_SCHEDULERS_NULL_H */
