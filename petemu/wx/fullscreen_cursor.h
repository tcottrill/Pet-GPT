#pragma once
#include <cmath>

// Reused from mp4_av_player_vs2022/playback_timing.h. Keep the policy independent
// of emulation time: a paused fullscreen picture must also hide an idle cursor.
class PointerActivity {
    double x_ = 0, y_ = 0;
    bool valid_ = false;
public:
    bool update(double x, double y, bool valid, double threshold) {
        if (!valid || !std::isfinite(x) || !std::isfinite(y)) { valid_ = false; return false; }
        if (!valid_) { x_ = x; y_ = y; valid_ = true; return false; }
        if (std::hypot(x - x_, y - y_) < threshold) return false;
        x_ = x; y_ = y;
        return true;
    }
};

class FullscreenCursor {
    double activity_ = 0;
    bool fullscreen_ = false, focused_ = false;
public:
    bool update(bool fullscreen, bool focused, bool pointerActivity,
                bool interacting, double now) {
        if (!std::isfinite(now)) return true;
        if (!fullscreen || !focused || pointerActivity || interacting ||
            fullscreen != fullscreen_ || focused != focused_ || now < activity_)
            activity_ = now;
        fullscreen_ = fullscreen;
        focused_ = focused;
        return !fullscreen || !focused || interacting || now - activity_ < 3.0;
    }
};
