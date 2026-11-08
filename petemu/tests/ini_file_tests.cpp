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

    const std::string spaced = R"(C:\Users\Jane Smith\My Programs)";
    const auto readDir = [&] { return get_config_string(std::string("paths"), std::string("lastromdir"), std::string()); };
    set_config_string("paths", "lastromdir", spaced.c_str());
    SetIniFile(path.u8string().c_str());
    check("value with spaces survives save and reload", readDir() == spaced);
    set_config_string("paths", "lastromdir", spaced.c_str());
    SetIniFile(path.u8string().c_str());
    const auto countLines = [&](const std::string& prefix) {
        std::ifstream in(path); std::string line; int n = 0;
        while (std::getline(in, line)) if (line.rfind(prefix, 0) == 0) ++n;
        return n;
    };
    check("repeated saves do not duplicate a spaced value", countLines("lastromdir=") == 1);

    {
        // Duplicates left by older builds: the last line is the newest value.
        std::ofstream out(path, std::ios::binary);
        out << "[paths]\r\nlastromdir=C:\\Old Dir\r\nlastromdir=C:\\New Dir\r\n"
            << "  [video]  \r\nfullscreen=True\r\n";
    }
    SetIniFile(path.u8string().c_str());
    check("duplicate keys resolve to the last value", readDir() == R"(C:\New Dir)");
    check("CRLF section header parses", get_config_bool("video", "fullscreen", false));
    check("indented section header parses", get_config_bool(std::string("video"), std::string("fullscreen"), false));
    set_config_int("video", "scale", 3);
    check("merged duplicates are written once", countLines("lastromdir=") == 1);
    check("save leaves no temp file", !std::filesystem::exists(path.string() + ".tmp"));
    std::filesystem::remove(path);

    // Non-ASCII folder: the path is UTF-8 and must not go through the ANSI code page.
    const auto folder = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"pet-ini-éß日");
    std::filesystem::create_directories(folder);
    const auto unicodeIni = folder / "pet.ini";
    SetIniFile(unicodeIni.u8string().c_str());
    set_config_int("video", "scale", 2);
    check("non-ASCII path saves", std::filesystem::exists(unicodeIni));
    SetIniFile(unicodeIni.u8string().c_str());
    check("non-ASCII path reloads", get_config_int("video", "scale", 0) == 2);
    std::filesystem::remove_all(folder);
    return failures ? 1 : 0;
}
