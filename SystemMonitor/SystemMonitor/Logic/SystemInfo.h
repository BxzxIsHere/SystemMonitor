#pragma once

#include <cstdint>
#include <string>

namespace sysmon {

// Everything about the machine that cannot change while the app is running.
// Gathered once, on first use.
struct SystemInfo {
    std::wstring cpuBrand;
    std::wstring osName;
    std::wstring osBuild;
    std::wstring machineName;
    std::wstring userName;
    unsigned physicalCores = 0;
    unsigned logicalCores = 0;
    std::uint64_t totalMemoryBytes = 0;

    static const SystemInfo& Current();
};

} // namespace sysmon
