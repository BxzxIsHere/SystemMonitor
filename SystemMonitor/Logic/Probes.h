#pragma once

#include "Libs/Platform.h"
#include "Logic/Metrics.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace sysmon {

// Minimal RAII wrapper around a performance counter query.
//
// Counters are always added by their English path: display names are localised
// on non-English installs, and a hard-coded localised path silently returns
// zero forever on those machines.
class PdhQuery {
public:
    PdhQuery() = default;
    ~PdhQuery();

    PdhQuery(const PdhQuery&) = delete;
    PdhQuery& operator=(const PdhQuery&) = delete;

    bool Open();
    bool AddCounter(const wchar_t* englishPath, PDH_HCOUNTER* counter);
    bool Collect();
    bool Valid() const { return query_ != nullptr; }

    // Rate counters need two collections before they mean anything, so the
    // first call after Open deliberately reports nothing.
    bool Primed() const { return collections_ >= 2; }

    static double Value(PDH_HCOUNTER counter);

private:
    PDH_HQUERY query_ = nullptr;
    unsigned collections_ = 0;
};

// Total and per-core load, straight from the kernel processor performance
// counters, plus the current clock from the power management interface.
class CpuProbe {
public:
    CpuProbe();
    void Sample(Snapshot& snapshot);

private:
    struct CoreTimes {
        std::uint64_t idle = 0;
        std::uint64_t busy = 0;
    };

    std::vector<CoreTimes> previous_;
    unsigned coreCount_ = 0;
    bool primed_ = false;
};

// Physical memory, commit charge, and the system-wide object counts that sit
// beside the CPU readout in every task manager ever written.
class MemoryProbe {
public:
    void Sample(Snapshot& snapshot);
};

// Throughput and busy time across all physical disks, plus the combined
// capacity of the fixed volumes.
class StorageProbe {
public:
    bool Open();
    void Sample(Snapshot& snapshot);
    void SampleCapacity(Snapshot& snapshot);

private:
    PdhQuery query_;
    PDH_HCOUNTER readBytes_ = nullptr;
    PDH_HCOUNTER writeBytes_ = nullptr;
    PDH_HCOUNTER busyTime_ = nullptr;

    std::uint64_t capacityBytes_ = 0;
    std::uint64_t freeBytes_ = 0;
};

// Aggregate traffic over every live, non-loopback adapter, with the busiest
// one reported by name.
class NetworkProbe {
public:
    void Sample(Snapshot& snapshot);

private:
    std::uint64_t previousReceived_ = 0;
    std::uint64_t previousSent_ = 0;
    std::uint64_t previousTicks_ = 0;
    bool primed_ = false;
};

// Adapter identity and video memory come from DXGI; utilisation comes from the
// GPU engine counters, which is the same source the task manager graph uses.
class GpuProbe {
public:
    GpuProbe();
    ~GpuProbe();

    GpuProbe(const GpuProbe&) = delete;
    GpuProbe& operator=(const GpuProbe&) = delete;

    bool Open();
    void Sample(Snapshot& snapshot);

private:
    double EngineUtilisation();

    // Temperature and fan speed, straight from the display miniport. Separate
    // from Sample because it is the one part that some drivers do not answer.
    void SampleThermals(Snapshot& snapshot);

    // Keeps DXGI out of this header; the adapter interfaces live in the
    // implementation where they belong.
    struct Adapter;

    PdhQuery query_;
    PDH_HCOUNTER engineUtilisation_ = nullptr;
    std::vector<std::uint8_t> buffer_;
    std::unique_ptr<Adapter> adapter_;
    bool countersAvailable_ = false;
};

// Top consumers by CPU. Process CPU is a delta over wall clock, so the first
// sample after start-up is skipped rather than reported as a spike.
class ProcessProbe {
public:
    ProcessProbe();
    void Sample(Snapshot& snapshot);

private:
    struct Usage {
        std::uint64_t cpuTime = 0; // 100 ns units, quantised to the clock tick
        std::uint64_t cycles = 0;  // actual cycles retired, no quantisation
    };

    // Reads every process in one call. Asking the kernel for the whole table is
    // both faster than opening each process in turn and, more importantly, the
    // only way to see the ones a normal account is not allowed to open at all.
    bool SampleFromKernel(std::vector<ProcessEntry>& entries,
                          std::unordered_map<std::uint32_t, Usage>& current, double seconds);

    // Used when the kernel table is unavailable. Slower, and blind to anything
    // the account cannot open, which is why it is the fallback and not the
    // other way round.
    void SampleFromToolhelp(std::vector<ProcessEntry>& entries,
                            std::unordered_map<std::uint32_t, Usage>& current, double seconds);

    float Load(std::uint64_t cpuTime, std::uint32_t pid, double seconds) const;

    std::unordered_map<std::uint32_t, Usage> previous_;
    std::vector<std::uint8_t> buffer_;
    std::vector<double> cycleDeltas_;
    std::int64_t previousCounter_ = 0;
    double counterFrequency_ = 1.0;
    unsigned coreCount_ = 1;
    bool kernelTableAvailable_ = true;
};

} // namespace sysmon
