// Runs the actual frontend and its event handlers, with isolated ROM/config data.
// Link all application sources except wx/app.cpp (included here) and supply
// PETGPT_DATA_DIR pointing at a writable test directory containing pet.ini/roms.
#include <wx/app.h>
#undef wxIMPLEMENT_APP
#define wxIMPLEMENT_APP(appname) wxIMPLEMENT_APP_NO_MAIN(appname)
#include "../wx/app.cpp"
#undef wxIMPLEMENT_APP
#include <cstdio>
#include <filesystem>
#ifdef __WXMSW__
#include "system/monitor_select.h"
#endif

namespace {
int failures = 0;
void check(const char* name, bool condition) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
bool command(int id) {
    if (!frame) { check("frontend remains alive", false); return false; }
    wxCommandEvent event(wxEVT_MENU, id);
    return frame->GetEventHandler()->ProcessEvent(event);
}
template<class Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        wxTheApp->Yield(true);
        if (predicate()) return true;
        wxMilliSleep(5);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
void verify_model(int menu, int model) {
    check("model command handled", command(menu));
    check("model selected through menu", pet_get_basic() == model);
    if (!frame) return;
    check("model radio check updated", frame->GetMenuBar()->IsChecked(menu));
    if (model == 1) {
        check("PET 2001 fixed 8 KB", pet_get_ram() == 8);
        check("PET 2001 incompatible RAM disabled", !frame->GetMenuBar()->IsEnabled(Ram4) && !frame->GetMenuBar()->IsEnabled(Ram32));
    }
    if (model == 8) {
        check("8032 fixed 32 KB", pet_get_ram() == 32);
        check("8032 incompatible RAM disabled", !frame->GetMenuBar()->IsEnabled(Ram4) && !frame->GetMenuBar()->IsEnabled(Ram8));
    }
}
} // namespace

int main(int argc, char** argv) {
    wxString directory;
    if (!wxGetEnv("PETGPT_DATA_DIR", &directory) || !std::filesystem::is_regular_file(std::filesystem::u8path(std::string(directory.utf8_string())) / "pet.ini")) {
        std::fprintf(stderr, "SKIP: PETGPT_DATA_DIR must name isolated data with writable pet.ini and all five ROM sets.\n");
        return 77;
    }
    std::vector<std::string> testArgs = {argv[0], "--smoke-test"};
#ifdef __WXMSW__
    const auto monitors = monsel::Enumerate();
    // A headless session (CI service account, no display) can enumerate
    // nothing; skip the monitor check there instead of reading past the end.
    const bool haveMonitor = !monitors.empty();
    const monsel::MonitorDesc expectedMonitor = haveMonitor ? monitors.back() : monsel::MonitorDesc{};
    if (haveMonitor) {
        SetIniFile((directory + "/pet.ini").ToStdString(wxConvUTF8).c_str());
        set_config_int("video", "starting_monitor", expectedMonitor.deviceId.empty() ? expectedMonitor.number : 1);
        set_config_string("video", "starting_monitor_id", expectedMonitor.deviceId.c_str());
        // Choose the last physical display, with primary as numeric fallback,
        // so ignoring the physical ID cannot accidentally pass on multi-monitor PCs.
    } else {
        std::fprintf(stderr, "NOTE: no monitors enumerated; skipping the monitor selection check.\n");
    }
#endif
    std::vector<char*> testArgv;
    for (auto& arg : testArgs) testArgv.push_back(arg.data());
    testArgv.push_back(nullptr);
    int testArgc = int(testArgs.size());
    if (!wxEntryStart(testArgc, testArgv.data())) return 1;
    if (!wxFileName::IsFileWritable(directory + "/pet.ini")) {
        std::fprintf(stderr, "SKIP: isolated pet.ini is not writable; refusing user-config fallback.\n");
        wxEntryCleanup(); return 77;
    }
    if (!wxTheApp->CallOnInit()) { wxEntryCleanup(); return 1; }
    // Run the real CallAfter initialization and initial paint. Bounded processing;
    // no polling loop and no modal dialog can block this smoke-mode application.
    wxTheApp->Yield(true);
    check("startup succeeded", frame && applicationExitCode == 0);
#ifdef __WXMSW__
    if (frame && haveMonitor) check("startup uses stable physical monitor selection",
        monsel::FromWindow(static_cast<HWND>(frame->GetHandle())).handle == expectedMonitor.handle);
#endif
    if (frame && applicationExitCode == 0) {
        // Hold the normal menu-open suspension during synchronous assertions so
        // the smoke timer cannot auto-close while a slow test machine runs them.
        wxMenuEvent menuOpen(wxEVT_MENU_OPEN);
        frame->GetEventHandler()->ProcessEvent(menuOpen);
        const wxString dash(static_cast<wchar_t>(0x2014));
        check("title contains Unicode em dash", frame->GetTitle() == "Commodore PET " + dash + " Pet-GPT 2.7");
        const int modelMenu[] = {Model1, Model2, Model4, Model12, Model8};
        const int models[] = {1, 2, 4, 12, 8};
        for (int i = 0; i < 5; ++i) {
            auto* item = frame->GetMenuBar()->FindItem(modelMenu[i]);
            check("model label contains Unicode em dash", item && item->GetItemLabel().Contains(dash));
            verify_model(modelMenu[i], models[i]);
        }
        check("all menu groups present", frame->GetMenuBar()->GetMenuCount() == 4);
        check("file load accelerator retained", frame->GetMenuBar()->FindItem(Load)->GetItemLabel().Contains("Ctrl+O"));
        command(Model2);
        const int ramMenus[] = {Ram4, Ram8, Ram16, Ram32};
        const int ramSizes[] = {4, 8, 16, 32};
        for (int i = 0; i < 4; ++i) {
            command(ramMenus[i]);
            check("RAM command changes machine and menu", pet_get_ram() == ramSizes[i] && frame->GetMenuBar()->IsChecked(ramMenus[i]));
        }
        const int speed = pet_get_speed(), graphics = pet_get_gfx_kbd(), crt = pet_get_crt();
        command(Speed); check("speed toggles", pet_get_speed() != speed); command(Speed);
        check("speed restored", pet_get_speed() == speed);
        command(Graphics); check("graphics keyboard toggles", pet_get_gfx_kbd() != graphics); command(Graphics);
        check("graphics keyboard restored", pet_get_gfx_kbd() == graphics);
        command(Crt); check("CRT toggles", pet_get_crt() != crt); command(Crt);
        check("CRT restored", pet_get_crt() == crt);
        command(White); check("white monitor selected", !pet_get_monitor() && frame->GetMenuBar()->IsChecked(White));
        command(Green); check("green monitor selected", pet_get_monitor() && frame->GetMenuBar()->IsChecked(Green));
        command(CrtSettings);
        CrtDialog* settings = nullptr;
        for (auto* child : frame->GetChildren()) if (auto* dialog = dynamic_cast<CrtDialog*>(child)) settings = dialog;
        check("CRT settings dialog shown", settings && settings->IsShown());
        if (settings) {
            settings->Close();
            check("CRT settings closes without closing frontend", !settings->IsShown() && frame);
            command(CrtSettings); check("CRT settings can reopen", settings->IsShown()); settings->Close();
        }
        command(Scale1);
        wxTheApp->Yield(true);
        const wxSize windowSize = frame->GetClientSize();
        command(Fullscreen); check("fullscreen entered", frame->IsFullScreen());
        command(Fullscreen); check("fullscreen exited", !frame->IsFullScreen());
        check("fullscreen restores client geometry", eventually([&] { return frame && frame->GetClientSize() == windowSize; }));
        // Drive the canvas key event, not a private shortcut implementation.
        wxGLCanvas* canvas = nullptr;
        for (auto* child : frame->GetChildren()) if (auto* gl = dynamic_cast<wxGLCanvas*>(child)) canvas = gl;
        check("GL canvas exists", canvas != nullptr);
        const int pauseId = frame->GetMenuBar()->FindMenuItem("Machine", "Pause (Ctrl+P / Pause)");
        const int screenshotId = frame->GetMenuBar()->FindMenuItem("File", "Save Screenshot (F6)");
        check("pause menu available", pauseId != wxNOT_FOUND);
        check("screenshot menu available", screenshotId != wxNOT_FOUND);
        if (canvas && pauseId != wxNOT_FOUND && screenshotId != wxNOT_FOUND) {
            for (const int id : {pauseId, screenshotId, int(Load), int(Eject), int(Reset),
                                 int(Fullscreen), int(Graphics), int(Crt)}) {
                std::unique_ptr<wxAcceleratorEntry> native(frame->GetMenuBar()->FindItem(id)->GetAccel());
                check("shortcut has no native menu repeat bypass", !native);
            }
            // A held F11 must toggle fullscreen once, not on every repeat.
            const bool fullBefore = frame->IsFullScreen();
            wxKeyEvent f11(wxEVT_KEY_DOWN); f11.m_keyCode = WXK_F11;
            canvas->GetEventHandler()->ProcessEvent(f11);
            wxKeyEvent f11Repeat(f11); f11Repeat.m_isRepeat = true;
            canvas->GetEventHandler()->ProcessEvent(f11Repeat);
            canvas->GetEventHandler()->ProcessEvent(f11Repeat);
            check("held F11 toggles fullscreen once", frame->IsFullScreen() != fullBefore);
            canvas->GetEventHandler()->ProcessEvent(f11);
            check("second F11 press restores the window", frame->IsFullScreen() == fullBefore);
            // Boot a real display so a blank but valid PNG cannot pass.
            for (int i = 0; i < 150; ++i) emu_run_frame();
            wxKeyEvent pauseKey(wxEVT_KEY_DOWN); pauseKey.m_keyCode = WXK_PAUSE;
            canvas->GetEventHandler()->ProcessEvent(pauseKey);
            check("Pause key pauses", frame->GetMenuBar()->IsChecked(pauseId));
            wxKeyEvent repeatedPause(pauseKey); repeatedPause.m_isRepeat = true;
            canvas->GetEventHandler()->ProcessEvent(repeatedPause);
            check("held pause key does not resume", frame->GetMenuBar()->IsChecked(pauseId));
            check("paused title visible", frame->GetTitle().Contains("[Paused]"));
            frame->Pause(); frame->Resume();
            check("temporary suspension preserves user pause", frame->GetMenuBar()->IsChecked(pauseId));
            wxKeyEvent letter(wxEVT_KEY_DOWN); letter.m_keyCode = 'P';
            canvas->GetEventHandler()->ProcessEvent(letter);
            check("paused typing does not queue PET keys", IsKeyUp('P'));

            const auto shots = std::filesystem::u8path(data_path("screenshots"));
            auto countShots = [&] {
                size_t count = 0;
                if (std::filesystem::exists(shots))
                    for (const auto& entry : std::filesystem::directory_iterator(shots))
                        if (entry.path().extension() == ".png") ++count;
                return count;
            };
            const auto before = countShots();
            std::vector<std::filesystem::path> existing;
            if (std::filesystem::exists(shots))
                for (const auto& entry : std::filesystem::directory_iterator(shots)) existing.push_back(entry.path());
            // Release the test's menu suspension. User pause alone must now
            // keep the screen frozen while actual timer events are processed.
            frame->Resume();
            // A GL error left over from earlier rendering must not fail the capture.
            glEnable(0x7FFF); // invalid enum: queues GL_INVALID_ENUM
            command(screenshotId);
            for (int i = 0; i < 30; ++i) { wxMilliSleep(20); wxTheApp->Yield(true); }
            wxKeyEvent shotKey(wxEVT_KEY_DOWN); shotKey.m_keyCode = WXK_F6;
            canvas->GetEventHandler()->ProcessEvent(shotKey);
            wxKeyEvent repeatedShot(shotKey); repeatedShot.m_isRepeat = true;
            canvas->GetEventHandler()->ProcessEvent(repeatedShot);
            frame->Pause(); // Restore the surrounding test's menu suspension.
            check("menu and F6 save separate PNGs", countShots() == before + 2);
            check("screenshots preserve pause", frame->GetMenuBar()->IsChecked(pauseId));
            std::vector<unsigned char> firstPixels;
            if (std::filesystem::exists(shots)) {
                for (const auto& entry : std::filesystem::directory_iterator(shots)) {
                    if (entry.path().extension() != ".png" || std::find(existing.begin(), existing.end(), entry.path()) != existing.end()) continue;
                    wxImage image(wxString::FromUTF8(entry.path().u8string()), wxBITMAP_TYPE_PNG);
                    check("saved screenshot decodes", image.IsOk());
                    if (image.IsOk()) {
                        const double dpi = canvas->GetContentScaleFactor();
                        check("screenshot matches drawable size", image.GetWidth() == int(canvas->GetClientSize().x * dpi) && image.GetHeight() == int(canvas->GetClientSize().y * dpi));
                        const size_t size = size_t(image.GetWidth()) * image.GetHeight() * 3;
                        std::vector<unsigned char> pixels(image.GetData(), image.GetData() + size);
                        check("screenshot contains rendered PET pixels", std::count_if(pixels.begin(), pixels.end(), [](unsigned char c) { return c > 32; }) > 100);
                        if (firstPixels.empty()) firstPixels = pixels;
                        else check("user pause freezes display across timer events", firstPixels == pixels);
                    }
                }
            }
            wxKeyEvent resumeKey(wxEVT_KEY_DOWN); resumeKey.m_keyCode = 'P'; resumeKey.SetControlDown(true);
            canvas->GetEventHandler()->ProcessEvent(resumeKey);
            check("Ctrl+P resumes", !frame->GetMenuBar()->IsChecked(pauseId));
            check("resume clears paused title", !frame->GetTitle().Contains("[Paused]"));
            canvas->GetEventHandler()->ProcessEvent(letter);
            check("plain P still reaches PET input", IsKeyDown('P'));
            RawInput_ReleaseAll();
        }
        if (canvas) {
            command(Fullscreen);
            RawInput_SetKey(VK_ESCAPE, true);
            wxKeyEvent escape(wxEVT_KEY_DOWN); escape.m_keyCode = WXK_ESCAPE;
            canvas->GetEventHandler()->ProcessEvent(escape);
            check("Escape leaves standalone fullscreen", frame && !frame->IsFullScreen());
            check("Escape key consumed", IsKeyUp(VK_ESCAPE));
        }
        check("no host errors during UI operations", applicationExitCode == 0);
        // Do not resume the timer: close through the real close event while the
        // menu suspension is active, exercising shutdown in that state too.
        if (frame) frame->Close();
    } else if (frame) frame->Close();
    wxTheApp->ProcessPendingEvents();
    wxTheApp->OnExit();
    wxEntryCleanup();
    std::printf("Frontend integration failures: %d\n", failures);
    return failures || applicationExitCode ? 1 : 0;
}
