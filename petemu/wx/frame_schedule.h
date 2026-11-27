#pragma once
#include <chrono>

// No UI or machine dependency: delayed timers catch up at most four frames.
// Long stalls discard debt instead of freezing the UI or replaying old audio.
class FrameSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto period = std::chrono::nanoseconds(1000000000 / 60);
    // Follows the emulated video frame; next_ is kept so a change never stalls or bursts.
    void SetPeriod(std::chrono::nanoseconds p) { if (p.count() > 0) period_ = p; }
    void Reset(Clock::time_point now) { next_ = now; }
    int Due(Clock::time_point now, bool* dropped = nullptr) {
        if (dropped) *dropped = false;
        if (now < next_) return 0;
        if (now - next_ > period_ * 4) {
            next_ = now;
            if (dropped) *dropped = true;
        }
        int frames = 0;
        while (next_ <= now && frames < 4) { next_ += period_; ++frames; }
        return frames;
    }
private:
    Clock::time_point next_{};
    std::chrono::nanoseconds period_ = period;
};
