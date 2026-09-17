#include "Logic/Probes.h"

#include <algorithm>

namespace sysmon {
namespace {

// Neither of these structures is in a public header, but both have been stable
// since Windows NT and are what every task manager reads.
struct ProcessorPerformance {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; // includes idle
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

struct ProcessorPowerInfo {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

constexpr ULONG kSystemProcessorPerformanceInformation = 8;
constexpr int kProcessorInformation = 11;

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using CallNtPowerInformationFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);

NtQuerySystemInformationFn SystemInformationEntry() {
    static NtQuerySystemInformationFn entry = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(
                           GetProcAddress(ntdll, "NtQuerySystemInformation"))
                     : nullptr;
    }();
    return entry;
}

CallNtPowerInformationFn PowerInformationEntry() {
    static CallNtPowerInformationFn entry = [] {
        const HMODULE powrprof = LoadLibraryExW(L"powrprof.dll", nullptr,
                                                LOAD_LIBRARY_SEARCH_SYSTEM32);
        return powrprof ? reinterpret_cast<CallNtPowerInformationFn>(
                              GetProcAddress(powrprof, "CallNtPowerInformation"))
                        : nullptr;
    }();
    return entry;
}

} // namespace

CpuProbe::CpuProbe() {
    coreCount_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (coreCount_ == 0) coreCount_ = 1;
    previous_.resize(coreCount_);
}

void CpuProbe::Sample(Snapshot& snapshot) {
    const auto query = SystemInformationEntry();
    if (!query) return;

    std::vector<ProcessorPerformance> counters(coreCount_);
    ULONG returned = 0;
    const LONG status =
        query(kSystemProcessorPerformanceInformation, counters.data(),
              static_cast<ULONG>(counters.size() * sizeof(ProcessorPerformance)), &returned);
    if (status < 0) return;

    const std::size_t reported =
        std::min<std::size_t>(coreCount_, returned / sizeof(ProcessorPerformance));
    if (reported == 0) return;

    snapshot.cpu.cores.assign(reported, 0.0f);

    std::uint64_t totalIdleDelta = 0;
    std::uint64_t totalBusyDelta = 0;

    for (std::size_t i = 0; i < reported; ++i) {
        const auto& counter = counters[i];
        const auto idle = static_cast<std::uint64_t>(counter.IdleTime.QuadPart);
        // KernelTime already contains IdleTime, so the busy share is whatever
        // is left once idle is taken back out.
        const auto busy = static_cast<std::uint64_t>(counter.KernelTime.QuadPart) +
                          static_cast<std::uint64_t>(counter.UserTime.QuadPart) - idle;

        const std::uint64_t idleDelta = idle - previous_[i].idle;
        const std::uint64_t busyDelta = busy - previous_[i].busy;
        previous_[i] = CoreTimes{idle, busy};

        if (!primed_) continue;

        const std::uint64_t span = idleDelta + busyDelta;
        snapshot.cpu.cores[i] =
            span > 0 ? static_cast<float>(static_cast<double>(busyDelta) / span) : 0.0f;

        totalIdleDelta += idleDelta;
        totalBusyDelta += busyDelta;
    }

    if (!primed_) {
        // The first pass only establishes a baseline; reporting it would show a
        // full load spike for the lifetime of the process so far.
        primed_ = true;
        return;
    }

    const std::uint64_t span = totalIdleDelta + totalBusyDelta;
    snapshot.cpu.total =
        span > 0 ? static_cast<float>(static_cast<double>(totalBusyDelta) / span) : 0.0f;

    if (const auto power = PowerInformationEntry()) {
        std::vector<ProcessorPowerInfo> clocks(coreCount_);
        const LONG powerStatus =
            power(kProcessorInformation, nullptr, 0, clocks.data(),
                  static_cast<ULONG>(clocks.size() * sizeof(ProcessorPowerInfo)));
        if (powerStatus >= 0 && !clocks.empty()) {
            std::uint64_t sum = 0;
            for (const auto& clock : clocks) sum += clock.CurrentMhz;
            snapshot.cpu.currentMhz = static_cast<unsigned>(sum / clocks.size());
            snapshot.cpu.maxMhz = clocks.front().MaxMhz;
        }
    }
}

} // namespace sysmon
