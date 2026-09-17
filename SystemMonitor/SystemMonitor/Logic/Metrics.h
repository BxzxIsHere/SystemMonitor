#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sysmon {

// Fast samples drive the graphs; the slow multiple covers the probes that are
// too expensive to run at that rate (process enumeration, volume capacity).
constexpr unsigned kSampleIntervalMs = 250;
constexpr unsigned kSlowSampleEvery = 4;

// 300 samples at 250 ms is a rolling 75 second window, which is wide enough to
// show a spike settling down but still moves visibly while you watch it.
constexpr std::size_t kHistoryLength = 300;

// A fixed-capacity ring of recent samples. Index 0 is always the oldest sample
// still retained, so a chart can walk it left to right without any bookkeeping.
class Trace {
public:
    void Push(float value) {
        samples_[head_] = value;
        head_ = (head_ + 1) % kHistoryLength;
        if (count_ < kHistoryLength) ++count_;
    }

    std::size_t Size() const { return count_; }
    bool Empty() const { return count_ == 0; }
    static constexpr std::size_t Capacity() { return kHistoryLength; }

    float At(std::size_t indexFromOldest) const {
        if (indexFromOldest >= count_) return 0.0f;
        const std::size_t oldest = (head_ + kHistoryLength - count_) % kHistoryLength;
        return samples_[(oldest + indexFromOldest) % kHistoryLength];
    }

    float Latest() const { return count_ == 0 ? 0.0f : At(count_ - 1); }

    float Peak() const {
        float peak = 0.0f;
        for (std::size_t i = 0; i < count_; ++i) peak = std::max(peak, At(i));
        return peak;
    }

    float Average() const {
        if (count_ == 0) return 0.0f;
        float sum = 0.0f;
        for (std::size_t i = 0; i < count_; ++i) sum += At(i);
        return sum / static_cast<float>(count_);
    }

private:
    std::array<float, kHistoryLength> samples_{};
    std::size_t count_ = 0;
    std::size_t head_ = 0;
};

// Loads are normalised to 0..1 everywhere; only the formatter turns them into
// percentages, so nothing downstream has to remember which scale it is on.
struct CpuSnapshot {
    float total = 0.0f;
    std::vector<float> cores;
    unsigned currentMhz = 0;
    unsigned maxMhz = 0;
    unsigned processes = 0;
    unsigned threads = 0;
    unsigned handles = 0;
};

struct MemorySnapshot {
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t availableBytes = 0;
    std::uint64_t committedBytes = 0;
    std::uint64_t commitLimitBytes = 0;
    float load = 0.0f;
};

struct StorageSnapshot {
    double readBytesPerSecond = 0.0;
    double writeBytesPerSecond = 0.0;
    float activity = 0.0f;
    std::uint64_t capacityBytes = 0;
    std::uint64_t freeBytes = 0;
    bool available = false;
};

struct NetworkSnapshot {
    double downBytesPerSecond = 0.0;
    double upBytesPerSecond = 0.0;
    std::uint64_t receivedBytes = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t linkSpeedBitsPerSecond = 0;
    std::wstring adapter;
};

struct GpuSnapshot {
    float utilisation = 0.0f;
    std::uint64_t dedicatedUsedBytes = 0;
    std::uint64_t dedicatedTotalBytes = 0;
    std::wstring name;
    bool available = false;

    // Read from the display driver rather than from a sensor chip, so it needs
    // no kernel driver of ours. Not every driver reports it, which is what
    // hasTemperature says; a card idling with its fans stopped reports zero rpm
    // and means it, so that cannot be used to tell "absent" from "zero".
    float temperatureCelsius = 0.0f;
    unsigned fanRpm = 0;
    bool hasTemperature = false;
};

struct ProcessEntry {
    std::wstring name;
    std::uint32_t pid = 0;
    float cpu = 0.0f;
    std::uint64_t workingSetBytes = 0;
};

struct Snapshot {
    CpuSnapshot cpu;
    MemorySnapshot memory;
    StorageSnapshot storage;
    NetworkSnapshot network;
    GpuSnapshot gpu;
    // Every running process, in no particular order. The view sorts this by
    // whichever column the user picked, which it could not do from a list the
    // probe had already truncated to a top few by one fixed metric.
    std::vector<ProcessEntry> processes;
    std::uint64_t uptimeSeconds = 0;
};

// Rolling series, kept next to the snapshot so the UI never has to accumulate
// history of its own.
struct Traces {
    Trace cpu;
    Trace memory;
    Trace gpu;
    Trace diskActivity;
    Trace networkDown;
    Trace networkUp;
};

// One megabit per second: the floor the network chart never scales below, so a
// trickle of background traffic does not fill the panel.
constexpr double kNetworkScaleFloor = 131072.0;

struct Readings {
    Snapshot snapshot;
    Traces traces;
    // Peak rate still inside the window, which keeps the network chart on a
    // stable scale instead of rescaling on every sample.
    double networkScale = kNetworkScaleFloor;
    std::uint64_t version = 0;
};

} // namespace sysmon
