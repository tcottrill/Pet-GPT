// Copyright Tim Cottrill 2025; portable host paths, 2026. Public domain.
#include "path_helper.h"
#include "iniFile.h"
#include "sys_log.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;
namespace { pet_paths::Layout layout; }

namespace pet_paths {
bool is_user_content(const std::string& relative) {
    const fs::path p = fs::u8path(relative);
    if (p.empty()) return false;
    const std::string first = p.begin()->u8string();
    return first == "files" || first == "roms" || first == "screenshots";
}

fs::path route(const Layout& l, const std::string& relative) {
    return (is_user_content(relative) ? l.user : l.config) / fs::u8path(relative);
}

bool linux_split(const std::string& home, const std::string& xdgDataHome, fs::path* config, fs::path* user) {
    if (home.empty()) return false;
    const fs::path homePath = fs::u8path(home);
    // XDG requires an absolute path; anything else is ignored, as the spec says.
    const fs::path xdg = xdgDataHome.empty() ? fs::path() : fs::u8path(xdgDataHome);
    const fs::path dataHome = xdg.is_absolute() ? xdg : homePath / ".local" / "share";
    *config = dataHome / "pet-gpt";
    *user = homePath / "Pet-GPT";
    return true;
}

void migrate_legacy(const fs::path& legacy, const Layout& l, std::vector<std::string>* report) {
    std::error_code ec;
    if (legacy.empty() || !fs::is_directory(legacy, ec) || fs::equivalent(legacy, l.config, ec)) return;
    const auto note = [&](const std::string& text) { if (report) report->push_back(text); };
    const auto move = [&](const fs::path& from, const fs::path& to) {
        std::error_code mec;
        fs::create_directories(to.parent_path(), mec);
        fs::rename(from, to, mec);
        if (mec) note("could not move " + from.u8string() + " to " + to.u8string() + ": " + mec.message());
        else note("moved " + from.u8string() + " to " + to.u8string());
    };
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(legacy, ec)) entries.push_back(entry.path());
    for (const auto& from : entries) {
        const std::string name = from.filename().u8string();
        const fs::path to = route(l, name);
        std::error_code sec;
        if (!fs::exists(fs::symlink_status(to, sec))) { move(from, to); continue; }
        // Both sides hold a files/ (or roms/...) folder: merge one level down,
        // still never replacing anything that is already there.
        if (is_user_content(name) && fs::is_directory(from, sec) && fs::is_directory(to, sec)) {
            std::vector<fs::path> children;
            for (const auto& child : fs::directory_iterator(from, sec)) children.push_back(child.path());
            for (const auto& child : children) {
                const fs::path dest = to / child.filename();
                if (fs::exists(fs::symlink_status(dest, sec))) note("left " + child.u8string() + " in place: " + dest.u8string() + " already exists");
                else move(child, dest);
            }
            std::error_code rec;
            if (fs::is_empty(from, rec)) fs::remove(from, rec);
            continue;
        }
        note("left " + from.u8string() + " in place: " + to.u8string() + " already exists");
    }
    if (fs::is_empty(legacy, ec)) {
        fs::remove(legacy, ec);
        note("removed the empty old folder " + legacy.u8string());
    } else {
        note("the old folder " + legacy.u8string() + " still holds the items above; move or delete them by hand");
    }
}
}

namespace {
#ifndef _WIN32
std::string environment(const char* name) {
    wxString value;
    return wxGetEnv(name, &value) ? std::string(value.utf8_string()) : std::string();
}

void write_user_readme(const fs::path& user, const fs::path& config) {
    const fs::path readme = user / "README.txt";
    std::error_code ec;
    if (fs::exists(readme, ec)) return;
    std::ofstream out(readme);
    out << "Pet-GPT - your PET programs, ROMs and screenshots\n"
           "=================================================\n\n"
           "files/        Programs and disk images (.prg .t64 .tap .d64 .d71).\n"
           "              This folder is the emulated drive 8: LOAD\"$\",8 lists it.\n"
           "roms/         The PET ROM sets, one folder per model:\n"
           "                pet2001  pet2001n  pet4000-9  pet4000-12  cbm8032\n"
           "              The README in the Pet-GPT distribution lists the file names.\n"
           "screenshots/  F6 saves a PNG here.\n\n"
           "Settings (pet.ini) and the log are kept separately, in\n"
        << config.u8string() << ",\n"
           "so resetting them never touches anything here.\n";
}
#endif
}

void initialize_paths(const std::string& executable) {
    std::vector<std::string> report;
    layout.resources = fs::u8path(executable).parent_path();
    wxString overridePath;
    const bool overridden = wxGetEnv("PETGPT_DATA_DIR", &overridePath) && !overridePath.empty();
    std::error_code ec;
    if (overridden) layout.resources = fs::u8path(std::string(overridePath.utf8_string()));
    else if (!fs::exists(layout.resources / "roms", ec) && fs::exists(layout.resources / "../share/pet-gpt", ec))
        layout.resources = fs::weakly_canonical(layout.resources / "../share/pet-gpt", ec);
    const auto legacyUser = fs::u8path(std::string(wxStandardPaths::Get().GetUserDataDir().utf8_string()));
    const auto portableIni = layout.resources / "pet.ini";
    const bool portable = fs::exists(portableIni, ec) && wxFileName::IsFileWritable(wxString::FromUTF8(portableIni.u8string()));

    if (portable || overridden) {
        layout.config = layout.user = layout.resources;
    } else {
        layout.config = layout.user = legacyUser;   // Windows: %APPDATA%\Pet-GPT
#ifndef _WIN32
        fs::path config, user;
        if (pet_paths::linux_split(environment("HOME"), environment("XDG_DATA_HOME"), &config, &user)) {
            layout.config = config;
            layout.user = user;
            fs::create_directories(layout.config, ec);
            pet_paths::migrate_legacy(legacyUser, layout, &report);
            fs::create_directories(layout.user / "roms", ec);
            write_user_readme(layout.user, layout.config);
        }
#endif
    }
    fs::create_directories(layout.user / "files", ec);
    if (ec) report.push_back("could not create " + (layout.user / "files").u8string() + ": " + ec.message());
    SetIniFile((layout.config / "pet.ini").u8string().c_str());

    // The log opens after this returns; sys_log keeps these until it does.
    LOG_INFO("Paths: resources %s, settings %s, user files %s", layout.resources.u8string().c_str(),
             layout.config.u8string().c_str(), layout.user.u8string().c_str());
    for (const auto& line : report) LOG_INFO("Paths: %s", line.c_str());
}

std::string resource_path(const std::string& relative) {
    std::error_code ec;
    const auto local = pet_paths::route(layout, relative);
    return (fs::exists(local, ec) ? local : layout.resources / fs::u8path(relative)).u8string();
}
std::string data_path(const std::string& relative) { return pet_paths::route(layout, relative).u8string(); }
std::string getpathM(const char* dir,const char* file) {
    auto p=layout.resources;
    if(dir) p/=fs::u8path(dir);
    if(file) p/=fs::u8path(file);
    return p.u8string();
}
std::wstring getpathU(const char* dir,const char* file) { return wxString::FromUTF8(getpathM(dir,file)).ToStdWstring(); }
