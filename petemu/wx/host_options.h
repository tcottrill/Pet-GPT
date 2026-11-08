#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// Port of HostCmdLine/HostParseCommandLine from system/host_window.cpp.
// Keep established single-dash spellings and invalid-value fallback semantics.
struct HostOptions {
    std::string rom, monitorId;
    int fullscreen = -1, scale = -1, monitor = -1;
    bool monitorIdSet = false, help = false;
};
inline std::string host_lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
inline HostOptions parse_host_options(const std::vector<std::string>& args) {
    HostOptions result;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string option = host_lower(args[i]), value;
        if (option.empty()) continue;
        if (option.rfind("--", 0) == 0) option.erase(0, 1);
        const auto equals = option.find('=');
        if (equals != std::string::npos) { value = args[i].substr(args[i].find('=') + 1); option.resize(equals); }
        const auto argument = [&]() {
            if (equals != std::string::npos) return value;
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (option == "-fullscreen") result.fullscreen = 1;
        else if (option == "-window" || option == "-windowed") result.fullscreen = 0;
        else if (option == "-h" || option == "-help" || option == "-?" || option == "/?") result.help = true;
        else if (option == "-rom") { const auto path = argument(); if (!path.empty()) result.rom = path; }
        else if (option == "-scale") {
            const auto text = host_lower(argument());
            if (!text.empty()) {
                result.scale = text == "fit" ? 0 : std::atoi(text.c_str());
                if (result.scale < 0 || result.scale > 3) result.scale = 0;
            }
        } else if (option == "-monitor") {
            const auto text = argument();
            if (!text.empty()) {
                result.monitor = std::max(1, std::atoi(text.c_str()));
                result.monitorId.clear(); result.monitorIdSet = false;
            }
        } else if (option == "-monitorid") { result.monitorId = argument(); result.monitorIdSet = true; }
        else if (option == "-disk") (void)argument(); // core consumes this option
        else if (args[i][0] != '-' && result.rom.empty()) result.rom = args[i];
    }
    return result;
}

// Original HostEffectiveScale policy, with platform-independent client sizes.
inline int host_effective_scale(int requested, int availableW, int availableH, int baseW, int baseH) {
    for (int n = std::min(requested, 3); n >= 1; --n)
        if (baseW * n <= availableW && baseH * n <= availableH) return n;
    return 0;
}
