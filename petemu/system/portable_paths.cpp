// Copyright Tim Cottrill 2025; portable host paths, 2026. Public domain.
#include "path_helper.h"
#include "iniFile.h"
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <filesystem>
#include <cstdlib>
namespace { std::filesystem::path resources, data; }
void initialize_paths(const std::string& executable) {
    namespace fs=std::filesystem;
    resources=fs::u8path(executable).parent_path();
    wxString overridePath;
    const bool overridden=wxGetEnv("PETGPT_DATA_DIR", &overridePath) && !overridePath.empty();
    if(overridden) resources=fs::u8path(std::string(overridePath.utf8_string()));
    else if(!fs::exists(resources/"roms") && fs::exists(resources/"../share/pet-gpt"))
        resources=fs::weakly_canonical(resources/"../share/pet-gpt");
    const auto user=fs::u8path(std::string(wxStandardPaths::Get().GetUserDataDir().utf8_string()));
    const auto portableIni=resources/"pet.ini";
    const bool portable=fs::exists(portableIni) && wxFileName::IsFileWritable(wxString::FromUTF8(portableIni.u8string()));
    data=(portable || overridden) ? resources : user;
    fs::create_directories(data/"files");
    SetIniFile((data/"pet.ini").u8string().c_str());
}
std::string resource_path(const std::string& relative) {
    const auto local=data/std::filesystem::u8path(relative);
    return (std::filesystem::exists(local) ? local : resources/std::filesystem::u8path(relative)).u8string();
}
std::string data_path(const std::string& relative) { return (data/std::filesystem::u8path(relative)).u8string(); }
std::string getpathM(const char* dir,const char* file) {
    auto p=resources;
    if(dir) p/=std::filesystem::u8path(dir);
    if(file) p/=std::filesystem::u8path(file);
    return p.u8string();
}
std::wstring getpathU(const char* dir,const char* file) { return wxString::FromUTF8(getpathM(dir,file)).ToStdWstring(); }
