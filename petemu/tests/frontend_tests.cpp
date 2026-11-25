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
#include <fstream>
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
        check("title contains Unicode em dash", frame->GetTitle() == "Commodore PET " + dash + " Pet-GPT " PETGPT_VERSION);
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
#if defined(__WXGTK__) && defined(GDK_WINDOWING_WAYLAND)
        if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
            command(ScaleFit);
            check("Wayland Fit uses compositor maximization", eventually([&] { return frame->IsMaximized(); }));
            command(Fullscreen);
            command(Fullscreen);
            check("Wayland Fit survives fullscreen roundtrip", eventually([&] { return frame->IsMaximized(); }));
            command(Scale1);
            check("Wayland preset leaves maximization", eventually([&] { return !frame->IsMaximized(); }));
            command(ScaleFit); command(Scale1);
            for (int i = 0; i < 30; ++i) { wxMilliSleep(10); wxTheApp->Yield(true); }
            check("Wayland preset cancels outstanding Fit request", !frame->IsMaximized());
        }
#endif
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
            command(Fullscreen);
            eventually([&] { return frame->IsFullScreen(); });
            canvas->SetFocus();
            wxMouseEvent enter(wxEVT_ENTER_WINDOW); enter.SetPosition(wxPoint(10, 10));
            canvas->GetEventHandler()->ProcessEvent(enter);
            if (frame->IsActive()) {
                // Hides 3 s after the last activity. Late activate/configure
                // events can restart that timer, so poll with a deadline.
                bool kept = true; // the compositor may take focus away mid-test
                auto still = [&] { kept = kept && frame->IsActive() && wxWindow::FindFocus() == canvas; return kept; };
                // wxYield on MSW does not deliver the frame timer here, so tick it directly.
                wxTimer tickSource; wxTimerEvent tick(tickSource);
                auto step = [&] { wxMilliSleep(20); wxTheApp->Yield(true); frame->ProcessWindowEvent(tick); };
                // Wall-clock bounds: Windows rounds short sleeps up to its timer tick.
                const auto entered = std::chrono::steady_clock::now();
                auto since = [&] { return std::chrono::steady_clock::now() - entered; };
                while (since() < std::chrono::milliseconds(2000) && still()) step();
                if (kept) check("fullscreen cursor stays visible before timeout", !frame->CursorHidden());
                bool hidden = false;
                while (!hidden && since() < std::chrono::seconds(7) && still()) { step(); hidden = frame->CursorHidden(); }
                if (!kept) std::puts("NOTE: cursor hide check skipped because the test window lost focus");
                else check("fullscreen hides idle cursor while paused", hidden);
#ifdef __WXGTK__
                // X11 exposes the blank cursor type; Wayland reports every
                // themed cursor as a pixmap, so only the app state is checked there.
                // Without focus the cursor was never hidden (see the note above).
                if (kept && !native_wayland()) {
                    auto* cursor = canvas->GetCursor().GetCursor();
                    check("GTK applies blank cursor", cursor && gdk_cursor_get_cursor_type(cursor) == GDK_BLANK_CURSOR);
                }
#endif
                wxMouseEvent motion(wxEVT_MOTION); motion.SetPosition(wxPoint(100, 100));
                canvas->GetEventHandler()->ProcessEvent(motion);
                check("motion restores fullscreen cursor", !frame->CursorHidden());
            } else std::puts("NOTE: cursor focus test skipped because compositor did not activate the test window");
            command(Fullscreen);
            eventually([&] { return !frame->IsFullScreen(); });
            // Wait for the windowed configure before taking the two PNGs.
            eventually([&] { return frame->GetClientSize() == windowSize; });
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
        // ROM availability: greyed model menu items and the availability query.
        {
            const std::filesystem::path roms = std::filesystem::u8path(std::string(directory.utf8_string())) / "roms";
            const std::filesystem::path set = roms / "cbm8032", hidden = roms / "cbm8032.hidden";
            std::error_code ec;
            check("every ROM set reports available", pet_rom_set_available(1) && pet_rom_set_available(2) && pet_rom_set_available(4) &&
                pet_rom_set_available(12) && pet_rom_set_available(8));
            check("unknown model is not available", !pet_rom_set_available(3));
            check("Model8 enabled when its ROMs exist", frame->GetMenuBar()->IsEnabled(Model8));
            // Availability is cached: commands do not touch the disk, opening a menu
            // checks again.
            auto openMenu = [&] {
                wxMenuEvent open(wxEVT_MENU_OPEN, 0, frame->GetMenuBar()->GetMenu(1));
                open.SetEventObject(frame);
                frame->GetEventHandler()->ProcessEvent(open);
            };
            if (pet_get_basic() != 8) {
                std::filesystem::rename(set, hidden, ec);
                if (!ec) {
                    command(Reset);
                    check("a command does not re-check ROM files", frame->GetMenuBar()->IsEnabled(Model8));
                    openMenu();
                    check("missing ROM set reported unavailable", !pet_rom_set_available(8));
                    check("model without ROMs is greyed out", !frame->GetMenuBar()->IsEnabled(Model8));
                    check("model with ROMs stays enabled", frame->GetMenuBar()->IsEnabled(Model2));
                    std::filesystem::rename(hidden, set, ec);
                    openMenu();
                    check("restored ROM set enabled again", pet_rom_set_available(8) && frame->GetMenuBar()->IsEnabled(Model8));
                } else std::puts("NOTE: could not move the cbm8032 ROM folder; greyed-model check skipped");
            }
        }
        // Loading a program: BASIC autoruns, machine code gets a SYS hint in the title.
        {
            const auto dir = std::filesystem::temp_directory_path() / "pet-gpt-frontend-prg";
            std::filesystem::create_directories(dir);
            auto write = [&](const char* name, std::vector<unsigned char> bytes) {
                const auto path = dir / name;
                std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                return path.u8string();
            };
            // 10 PRINT"HI"
            const std::string basic = write("hello.prg", {0x01, 0x04, 0x0C, 0x04, 0x0A, 0x00, 0x99, 0x22, 0x48, 0x49, 0x22, 0x00, 0x00, 0x00});
            const std::string ml = write("code.prg", {0x00, 0x10, 0xEA, 0x60});
            pet_set_basic(2);
            const bool basic1 = pet_get_basic() == 1;
            const int buffer = basic1 ? 0x020F : 0x026F, count = basic1 ? 0x020D : 0x9E;
            set_config_bool("machine", "autorun", false);
            pet_load_software(basic.c_str());
            check("autorun off leaves the keyboard buffer empty", pet_peek(count) == 0);
            check("BASIC hint says type RUN", frame->GetTitle().Contains("type RUN"));
            set_config_bool("machine", "autorun", true);
            pet_load_software(basic.c_str());
            check("autorun queues RUN + Return", pet_peek(count) == 4 && pet_peek(buffer) == 'R' && pet_peek(buffer + 1) == 'U' &&
                pet_peek(buffer + 2) == 'N' && pet_peek(buffer + 3) == 13);
            for (int i = 0; i < 120; ++i) emu_run_frame();
            check("autorun buffer consumed by the KERNAL", pet_peek(count) == 0);
            bool printed = false;   // screen rows are 40 codes wide; H=8, I=9 at row start
            for (int row = 0; row < 25; ++row) printed = printed || (pet_peek(0x8000 + row * 40) == 8 && pet_peek(0x8000 + row * 40 + 1) == 9);
            check("autorun actually ran the program", printed);
            pet_load_software(ml.c_str());
            check("machine code is not auto-run", pet_peek(count) == 0);
            check("machine code hint names SYS 4096", frame->GetTitle().Contains("SYS 4096"));
            std::filesystem::remove_all(dir);
        }
        // Unsupported files, .p00, drag and drop, File > Recent and the new menu items.
        {
            const auto dir = std::filesystem::temp_directory_path() / "pet-gpt-frontend-recent";
            std::error_code ec; std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir);
            auto write = [&](const char* name, std::vector<unsigned char> bytes) {
                const auto path = dir / name;
                std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                return path.u8string();
            };
            auto full = [](const std::string& path) { return wxFileName(wxString::FromUTF8(path)).GetFullPath().ToStdString(wxConvUTF8); };
            const std::vector<unsigned char> hello = {0x01, 0x04, 0x0C, 0x04, 0x0A, 0x00, 0x99, 0x22, 0x48, 0x49, 0x22, 0x00, 0x00, 0x00};
            const std::string basic = write("hello.prg", hello), ml = write("code.prg", {0x00, 0x10, 0xEA, 0x60});
            set_config_bool("machine", "autorun", false);

            auto fileMenu = [&] { return frame->GetMenuBar()->GetMenu(0); };
            wxMenu* recentMenu = nullptr;
            for (auto* item : fileMenu()->GetMenuItems()) if (item->GetSubMenu() && item->GetItemLabelText() == "Recent") recentMenu = item->GetSubMenu();
            check("File > Recent submenu exists", recentMenu != nullptr);
            check("Open Files/ROM/Screenshots folder items exist", frame->GetMenuBar()->FindItem(OpenFiles) && frame->GetMenuBar()->FindItem(OpenRoms) && frame->GetMenuBar()->FindItem(OpenShots));
            check("Help > Quick Start exists", frame->GetMenuBar()->FindItem(QuickStart) != nullptr);
            const wxString quick = QuickStartText();
            check("Quick Start covers disks, tapes and keys", quick.Contains("LOAD\"$\",8") && quick.Contains("LIST") && quick.Contains("File > Tape > Play") &&
                quick.Contains("Caps Lock") && quick.Contains("Keyboard Mapping") && quick.Contains(wxString::FromUTF8(data_path("roms"))));
            const auto filesFolder = std::filesystem::u8path(data_path("files"));
            command(OpenFiles);
            check("Open Files Folder creates the folder", std::filesystem::is_directory(filesFolder));

            // Recent list: ordering, de-duplication, cap, persistence.
            command(RecentClear);
            check("Clear empties the list", RecentLoad().empty() && !frame->GetMenuBar()->FindItem(RecentClear)->IsEnabled());
            check("loading a PRG succeeds", frame->LoadPath(wxString::FromUTF8(basic)));
            check("loading machine code succeeds", frame->LoadPath(wxString::FromUTF8(ml)));
            check("loading the first again succeeds", frame->LoadPath(wxString::FromUTF8(basic)));
            auto list = RecentLoad();
            check("recent list is most recent first without duplicates", list.size() == 2 && list[0] == full(basic) && list[1] == full(ml));
            SetIniFile(data_path("pet.ini").c_str());
            check("recent list persists in pet.ini [recent]", RecentLoad() == list && get_config_string(std::string("recent"), std::string("file1"), std::string()) == full(basic));
            check("recent menu shows the entries", recentMenu && frame->GetMenuBar()->FindItem(RecentFirst) && frame->GetMenuBar()->FindItem(RecentFirst)->GetHelp() == wxString::FromUTF8(full(basic)) &&
                frame->GetMenuBar()->FindItem(RecentFirst + 1)->GetHelp() == wxString::FromUTF8(full(ml)) && !frame->GetMenuBar()->FindItem(RecentFirst + 2) && frame->GetMenuBar()->FindItem(RecentClear)->IsEnabled());
            for (int i = 0; i < 10; ++i) RecentAdd(full(write(("extra" + std::to_string(i) + ".prg").c_str(), hello)));
            list = RecentLoad();
            check("recent list is capped at 8", list.size() == size_t(kRecentMax) && list[0] == full((dir / "extra9.prg").u8string()));
            RecentSave({}); RecentAdd(full(basic)); RecentAdd(full(ml));
            frame->LoadPath(wxString::FromUTF8(basic)); // rebuild the menu from the ini
            // Choosing an entry loads it through the same path as File > Load, and moves it to the top.
            command(RecentFirst + 1);
            check("Recent entry loads the program", frame->GetTitle().Contains("SYS 4096"));
            check("loaded Recent entry moves to the top", RecentLoad().front() == full(ml));
            // A file that has gone is reported and removed.
            const std::string gone = write("gone.prg", hello);
            frame->LoadPath(wxString::FromUTF8(gone));
            std::filesystem::remove(dir / "gone.prg");
            const int exitBefore = applicationExitCode; lastMessageTitle.clear();
            command(RecentFirst);
            check("missing Recent file is reported", lastMessageTitle == "File not found" && lastMessageText.Contains("gone.prg"));
            list = RecentLoad();
            check("missing Recent file is removed", std::find(list.begin(), list.end(), full(gone)) == list.end() && list.size() == 2);
            check("missing Recent file is not a host error", applicationExitCode == exitBefore);

            // Unsupported extensions leave the machine alone and report an error.
            check("load basic before the unsupported test", pet_load_software(basic.c_str()));
            for (int i = 0; i < 30; ++i) emu_run_frame();
            const wxString noteBefore = frame->GetTitle();
            const int ramBefore = pet_peek(0x2000), linkBefore = pet_peek(0x0401);
            const auto recentBefore = RecentLoad();
            for (const char* name : {"picture.jpg", "notes.txt", "archive.zip"}) {
                const std::string bad = write(name, {0x00, 0x20, 0x5A, 0x5B});
                lastMessageTitle.clear(); applicationExitCode = 0;
                check("unsupported extension is refused", !pet_load_software(bad.c_str()));
                check("unsupported extension reports an error", lastMessageTitle == "Unsupported file type" && applicationExitCode == 1 &&
                    lastMessageText.Contains(wxString(".") + wxFileName(name).GetExt()) && lastMessageText.Contains(".d71"));
                applicationExitCode = 0;
                check("unsupported file did not load or reset", pet_peek(0x2000) == ramBefore && pet_peek(0x0401) == linkBefore && frame->GetTitle() == noteBefore);
                check("unsupported file did not join Recent", !frame->LoadPath(wxString::FromUTF8(bad)) && RecentLoad() == recentBefore);
                applicationExitCode = 0;
            }
            check("extensionless file is accepted", pet_load_software(write("NOEXT", {0x00, 0x10, 0xEA, 0x60}).c_str()) && frame->GetTitle().Contains("SYS 4096"));

            // PC64 .p00: 26-byte header, then a PRG.
            std::vector<unsigned char> p00 = {'C', '6', '4', 'F', 'i', 'l', 'e', 0};
            for (int i = 0; i < 16; ++i) p00.push_back(i < 4 ? 'T' : 0);
            p00.push_back(0); p00.push_back(0);
            p00.insert(p00.end(), {0x00, 0x30, 0x42, 0x43});
            check("p00 loads", pet_load_software(write("game.p00", p00).c_str()));
            check("p00 payload lands at its load address", pet_peek(0x3000) == 0x42 && pet_peek(0x3001) == 0x43 && frame->GetTitle().Contains("SYS 12288"));
            p00[0] = 'X'; lastMessageTitle.clear();
            check("p00 without the C64File header is refused", !pet_load_software(write("bad.p00", p00).c_str()) && lastMessageTitle == "Cannot load P00");
            applicationExitCode = 0;

            // Drag and drop loads after OnDropFiles returns, and the first file wins.
            pet_load_software(basic.c_str());
            auto* target = canvas ? dynamic_cast<ProgramDropTarget*>(canvas->GetDropTarget()) : nullptr;
            check("canvas has the program drop target", target != nullptr);
            if (target) {
                wxArrayString dropped; dropped.Add(wxString::FromUTF8(ml)); dropped.Add(wxString::FromUTF8(basic)); dropped.Add(wxString::FromUTF8(basic));
                check("drop is accepted", target->OnDropFiles(0, 0, dropped));
                check("drop does not load inside OnDropFiles", pet_peek(0x1000) != 0xEA);
                check("dropped file loads afterwards", eventually([&] { return pet_peek(0x1000) == 0xEA; }));
                check("extra dropped files are noted", eventually([&] { return frame->GetTitle().Contains("2 other dropped file(s) ignored"); }));
            }
            command(RecentClear);
            std::filesystem::remove_all(dir, ec);
#ifdef __WXMSW__
            // WM_DEVICECHANGE only raises a flag for poll_joystick(); must be harmless to send.
            SendMessage(static_cast<HWND>(frame->GetHandle()), WM_DEVICECHANGE, 0x0007, 0);
            emu_run_frame();
#endif
            set_config_bool("machine", "autorun", true);
        }
        if (canvas) {
            // Escape while windowed is a PET key: it must not quit or leave anything.
            check("window starts windowed for the Escape test", !frame->IsFullScreen());
            wxKeyEvent escape(wxEVT_KEY_DOWN); escape.m_keyCode = WXK_ESCAPE;
            canvas->GetEventHandler()->ProcessEvent(escape);
            check("Escape while windowed does not close the window", frame != nullptr && applicationExitCode == 0);
            check("Escape while windowed reaches the PET keyboard", IsKeyDown(VK_ESCAPE));
            wxKeyEvent escapeUp(wxEVT_KEY_UP); escapeUp.m_keyCode = WXK_ESCAPE;
            canvas->GetEventHandler()->ProcessEvent(escapeUp);
            check("Escape release reaches the PET keyboard", IsKeyUp(VK_ESCAPE));
            for (int i = 0; i < 3; ++i) check("emulator keeps running with Escape down", emu_run_frame());
            command(Fullscreen);
            RawInput_SetKey(VK_ESCAPE, true);
            canvas->GetEventHandler()->ProcessEvent(escape);
            check("Escape leaves standalone fullscreen", frame && !frame->IsFullScreen());
            check("Escape key consumed", IsKeyUp(VK_ESCAPE));
            check("Escape leaving fullscreen does not close the window", frame != nullptr && applicationExitCode == 0);
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
