#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sysmon::ui {

// Exponential ease towards a target.
//
// The step is derived from elapsed time rather than counted in frames, so the
// motion is identical at 60 fps and at 240, and a dropped frame does not show
// up as a stutter.
class Eased {
public:
    void Approach(float target, float seconds, float rate = 9.0f) {
        value_ += (target - value_) * (1.0f - std::exp(-rate * seconds));
    }

    // Jumps straight to a value, for the first sample where easing up from
    // zero would look like a spike that never happened.
    void Set(float value) { value_ = value; }

    float Value() const { return value_; }

private:
    float value_ = 0.0f;
};

// One eased value per entry of a parallel series, such as the per-core loads.
class EasedSeries {
public:
    void Approach(const std::vector<float>& targets, float seconds, float rate = 9.0f) {
        if (values_.size() != targets.size()) {
            // The core count only changes if the series itself was replaced, so
            // seeding straight from the targets avoids a sweep up from zero.
            values_.assign(targets.begin(), targets.end());
            eased_.resize(targets.size());
            for (std::size_t i = 0; i < targets.size(); ++i) eased_[i].Set(targets[i]);
            return;
        }

        for (std::size_t i = 0; i < targets.size(); ++i) {
            eased_[i].Approach(targets[i], seconds, rate);
            values_[i] = eased_[i].Value();
        }
    }

    const std::vector<float>& Values() const { return values_; }

private:
    std::vector<Eased> eased_;
    std::vector<float> values_;
};

// Eased values keyed by an id, for lists whose members come and go: processes
// start and exit constantly, and each row should animate on its own terms
// rather than inherit whatever value the previous occupant of that row had.
class EasedTable {
public:
    float Approach(std::uint32_t key, float target, float seconds, float rate = 9.0f) {
        const auto [position, inserted] = entries_.try_emplace(key);
        Entry& entry = position->second;
        entry.touched = generation_;

        if (inserted) {
            // Seeded at its value rather than eased up from zero. A process that
            // appears already busy is not a process that just ramped up, and
            // animating it from nothing is both a lie and, where the value is
            // what the rows are ordered by, a way for a busy process to sort
            // itself out of the window that keeps it animated at all.
            entry.value.Set(target);
        } else {
            entry.value.Approach(target, seconds, rate);
        }
        return entry.value.Value();
    }

    // Drops anything not touched this generation, so exited processes do not
    // accumulate for the lifetime of the app.
    void Sweep() {
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = it->second.touched == generation_ ? std::next(it) : entries_.erase(it);
        }
        ++generation_;
    }

private:
    struct Entry {
        Eased value;
        std::uint64_t touched = 0;
    };

    std::unordered_map<std::uint32_t, Entry> entries_;
    std::uint64_t generation_ = 1;
};

} // namespace sysmon::ui
