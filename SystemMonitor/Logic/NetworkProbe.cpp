#include "Logic/Probes.h"

#include <winsock2.h>
#include <ws2ipdef.h>

#include <iphlpapi.h>
#include <netioapi.h>

namespace sysmon {
namespace {

// ipifcons.h spells this 24; naming it locally keeps the include list short.
constexpr ULONG kLoopbackInterface = 24;

} // namespace

void NetworkProbe::Sample(Snapshot& snapshot) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) return;

    std::uint64_t received = 0;
    std::uint64_t sent = 0;
    std::uint64_t busiest = 0;

    auto& network = snapshot.network;
    network.adapter.clear();
    network.linkSpeedBitsPerSecond = 0;

    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const MIB_IF_ROW2& row = table->Table[index];

        if (row.Type == kLoopbackInterface) continue;
        if (row.OperStatus != IfOperStatusUp) continue;
        // The stack layers synthetic filter interfaces over real adapters;
        // counting those as well would double every byte on screen.
        if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;

        received += row.InOctets;
        sent += row.OutOctets;

        const std::uint64_t traffic = row.InOctets + row.OutOctets;
        if (traffic > busiest) {
            busiest = traffic;
            network.adapter.assign(row.Alias);
            network.linkSpeedBitsPerSecond = row.ReceiveLinkSpeed;
        }
    }

    FreeMibTable(table);

    network.receivedBytes = received;
    network.sentBytes = sent;

    const std::uint64_t now = GetTickCount64();
    if (primed_ && now > previousTicks_) {
        const double seconds = (now - previousTicks_) / 1000.0;
        // An adapter disappearing takes its counters with it, so a delta can go
        // backwards. Reporting idle beats reporting an impossible spike.
        network.downBytesPerSecond =
            received >= previousReceived_ ? (received - previousReceived_) / seconds : 0.0;
        network.upBytesPerSecond =
            sent >= previousSent_ ? (sent - previousSent_) / seconds : 0.0;
    }

    previousReceived_ = received;
    previousSent_ = sent;
    previousTicks_ = now;
    primed_ = true;
}

} // namespace sysmon
