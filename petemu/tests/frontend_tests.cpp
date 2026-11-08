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
    char smoke[] = "--smoke-test";
    char* testArgv[] = {argv[0], smoke, nullptr};
    int testArgc = 2;
    if (!wxEntryStart(testArgc, testArgv)) return 1;
    if (!wxFileName::IsFileWritable(directory + "/pet.ini")) {
        std::fprintf(stderr, "SKIP: isolated pet.ini is not writable; refusing user-config fallback.\n");
        wxEntryCleanup(); return 77;
    }
    if (!wxTheApp->CallOnInit()) { wxEntryCleanup(); return 1; }
    // Run the real CallAfter initialization and initial paint. Bounded processing;
    // no polling loop and no modal dialog can block this smoke-mode application.
    wxTheApp->Yield(true);
    check("startup succeeded", frame && applicationExitCode == 0);
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
