// sys_log: messages logged before LogOpen are kept up to a cap (a log that
// never opens must not grow memory without limit), and the drop is reported
// once the file opens.
#include "sys_log.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    int failures = 0;
    const auto check = [&](const char* label, bool ok) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    Log::setConsoleOutputEnabled(false);   // keep the test's own output readable
    Log::setLevel(Log::Level::Info);
    LOG_DEBUG("below the level, never queued");
    for (int i = 0; i < 5000; ++i) LOG_INFO("early message %d", i);

    const auto path = std::filesystem::temp_directory_path() /
        ("pet-log-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    check("log opens", LogOpen(path.u8string()));
    LOG_INFO("after open");
    LogClose();

    std::ifstream in(path);
    std::string line;
    int early = 0; bool dropNotice = false, afterOpen = false, debug = false;
    while (std::getline(in, line)) {
        if (line.find("early message") != std::string::npos) ++early;
        if (line.find("904 earlier message(s) dropped") != std::string::npos) dropNotice = true;
        if (line.find("after open") != std::string::npos) afterOpen = true;
        if (line.find("below the level") != std::string::npos) debug = true;
    }
    in.close();
    check("pre-open messages kept up to the cap", early == 4096);
    check("dropped count reported", dropNotice);
    check("messages after open written", afterOpen);
    check("level filter applied", !debug);
    std::filesystem::remove(path);
    return failures ? 1 : 0;
}
