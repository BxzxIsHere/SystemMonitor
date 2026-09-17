#include "Logic/Probes.h"

#include <algorithm>

namespace sysmon {

bool StorageProbe::Open() {
    if (!query_.Open()) return false;

    const bool ok =
        query_.AddCounter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &readBytes_) &&
        query_.AddCounter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &writeBytes_) &&
        query_.AddCounter(L"\\PhysicalDisk(_Total)\\% Disk Time", &busyTime_);

    if (ok) query_.Collect();
    return ok;
}

void StorageProbe::Sample(Snapshot& snapshot) {
    auto& storage = snapshot.storage;
    storage.capacityBytes = capacityBytes_;
    storage.freeBytes = freeBytes_;

    if (!query_.Valid() || !query_.Collect()) return;
    storage.available = true;
    if (!query_.Primed()) return;

    storage.readBytesPerSecond = PdhQuery::Value(readBytes_);
    storage.writeBytesPerSecond = PdhQuery::Value(writeBytes_);

    // Percent disk time is really queue-weighted busy time and happily exceeds
    // one hundred on a striped or heavily queued volume, so it is clamped.
    storage.activity =
        std::clamp(static_cast<float>(PdhQuery::Value(busyTime_) / 100.0), 0.0f, 1.0f);
}

void StorageProbe::SampleCapacity(Snapshot& snapshot) {
    capacityBytes_ = 0;
    freeBytes_ = 0;

    const DWORD drives = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if ((drives & (1u << index)) == 0) continue;

        const wchar_t root[] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;

        ULARGE_INTEGER free{};
        ULARGE_INTEGER total{};
        if (GetDiskFreeSpaceExW(root, nullptr, &total, &free)) {
            capacityBytes_ += total.QuadPart;
            freeBytes_ += free.QuadPart;
        }
    }

    snapshot.storage.capacityBytes = capacityBytes_;
    snapshot.storage.freeBytes = freeBytes_;
}

} // namespace sysmon
