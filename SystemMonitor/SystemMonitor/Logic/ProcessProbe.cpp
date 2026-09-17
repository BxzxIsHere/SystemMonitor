#include "Logic/Probes.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>

namespace sysmon {
namespace {

// Kernel time is reported in 100 nanosecond units.
constexpr double kCpuTimeUnitsPerSecond = 10'000'000.0;

constexpr ULONG kSystemProcessInformation = 5;
constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

// winternl.h declares this structure, but truncated to the few fields it wants
// to admit to. The full layout has been stable since Vista and is what every
// task manager on Windows reads; the fields past ImageName are the ones that
// make it worth reading at all.
struct SystemProcess {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
};

NtQuerySystemInformationFn ResolveQuery() {
    // ntdll is already loaded into every process, so this is a lookup rather
    // than a load, and it cannot fail for want of the library.
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
}

std::uint64_t ToUint64(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

} // namespace

ProcessProbe::ProcessProbe() {
    coreCount_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (coreCount_ == 0) coreCount_ = 1;

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    counterFrequency_ = frequency.QuadPart ? static_cast<double>(frequency.QuadPart) : 1.0;

    // One megabyte holds a few hundred processes, so in practice the buffer is
    // sized once here and never grown again.
    buffer_.resize(1 << 20);
}

float ProcessProbe::Load(std::uint64_t cpuTime, std::uint32_t pid, double seconds) const {
    if (seconds <= 0.0) return 0.0f;

    const auto seen = previous_.find(pid);
    if (seen == previous_.end() || cpuTime < seen->second.cpuTime) return 0.0f;

    const double busySeconds = (cpuTime - seen->second.cpuTime) / kCpuTimeUnitsPerSecond;
    return std::clamp(static_cast<float>(busySeconds / (seconds * coreCount_)), 0.0f, 1.0f);
}

bool ProcessProbe::SampleFromKernel(std::vector<ProcessEntry>& entries,
                                    std::unordered_map<std::uint32_t, Usage>& current,
                                    double seconds) {
    static const NtQuerySystemInformationFn query = ResolveQuery();
    if (!query) return false;

    // The process table can grow between sizing it and reading it, so the call
    // is retried rather than trusted to fit first time.
    for (int attempt = 0;; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = query(kSystemProcessInformation, buffer_.data(),
                                      static_cast<ULONG>(buffer_.size()), &needed);
        if (status == 0) break;
        if (status != kInfoLengthMismatch || attempt >= 6) return false;
        buffer_.resize(needed ? needed + (64u << 10) : buffer_.size() * 2);
    }

    const std::uint8_t* const begin = buffer_.data();
    const std::uint8_t* const end = begin + buffer_.size();
    const std::uint8_t* cursor = begin;

    // Cycle deltas are collected first and turned into percentages afterwards,
    // because a share is only meaningful against the total, and the total is
    // not known until the whole table has been walked.
    cycleDeltas_.clear();
    cycleDeltas_.reserve(entries.capacity());
    double totalCycles = 0.0;
    bool cyclesUsable = false;

    for (;;) {
        if (cursor + sizeof(SystemProcess) > end) return false;
        const auto* process = reinterpret_cast<const SystemProcess*>(cursor);

        const auto pid =
            static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(process->UniqueProcessId));

        const std::uint64_t cpuTime =
            static_cast<std::uint64_t>(process->KernelTime.QuadPart) +
            static_cast<std::uint64_t>(process->UserTime.QuadPart);
        const std::uint64_t cycles = process->CycleTime;
        if (cycles != 0) cyclesUsable = true;

        double cycleDelta = 0.0;
        const auto seen = previous_.find(pid);
        if (seen != previous_.end() && cycles >= seen->second.cycles) {
            cycleDelta = static_cast<double>(cycles - seen->second.cycles);
        }

        // The idle process is counted towards the total and then dropped. Its
        // whole job is to be busy doing nothing, so listing it would pin it at
        // the top forever, but the cycles it burned are exactly the share of
        // the machine that nothing else was using.
        totalCycles += cycleDelta;
        current[pid] = Usage{cpuTime, cycles};

        if (pid != 0) {
            ProcessEntry entry;
            entry.pid = pid;
            entry.name = process->ImageName.Buffer
                             ? std::wstring(process->ImageName.Buffer,
                                            process->ImageName.Length / sizeof(wchar_t))
                             : std::wstring(L"System Idle");
            entry.workingSetBytes = process->WorkingSetSize;
            entry.cpu = Load(cpuTime, pid, seconds); // replaced below when cycles work
            entries.push_back(std::move(entry));
            cycleDeltas_.push_back(cycleDelta);
        }

        if (process->NextEntryOffset == 0) break;
        if (process->NextEntryOffset < sizeof(SystemProcess)) return false;
        cursor += process->NextEntryOffset;
    }

    // Cycles rather than kernel and user time, because those are accumulated in
    // whole 15.6 ms clock ticks: on eight cores that is 0.195% at a time, so
    // everything quieter than that reports zero and the table fills with idle
    // processes that are not idle. Cycles have no such floor.
    if (cyclesUsable && totalCycles > 0.0 && cycleDeltas_.size() == entries.size()) {
        for (std::size_t at = 0; at < entries.size(); ++at) {
            entries[at].cpu =
                std::clamp(static_cast<float>(cycleDeltas_[at] / totalCycles), 0.0f, 1.0f);
        }
    }

    return true;
}

void ProcessProbe::SampleFromToolhelp(std::vector<ProcessEntry>& entries,
                                      std::unordered_map<std::uint32_t, Usage>& current,
                                      double seconds) {
    const HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshotHandle == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);

    if (Process32FirstW(snapshotHandle, &process)) {
        do {
            if (process.th32ProcessID == 0) continue;

            ProcessEntry entry;
            entry.pid = process.th32ProcessID;
            entry.name = process.szExeFile;

            const HANDLE handle =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID);
            if (handle) {
                FILETIME created{};
                FILETIME exited{};
                FILETIME kernel{};
                FILETIME user{};
                if (GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
                    const std::uint64_t cpuTime = ToUint64(kernel) + ToUint64(user);
                    entry.cpu = Load(cpuTime, entry.pid, seconds);
                    current[entry.pid] = Usage{cpuTime};
                }

                PROCESS_MEMORY_COUNTERS memory{};
                memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(handle, &memory, sizeof(memory))) {
                    entry.workingSetBytes = memory.WorkingSetSize;
                }
                CloseHandle(handle);
            }

            entries.push_back(std::move(entry));
        } while (Process32NextW(snapshotHandle, &process));
    }

    CloseHandle(snapshotHandle);
}

void ProcessProbe::Sample(Snapshot& snapshot) {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);

    // The performance counter rather than the tick count: the tick count moves
    // in 15.6 ms steps, which is a percent and a half of error on a one second
    // interval, and it shows up as every figure in the table quivering.
    const double seconds =
        previousCounter_ > 0 ? (counter.QuadPart - previousCounter_) / counterFrequency_ : 0.0;

    // Rebuilt from scratch each pass so that exited processes drop out and a
    // recycled process id can never inherit the previous owner's counters.
    std::unordered_map<std::uint32_t, Usage> current;
    current.reserve(previous_.size() + 32);

    std::vector<ProcessEntry> entries;
    entries.reserve(std::max<std::size_t>(previous_.size() + 32, 256));

    if (kernelTableAvailable_) {
        if (!SampleFromKernel(entries, current, seconds)) {
            kernelTableAvailable_ = false;
            entries.clear();
            current.clear();
        }
    }
    if (!kernelTableAvailable_) {
        SampleFromToolhelp(entries, current, seconds);
    }

    previous_ = std::move(current);
    previousCounter_ = counter.QuadPart;

    // Published whole and unordered. Truncating here would decide for the view
    // which processes matter, and the view lets the user sort by any column.
    snapshot.processes = std::move(entries);
}

} // namespace sysmon
