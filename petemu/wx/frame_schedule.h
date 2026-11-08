#pragma once
#include <chrono>

// No UI or machine dependency: delayed timers catch up at most four frames.
// Long stalls discard debt instead of freezing the UI or replaying old audio.
class FrameSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto period = std::chrono::nanoseconds(1000000000 / 60);
    void Reset(Clock::time_point now) { next_ = now; }
    int Due(Clock::time_point now, bool* dropped = nullptr) {
        if (dropped) *dropped = false;
        if (now < next_) return 0;
        if (now - next_ > period * 4) {
            next_ = now;
            if (dropped) *dropped = true;
        }
        int frames = 0;
        while (next_ <= now && frames < 4) { next_ += period; ++frames; }
        return frames;
    }
private:
    Clock::time_point next_{};
};
