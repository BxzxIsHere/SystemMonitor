#include "Logic/Probes.h"

#include <psapi.h>

namespace sysmon {

void MemoryProbe::Sample(Snapshot& snapshot) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        auto& memory = snapshot.memory;
        memory.totalBytes = status.ullTotalPhys;
        memory.availableBytes = status.ullAvailPhys;
        memory.usedBytes = status.ullTotalPhys - status.ullAvailPhys;
        memory.load = status.ullTotalPhys > 0
                          ? static_cast<float>(static_cast<double>(memory.usedBytes) /
                                               status.ullTotalPhys)
                          : 0.0f;
    }

    // One call covers commit charge and the system object counts that sit
    // beside the processor readout in every task manager ever written.
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        const auto pageSize = static_cast<std::uint64_t>(performance.PageSize);
        snapshot.memory.committedBytes = performance.CommitTotal * pageSize;
        snapshot.memory.commitLimitBytes = performance.CommitLimit * pageSize;
        snapshot.cpu.processes = performance.ProcessCount;
        snapshot.cpu.threads = performance.ThreadCount;
        snapshot.cpu.handles = performance.HandleCount;
    }
}

} // namespace sysmon
