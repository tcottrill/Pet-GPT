// Path layout: the Linux split (settings in $XDG_DATA_HOME/pet-gpt, user
// content in ~/Pet-GPT) and the one-time move from the old ~/.Pet-GPT folder.
// Pure filesystem logic, so it runs on every platform.
#include "system/path_helper.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
static int failures = 0;
static void check(const char* label, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
static void touch(const fs::path& p, const char* text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}
static std::string read(const fs::path& p) {
    std::ifstream in(p); std::string s; std::getline(in, s); return s;
}

int main() {
    check("files is user content", pet_paths::is_user_content("files"));
    check("roms subfolder is user content", pet_paths::is_user_content("roms/cbm8032"));
    check("screenshots is user content", pet_paths::is_user_content("screenshots"));
    check("pet.ini is a setting", !pet_paths::is_user_content("pet.ini"));
    check("log is a setting", !pet_paths::is_user_content("petemu-log.txt"));

    fs::path config, user;
    check("split needs a home", !pet_paths::linux_split("", "", &config, &user));
    check("default split", pet_paths::linux_split("/home/deck", "", &config, &user) &&
        config == fs::path("/home/deck/.local/share/pet-gpt") && user == fs::path("/home/deck/Pet-GPT"));
#ifndef _WIN32   // "/data/xdg" is only an absolute path on POSIX
    check("XDG_DATA_HOME honoured", pet_paths::linux_split("/home/deck", "/data/xdg", &config, &user) &&
        config == fs::path("/data/xdg/pet-gpt") && user == fs::path("/home/deck/Pet-GPT"));
#endif
    check("relative XDG_DATA_HOME ignored", pet_paths::linux_split("/home/deck", "rel/xdg", &config, &user) &&
        config == fs::path("/home/deck/.local/share/pet-gpt"));

    const fs::path root = fs::temp_directory_path() /
        ("pet-paths-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    pet_paths::Layout layout{ root / "res", root / "share" / "pet-gpt", root / "Pet-GPT" };
    check("route: programs to the user folder", pet_paths::route(layout, "files") == layout.user / "files");
    check("route: settings to the config folder", pet_paths::route(layout, "pet.ini") == layout.config / "pet.ini");

    // Clean move: everything from the old folder lands in the right place and
    // the old folder goes away.
    const fs::path legacy = root / ".Pet-GPT";
    touch(legacy / "pet.ini", "[video]");
    touch(legacy / "petemu-log.txt");
    touch(legacy / "files" / "game.prg", "game");
    touch(legacy / "roms" / "pet2001" / "kernal.bin", "rom");
    touch(legacy / "screenshots" / "shot.png");
    fs::create_directories(layout.config);
    std::vector<std::string> report;
    pet_paths::migrate_legacy(legacy, layout, &report);
    check("settings moved", read(layout.config / "pet.ini") == "[video]");
    check("log moved with settings", fs::exists(layout.config / "petemu-log.txt"));
    check("programs moved", read(layout.user / "files" / "game.prg") == "game");
    check("ROM sets moved", read(layout.user / "roms" / "pet2001" / "kernal.bin") == "rom");
    check("screenshots moved", fs::exists(layout.user / "screenshots" / "shot.png"));
    check("empty old folder removed", !fs::exists(legacy));
    pet_paths::migrate_legacy(legacy, layout, &report);   // nothing left: a no-op
    check("second run is harmless", read(layout.user / "files" / "game.prg") == "game");

    // Conflicts: nothing is overwritten; new files merge in; leftovers stay.
    touch(legacy / "pet.ini", "[old]");
    touch(legacy / "files" / "game.prg", "old game");
    touch(legacy / "files" / "other.prg", "other");
    report.clear();
    pet_paths::migrate_legacy(legacy, layout, &report);
    check("existing settings not overwritten", read(layout.config / "pet.ini") == "[video]");
    check("existing program not overwritten", read(layout.user / "files" / "game.prg") == "game");
    check("new program merged in", read(layout.user / "files" / "other.prg") == "other");
    check("conflicting items left in the old folder",
        read(legacy / "pet.ini") == "[old]" && read(legacy / "files" / "game.prg") == "old game");
    bool reported = false;
    for (const auto& line : report) if (line.find("left ") == 0) reported = true;
    check("conflicts reported", reported);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("Path layout failures: %d\n", failures);
    return failures ? 1 : 0;
}
