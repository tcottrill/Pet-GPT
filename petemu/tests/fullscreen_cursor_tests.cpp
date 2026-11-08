#include "../wx/fullscreen_cursor.h"
#include <cstdio>
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) { if (!ok) { std::printf("FAIL: %s\n", name); ++failures; } };
    FullscreenCursor cursor;
    check(cursor.update(true, true, false, false, 10), "entry shows cursor");
    check(!cursor.update(true, true, false, false, 13), "three-second timeout, including pause");
    check(cursor.update(true, true, true, false, 14), "motion restores cursor");
    check(!cursor.update(true, true, false, false, 17), "motion restarts timeout");
    check(cursor.update(true, true, false, true, 30), "menu or drag restores cursor");
    check(cursor.update(true, true, false, false, 31), "menu close grace period");
    check(!cursor.update(true, true, false, false, 33), "hide after menu close");
    check(cursor.update(true, false, false, false, 40), "focus loss restores cursor");
    check(cursor.update(true, true, false, false, 50), "focus regain restarts timer");
    check(!cursor.update(true, true, false, false, 53), "hide after focus regain");
    check(cursor.update(false, true, false, false, 60), "windowed cursor stays visible");
    PointerActivity motion;
    check(!motion.update(100,100,true,6), "initial position is not motion");
    check(!motion.update(101,100,true,6), "ignore jitter");
    check(motion.update(107,100,true,6), "accumulated motion wakes cursor");
    std::printf("Cursor policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
