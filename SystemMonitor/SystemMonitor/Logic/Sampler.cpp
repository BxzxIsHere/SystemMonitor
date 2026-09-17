#include "Logic/Sampler.h"

#include "Libs/Platform.h"
#include "Logic/Probes.h"

#include <algorithm>
#include <chrono>

namespace sysmon {
namespace {

// Volume capacity moves on the scale of minutes, so re-reading it every ten
// seconds is already generous.
constexpr unsigned kCapacitySampleEvery = 40;

} // namespace

Sampler::~Sampler() {
    Stop();
}

void Sampler::Start() {
    if (worker_.joinable()) return;
    running_ = true;
    worker_ = std::thread(&Sampler::Run, this);
}

void Sampler::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool Sampler::Poll(Readings& destination) {
    std::lock_guard<std::mutex> lock(publishMutex_);
    if (published_.version == destination.version) return false;
    destination = published_;
    return true;
}

void Sampler::Run() {
    // Every probe is constructed here so that all of its state, including the
    // DXGI adapter the GPU probe holds, belongs to this thread alone.
    CpuProbe cpu;
    MemoryProbe memory;
    StorageProbe storage;
    NetworkProbe network;
    GpuProbe gpu;
    ProcessProbe processes;

    storage.Open();
    gpu.Open();

    Snapshot snapshot;
    unsigned tick = 0;

    for (;;) {
        cpu.Sample(snapshot);
        memory.Sample(snapshot);
        storage.Sample(snapshot);
        network.Sample(snapshot);
        gpu.Sample(snapshot);

        if (tick % kSlowSampleEvery == 0) processes.Sample(snapshot);
        if (tick % kCapacitySampleEvery == 0) storage.SampleCapacity(snapshot);

        snapshot.uptimeSeconds = GetTickCount64() / 1000;
        // The first pass only establishes the baselines the delta-based probes
        // need. Publishing it would seat a phantom zero at the head of every
        // trace, which shows up as a cliff on the left of the first chart.
        if (tick > 0) Publish(snapshot);
        ++tick;

        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(kSampleIntervalMs),
                       [this] { return !running_; });
        if (!running_) return;
    }
}

void Sampler::Publish(const Snapshot& snapshot) {
    std::lock_guard<std::mutex> lock(publishMutex_);

    published_.snapshot = snapshot;

    auto& traces = published_.traces;
    traces.cpu.Push(snapshot.cpu.total);
    traces.memory.Push(snapshot.memory.load);
    traces.gpu.Push(snapshot.gpu.utilisation);
    traces.diskActivity.Push(snapshot.storage.activity);
    traces.networkDown.Push(static_cast<float>(snapshot.network.downBytesPerSecond));
    traces.networkUp.Push(static_cast<float>(snapshot.network.upBytesPerSecond));

    // Scaling to the peak still in the window means the chart stays readable
    // during a download and settles back on its own afterwards, without the
    // axis twitching on every sample.
    published_.networkScale =
        std::max({kNetworkScaleFloor, static_cast<double>(traces.networkDown.Peak()),
                  static_cast<double>(traces.networkUp.Peak())});

    ++published_.version;
}

} // namespace sysmon
