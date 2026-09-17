#include "Logic/Probes.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

// d3dkmthk.h uses NTSTATUS without including what declares it.
#include <winternl.h>

#include <d3dkmthk.h>

#include <algorithm>
#include <map>
#include <string>

namespace sysmon {
namespace {

using Microsoft::WRL::ComPtr;

// The display miniport interface. It lives in gdi32 and is how the task manager
// reads a card's temperature, so it needs no kernel driver of ours and no vendor
// SDK. Resolved by name because it is not in any import library.
struct KernelModeThunk {
    NTSTATUS(APIENTRY* OpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID*) = nullptr;
    NTSTATUS(APIENTRY* QueryAdapterInfo)(D3DKMT_QUERYADAPTERINFO*) = nullptr;
    NTSTATUS(APIENTRY* CloseAdapter)(const D3DKMT_CLOSEADAPTER*) = nullptr;

    bool Usable() const { return OpenAdapterFromLuid && QueryAdapterInfo && CloseAdapter; }
};

const KernelModeThunk& Kmt() {
    static const KernelModeThunk thunk = [] {
        KernelModeThunk resolved;
        // gdi32 is already loaded in any process with a window, so this is a
        // lookup rather than a load.
        const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) return resolved;

        resolved.OpenAdapterFromLuid =
            reinterpret_cast<decltype(resolved.OpenAdapterFromLuid)>(
                GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
        resolved.QueryAdapterInfo = reinterpret_cast<decltype(resolved.QueryAdapterInfo)>(
            GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
        resolved.CloseAdapter = reinterpret_cast<decltype(resolved.CloseAdapter)>(
            GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        return resolved;
    }();
    return thunk;
}

// Instance names look like
//   pid_9184_luid_0x00000000_0x0000BC2E_phys_0_eng_3_engtype_3D
// and the trailing engine type is the only part worth keeping.
std::wstring EngineTypeOf(const wchar_t* instance) {
    static constexpr wchar_t kMarker[] = L"engtype_";
    const std::wstring name = instance ? instance : L"";
    const std::size_t position = name.rfind(kMarker);
    if (position == std::wstring::npos) return L"other";
    return name.substr(position + (ARRAYSIZE(kMarker) - 1));
}

} // namespace

// The adapter is held across samples so video memory can be read without
// rebuilding a DXGI factory four times a second.
struct GpuProbe::Adapter {
    ~Adapter() { CloseKernelHandle(); }

    void CloseKernelHandle() {
        if (kernelHandle == 0) return;
        const D3DKMT_CLOSEADAPTER close{kernelHandle};
        if (Kmt().CloseAdapter) Kmt().CloseAdapter(&close);
        kernelHandle = 0;
    }

    ComPtr<IDXGIAdapter3> device;
    std::wstring name;
    std::uint64_t dedicatedBytes = 0;

    // Identifies the same card to the display miniport that DXGI just described,
    // rather than enumerating again and hoping the order matches.
    LUID luid{};
    D3DKMT_HANDLE kernelHandle = 0;
    bool temperatureSupported = true;
};

GpuProbe::GpuProbe() = default;
GpuProbe::~GpuProbe() = default;

bool GpuProbe::Open() {
    adapter_ = std::make_unique<Adapter>();

    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> candidate;
        std::uint64_t best = 0;

        for (UINT index = 0; factory->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf()) !=
                             DXGI_ERROR_NOT_FOUND;
             ++index) {
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description))) continue;
            // The Basic Render Driver is always present and never interesting.
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (description.DedicatedVideoMemory >= best) {
                best = description.DedicatedVideoMemory;
                adapter_->name = description.Description;
                adapter_->dedicatedBytes = description.DedicatedVideoMemory;
                adapter_->luid = description.AdapterLuid;
                candidate.As(&adapter_->device);
            }
        }
    }

    if (!query_.Open()) return false;

    // GPU engine counters arrived in Windows 10 1709. Where they are missing
    // the panel simply falls back to reporting memory only.
    countersAvailable_ =
        query_.AddCounter(L"\\GPU Engine(*)\\Utilization Percentage", &engineUtilisation_);
    if (countersAvailable_) query_.Collect();
    return countersAvailable_;
}

double GpuProbe::EngineUtilisation() {
    if (!countersAvailable_ || !query_.Primed()) return 0.0;

    auto fetch = [&](DWORD& size, DWORD& count) {
        auto* items = buffer_.empty()
                          ? nullptr
                          : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        return PdhGetFormattedCounterArrayW(engineUtilisation_, PDH_FMT_DOUBLE, &size, &count,
                                            items);
    };

    DWORD size = static_cast<DWORD>(buffer_.size());
    DWORD count = 0;
    PDH_STATUS status = fetch(size, count);

    if (status == PDH_MORE_DATA) {
        // The instance list changes every time a process starts using the GPU,
        // so the buffer is grown on demand and reused afterwards.
        buffer_.resize(size);
        size = static_cast<DWORD>(buffer_.size());
        status = fetch(size, count);
    }
    if (status != ERROR_SUCCESS || count == 0) return 0.0;

    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
    std::map<std::wstring, double> byEngineType;
    for (DWORD index = 0; index < count; ++index) {
        byEngineType[EngineTypeOf(items[index].szName)] += items[index].FmtValue.doubleValue;
    }

    // Engines run in parallel, so summing everything would report 300% on an
    // idle machine. The busiest engine type is the number a task manager shows.
    double busiest = 0.0;
    for (const auto& [type, value] : byEngineType) busiest = std::max(busiest, value);
    return busiest;
}

void GpuProbe::SampleThermals(Snapshot& snapshot) {
    if (!adapter_ || !adapter_->temperatureSupported || !Kmt().Usable()) return;

    if (adapter_->kernelHandle == 0) {
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = adapter_->luid;
        if (Kmt().OpenAdapterFromLuid(&open) != 0) {
            adapter_->temperatureSupported = false;
            return;
        }
        adapter_->kernelHandle = open.hAdapter;
    }

    D3DKMT_ADAPTER_PERFDATA performance{};
    D3DKMT_QUERYADAPTERINFO request{};
    request.hAdapter = adapter_->kernelHandle;
    request.Type = KMTQAITYPE_ADAPTERPERFDATA;
    request.pPrivateDriverData = &performance;
    request.PrivateDriverDataSize = sizeof(performance);

    if (Kmt().QueryAdapterInfo(&request) != 0) {
        // A driver restart invalidates the handle, so it is dropped and
        // reopened on the next pass rather than giving up for good.
        adapter_->CloseKernelHandle();
        return;
    }

    // Deci-degrees Celsius. A card that reports an implausible figure is a card
    // whose driver does not really implement this, and saying nothing is better
    // than putting 0 C or 6000 C beside the memory readout.
    const float celsius = performance.Temperature / 10.0f;
    if (celsius <= 0.0f || celsius > 150.0f) {
        adapter_->temperatureSupported = false;
        return;
    }

    snapshot.gpu.temperatureCelsius = celsius;
    snapshot.gpu.fanRpm = performance.FanRPM;
    snapshot.gpu.hasTemperature = true;
}

void GpuProbe::Sample(Snapshot& snapshot) {
    auto& gpu = snapshot.gpu;

    if (adapter_) {
        gpu.name = adapter_->name;
        gpu.dedicatedTotalBytes = adapter_->dedicatedBytes;
        gpu.available = !adapter_->name.empty();

        if (adapter_->device) {
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            if (SUCCEEDED(adapter_->device->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
                gpu.dedicatedUsedBytes = memory.CurrentUsage;
            }
        }
    }

    SampleThermals(snapshot);

    if (!query_.Valid() || !query_.Collect()) return;
    gpu.utilisation = std::clamp(static_cast<float>(EngineUtilisation() / 100.0), 0.0f, 1.0f);
}

} // namespace sysmon
