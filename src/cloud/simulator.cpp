#include <cstdlib>
#include <iostream>
#include <list>
#include <deque>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <random>

#include "cloud/bvh.h"

#include "cloud/manager.h"
#include "cloud/r2t2.h"
#include "core/camera.h"
#include "core/geometry.h"
#include "core/transform.h"
#include "messages/utils.h"
#include "util/exception.h"
#include "cloud/raystate.h"
#include "messages/serialization.h"

using namespace std;
using namespace pbrt;

void usage(const char *argv0) {
    cerr << argv0 << " SCENE-DATA NUM-WORKERS WORKER-BANDWIDTH WORKER-LATENCY REBALANCE-PERIOD SPP PATH-DEPTH INIT-MAPPING" << endl;
    exit(EXIT_FAILURE);
}

vector<shared_ptr<Light>> loadLights() {
    vector<shared_ptr<Light>> lights;
    auto reader = global::manager.GetReader(ObjectType::Lights);

    while (!reader->eof()) {
        protobuf::Light proto_light;
        reader->read(&proto_light);
        lights.push_back(move(light::from_protobuf(proto_light)));
    }

    return lights;
}

shared_ptr<Camera> loadCamera(vector<unique_ptr<Transform>> &transformCache) {
    auto reader = global::manager.GetReader(ObjectType::Camera);
    protobuf::Camera proto_camera;
    reader->read(&proto_camera);
    return camera::from_protobuf(proto_camera, transformCache);
}

shared_ptr<GlobalSampler> loadSampler() {
    auto reader = global::manager.GetReader(ObjectType::Sampler);
    protobuf::Sampler proto_sampler;
    reader->read(&proto_sampler);
    return sampler::from_protobuf(proto_sampler);
}

Scene loadFakeScene() {
    auto reader = global::manager.GetReader(ObjectType::Scene);
    protobuf::Scene proto_scene;
    reader->read(&proto_scene);
    return from_protobuf(proto_scene);
}

struct SimRayMsg {
    RayStatePtr ray;
    uint64_t bytesRemaining;
    uint64_t deliveryDelay;
    uint64_t ackDelay;

    uint64_t srcWorkerID;
    uint64_t dstWorkerID;
};

struct Worker {
    uint64_t id;

    deque<RayStatePtr> inQueue;
    uint64_t outstanding = 0;
};

unordered_map<uint64_t, unordered_set<uint32_t>> loadInitMapping(const string &fname) {
    unordered_map<uint64_t, unordered_set<uint32_t>> workerToTreelets;
    string line;
    ifstream initAllocFile(fname);
    int workerIdx = 0;
    while (getline(initAllocFile, line)) {
        stringstream strm(line);
        string treeletStr;
        while (getline(strm, treeletStr)) {
            workerToTreelets[workerIdx].emplace(stoul(treeletStr));
        }
        workerIdx++;
    }

    return workerToTreelets;
}


class Simulator {
public:
    Simulator(uint64_t numWorkers_, uint64_t workerBandwidth_,
              uint64_t workerLatency_, uint64_t msPerRebalance_,
              uint64_t samplesPerPixel_, uint64_t pathDepth_,
              const string initAllocPath)
        : numWorkers(numWorkers_), workerBandwidth(workerBandwidth_),
          workerLatency(workerLatency_), msPerRebalance(msPerRebalance_),
          samplesPerPixel(samplesPerPixel_), pathDepth(pathDepth_),
          workers(numWorkers), workerToTreelets(loadInitMapping(initAllocPath)),
          transformCache(), sampler(loadSampler()), camera(loadCamera(transformCache)),
          lights(loadLights()), fakeScene(loadFakeScene()),
          sampleBounds(camera->film->GetSampleBounds()),
          sampleExtent(sampleBounds.Diagonal())
    {
        CHECK_EQ(workerToTreelets.size(), numWorkers);
        for (auto &kv : workerToTreelets) {
            for (uint32_t treelet : kv.second) {
                treeletToWorkers[treelet].push_back(kv.first);
            }
        }

        for (uint64_t id = 0; id < numWorkers; id++) {
            workers[id].id = id;
        }

        for (auto &light : lights) {
            light->Preprocess(fakeScene);
        }

        treelets.resize(global::manager.treeletCount());

        /* let's load all the treelets */
        for (size_t i = 0; i < treelets.size(); i++) {
            treelets[i] = make_unique<CloudBVH>(i);
        }

        setTiles();
    }

    void setTiles() {
        tileSize = ceil(sqrt(sampleBounds.Area() / numWorkers));

        const Vector2i extent = sampleBounds.Diagonal();
        const int safeTileLimit = ceil(sqrt(maxRays / samplesPerPixel));
    
        while (ceil(1.0 * extent.x / tileSize) * ceil(1.0 * extent.y / tileSize) >
               numWorkers) {
            tileSize++;
        }
    
        tileSize = min(tileSize, safeTileLimit);
    
        nTiles = Point2i((sampleBounds.Diagonal().x + tileSize - 1) / tileSize,
                        (sampleBounds.Diagonal().y + tileSize - 1) / tileSize);
    }

    bool shouldGenNewRays(const Worker &worker) {
        return worker.inQueue.size() + worker.outstanding < maxRays / 10 &&
            curCameraTile < nCameraTiles.x * nCameraTiles.y;
    }

    Bounds2i nextCameraTile() {
        const int tileX = curCameraTile % nCameraTiles.x;
        const int tileY = curCameraTile / nCameraTiles.x;
        const int x0 = sampleBounds.pMin.x + tileX * tileSize;
        const int x1 = min(x0 + tileSize, sampleBounds.pMax.x);
        const int y0 = sampleBounds.pMin.y + tileY * tileSize;
        const int y1 = min(y0 + tileSize, sampleBounds.pMax.y);
    
        curCameraTile++;
        return Bounds2i(Point2i{x0, y0}, Point2i{x1, y1});
    }

    uint64_t getNextWorker(const RayStatePtr &ray) {
        vector<uint64_t> &treelet_workers = treeletToWorkers.at(ray->CurrentTreelet());
        uniform_int_distribution<> dis(0, treelet_workers.size() - 1);
        return treelet_workers[dis(randgen)];
    }

    uint64_t getNetworkLen(const RayStatePtr &ray) {
        return ray->Serialize(rayBuffer);
    }

    void enqueueRay(Worker &worker, RayStatePtr &&ray) {
        SimRayMsg msg;
        msg.ray = move(ray);
        msg.bytesRemaining = getNetworkLen(msg.ray);
        msg.deliveryDelay = workerLatency;
        msg.ackDelay = workerLatency;
        msg.srcWorkerID = worker.id;
        msg.dstWorkerID = getNextWorker(msg.ray);

        worker.outstanding++;
        inTransit.emplace_back(move(msg));
    }

    void generateRays(Worker &worker) {
        Bounds2i tile = nextCameraTile();
        for (size_t sample = 0; sample < samplesPerPixel; sample++) {
            for (Point2i pixel : tile) {
                if (!InsideExclusive(pixel, sampleBounds)) continue;

                RayStatePtr ray = graphics::GenerateCameraRay(
                    camera, pixel, sample, pathDepth - 1, sampleExtent, sampler);

                enqueueRay(worker, move(ray));
            }
        }
    }

    void transmitRays() {
        vector<uint64_t> remainingIngress(numWorkers, workerBandwidth / 1000);
        vector<uint64_t> remainingEgress(numWorkers, workerBandwidth / 1000);
        auto iter = inTransit.begin();
        while (iter != inTransit.end()) {
            auto nextIter = next(iter);

            auto &msg = *iter;

            if (msg.deliveryDelay > 0) {
                msg.deliveryDelay--;
            } else if (msg.bytesRemaining > 0) {
                const Worker &dstWorker = workers[msg.dstWorkerID];
                if (dstWorker.inQueue.size() + dstWorker.outstanding < maxRays * 2) {
                    uint64_t &srcRemaining = remainingEgress[msg.srcWorkerID];
                    uint64_t &dstRemaining = remainingIngress[msg.dstWorkerID];
                    uint64_t transferBytes = min(msg.bytesRemaining, min(srcRemaining, dstRemaining));
                    srcRemaining -= transferBytes;
                    dstRemaining -= transferBytes;
                    msg.bytesRemaining -= transferBytes;
                }
            } else if (msg.ackDelay > 0) {
                msg.ackDelay--;
            } else { // Delivered
                workers[msg.dstWorkerID].inQueue.push_back(move(msg.ray));
                workers[msg.srcWorkerID].outstanding--;
                inTransit.erase(iter);
            }

            iter = nextIter;
        }
    }

    bool processRays(Worker &worker) {
        bool isWork = false;

        if (shouldGenNewRays(worker)) {
            generateRays(worker);
            isWork = true;
        }

        if (worker.inQueue.size() > 0) {
            isWork = true;

            MemoryArena arena;
            while (worker.inQueue.size() > 0) {
                RayStatePtr origRayPtr = move(worker.inQueue.front());
                worker.inQueue.pop_front();

                deque<RayStatePtr> rays;
                rays.emplace_back(move(origRayPtr));

                while (!rays.empty()) {
                    RayStatePtr rayPtr = move(rays.front());
                    rays.pop_front();

                    const TreeletId rayTreeletId = rayPtr->CurrentTreelet();
                    if (workerToTreelets.at(worker.id).count(rayTreeletId) == 0) {
                        enqueueRay(worker, move(rayPtr));
                        continue;
                    }
                    if (!rayPtr->toVisitEmpty()) {
                        auto newRayPtr = graphics::TraceRay(move(rayPtr),
                                                            *treelets[rayTreeletId]);
                        auto &newRay = *newRayPtr;

                        const bool hit = newRay.HasHit();
                        const bool emptyVisit = newRay.toVisitEmpty();

                        if (newRay.IsShadowRay()) {
                            if (hit || emptyVisit) {
                                newRay.Ld = hit ? 0.f : newRay.Ld;
                                // FIXME handle samples
                                //samples.emplace_back(*newRayPtr);
                            } else {
                                rays.push_back(move(newRayPtr));
                            }
                        } else if (!emptyVisit || hit) {
                            rays.push_back(move(newRayPtr));
                        } else if (emptyVisit) {
                            newRay.Ld = 0.f;
                            // FIXME handle samples
                            //samples.emplace_back(*newRayPtr);
                        }
                    } else if (rayPtr->HasHit()) {
                        RayStatePtr bounceRay, shadowRay;
                        tie(bounceRay, shadowRay) =
                            graphics::ShadeRay(move(rayPtr), *treelets[rayTreeletId],
                                               lights, sampleExtent, sampler, arena);

                        if (bounceRay != nullptr) {
                            rays.push_back(move(bounceRay));
                        }

                        if (shadowRay != nullptr) {
                            rays.push_back(move(shadowRay));
                        }
                    }
                }
            }
        }

        return isWork;
    }

    void simulate() {
        bool isWork = true;

        while (isWork) {
            isWork = false;
            if (curMS % msPerRebalance == 0) {

            }

            transmitRays();

            for (uint64_t workerID = 0; workerID < numWorkers; workerID++) {
                isWork |= processRays(workers[workerID]);
            }

            curMS++;
        }
    }

    void dump_stats() {
    }
private:
    uint64_t numWorkers;
    uint64_t workerBandwidth;
    uint64_t workerLatency;
    uint64_t msPerRebalance;
    uint64_t samplesPerPixel;
    uint64_t pathDepth;

    vector<Worker> workers;

    unordered_map<uint64_t, unordered_set<uint32_t>> workerToTreelets;
    unordered_map<uint32_t, vector<uint64_t>> treeletToWorkers;

    vector<unique_ptr<Transform>> transformCache;
    shared_ptr<GlobalSampler> sampler;
    shared_ptr<Camera> camera;
    vector<shared_ptr<Light>> lights;
    Scene fakeScene;

    Bounds2i sampleBounds;
    const Vector2i sampleExtent;
    Point2i nTiles;
    int tileSize;

    vector<unique_ptr<CloudBVH>> treelets;

    deque<SimRayMsg> inTransit;

    uint64_t curCameraTile {0};
    Point2i nCameraTiles;
    const uint64_t maxRays = 1'000'000;

    uint64_t curMS = 0;

    char rayBuffer[sizeof(RayState)];

    random_device rd {};
    mt19937 randgen {rd()};

    // Stats
    uint64_t totalRayTransfers = 0;
};

int main(int argc, char const *argv[]) {
    PbrtOptions.nThreads = 1;

    if (argc < 9) usage(argv[0]);

    uint64_t numWorkers = stoul(argv[2]);
    uint64_t workerBandwidth = stoul(argv[3]);
    uint64_t workerLatency = stoul(argv[4]);
    uint64_t msPerRebalance = stoul(argv[5]);
    uint64_t samplesPerPixel = stoul(argv[6]);
    uint64_t maxDepth = stoul(argv[7]);

    global::manager.init(argv[1]);

    Simulator simulator(numWorkers, workerBandwidth, workerLatency,
                        msPerRebalance, samplesPerPixel, maxDepth, argv[8]);

    simulator.simulate();

    simulator.dump_stats();
}
