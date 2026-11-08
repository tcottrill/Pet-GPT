#include "iniFile.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("pet-ini-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    const std::string id = R"(\\?\DISPLAY#GSM5B09#5&123&0&UID123#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})";
    {
        std::ofstream out(path);
        out << "[video]\nstarting_monitor_id=" << id << "\n"
            << "starting_monitor=2 # monitor comment\n"
            << "scale=1; legacy inline comment\n";
    }
    SetIniFile(path.u8string().c_str());
    int failures = 0;
    const auto check = [&](const char* label, bool ok) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const auto readId = [&] { return get_config_string(std::string("video"), std::string("starting_monitor_id"), std::string()); };
    check("physical monitor ID retains hash separators", readId() == id);
    check("whitespace-delimited hash comment supported", get_config_int("video", "starting_monitor", 0) == 2);
    check("legacy semicolon comment supported", get_config_int("video", "scale", 0) == 1);
    set_config_string("video", "starting_monitor_id", id.c_str());
    SetIniFile(path.u8string().c_str());
    check("physical ID survives save and reload", readId() == id);
    std::filesystem::remove(path);
    return failures ? 1 : 0;
}
