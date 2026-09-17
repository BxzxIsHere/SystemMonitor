#pragma once

#include "Logic/Metrics.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace sysmon {

// Runs every probe on a background thread and publishes a complete, consistent
// set of readings.
//
// The UI never blocks on a probe: it asks for the newest readings once a frame
// and gets an immediate answer, whether or not anything has changed.
class Sampler {
public:
    Sampler() = default;
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    void Start();
    void Stop();

    // Copies the published readings into the caller when they are newer than
    // the copy it already holds, and reports whether it did.
    bool Poll(Readings& destination);

private:
    void Run();
    void Publish(const Snapshot& snapshot);

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = false;

    std::mutex publishMutex_;
    Readings published_;
};

} // namespace sysmon
