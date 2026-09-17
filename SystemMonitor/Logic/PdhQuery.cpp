#include "Logic/Probes.h"

namespace sysmon {

PdhQuery::~PdhQuery() {
    if (query_) PdhCloseQuery(query_);
}

bool PdhQuery::Open() {
    if (query_) return true;
    return PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS;
}

bool PdhQuery::AddCounter(const wchar_t* englishPath, PDH_HCOUNTER* counter) {
    if (!query_) return false;
    return PdhAddEnglishCounterW(query_, englishPath, 0, counter) == ERROR_SUCCESS;
}

bool PdhQuery::Collect() {
    if (!query_) return false;
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;
    if (collections_ < 2) ++collections_;
    return true;
}

double PdhQuery::Value(PDH_HCOUNTER counter) {
    if (!counter) return 0.0;

    PDH_FMT_COUNTERVALUE value{};
    const PDH_STATUS status =
        PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value);
    if (status != ERROR_SUCCESS) return 0.0;

    // A counter that has only been collected once reports a status rather than
    // a value; treating that as zero is exactly right.
    if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
        return 0.0;
    }
    return value.doubleValue;
}

} // namespace sysmon
