#pragma once
#include <filesystem>
#include <string>
#include <vector>
void initialize_paths(const std::string& executable);
std::string resource_path(const std::string& relative);
std::string data_path(const std::string& relative);

std::wstring getpathU(const char* dir, const char* file);
std::string getpathM(const char* dir, const char* file);

// Path layout, exposed for tests. initialize_paths() is the only production
// caller.
namespace pet_paths {
// config: settings and the log. user: what people add to and look at -
// files/ (programs and disk images), roms/, screenshots/. In portable mode,
// with PETGPT_DATA_DIR, and on Windows the two are the same folder. On Linux
// they split the way AAE (and Dolphin) do, because the folder the program
// lives in - and wx's ~/.Pet-GPT - is hidden on SteamOS:
//   $XDG_DATA_HOME/pet-gpt (default ~/.local/share/pet-gpt)   pet.ini, log
//   ~/Pet-GPT                                                files, roms, screenshots
struct Layout {
    std::filesystem::path resources, config, user;
};
// True for the top-level names that belong in the user folder.
bool is_user_content(const std::string& relative);
// Where data_path(relative) lives for this layout.
std::filesystem::path route(const Layout& layout, const std::string& relative);
// Linux split layout from HOME and XDG_DATA_HOME (empty or relative
// XDG_DATA_HOME means the default). Empty home: no split possible.
bool linux_split(const std::string& home, const std::string& xdgDataHome,
                 std::filesystem::path* config, std::filesystem::path* user);
// One-time move of an older single-folder layout (~/.Pet-GPT) into the split
// layout. Never overwrites: anything whose destination already exists is left
// where it is and reported. The legacy folder is removed only once empty.
void migrate_legacy(const std::filesystem::path& legacy, const Layout& layout,
                    std::vector<std::string>* report);
}
