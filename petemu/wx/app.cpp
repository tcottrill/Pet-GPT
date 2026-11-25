// The UI thread owns machine mutations and the OpenGL context. Paint events
// only redraw; machine time is advanced by the bounded monotonic scheduler.
#include "glew.h"
#include <wx/wx.h>
#include <wx/glcanvas.h>
#include <wx/display.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/aboutdlg.h>
#include <wx/choicdlg.h>
#include <wx/dnd.h>
#include <wx/file.h>
#include <wx/wfstream.h>
#include <wx/filefn.h>
#include <wx/utils.h>
#include <wx/process.h>
#include <wx/progdlg.h>
#include <wx/stream.h>
#include "emulator.h"
#include "iniFile.h"
#include "system/path_helper.h"
#include "system/host_services.h"
#include "sys_audio/mixer.h"
#include "sys_input/rawinput.h"
#include "sys_input/joystick.h"
#include "petsrc/pet_keyboard_dialog.h"
#include "keyboard_input.h"
#include "crt_dialog.h"
#include "frame_schedule.h"
#include "fullscreen_cursor.h"
#include "host_options.h"
#include "system/host_view.h"
#include "sys_log.h"
#ifdef __WXMSW__
#include "system/monitor_select.h"
#endif
#ifdef __WXGTK__
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif
#include <algorithm>
#include <chrono>
#include <memory>
#include <functional>
#include <vector>

namespace {
enum {
    Load = wxID_HIGHEST + 100, Eject, Reset, Fullscreen, Scale1, Scale2, Scale3, ScaleFit,
    Model1, Model2, Model4, Model12, Model8, Ram4, Ram8, Ram16, Ram32,
    Speed, Graphics, Snes, Keyboard, Crt, Green, White, CrtSettings,
    TapePlay, TapeStop, TapeRewind, TapeEject, TogglePause, Screenshot,
    OpenFiles, OpenRoms, OpenShots, QuickStart, RecentClear, RecentFirst
};
constexpr int kRecentMax = 8;
wxString lastMessageTitle, lastMessageText; // most recent host_message, for tests
class PetFrame;
PetFrame* frame = nullptr;
bool smokeMode = false;
int applicationExitCode = 0;
bool native_wayland() {
#if defined(__WXGTK__) && defined(GDK_WINDOWING_WAYLAND)
    return GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
#else
    return false;
#endif
}
const char* knobNames[] = {"mono_blur_h", "mono_blur_v", "mono_halation", "mono_halation_radius",
                          "mono_scanline", "mono_contrast", "mono_brightness"};
class ProgramDropTarget final : public wxFileDropTarget {
public:
    explicit ProgramDropTarget(std::function<void(const wxString&, size_t)> load) : load_(std::move(load)) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& paths) override {
        if (paths.empty()) return false;
        load_(paths[0], paths.size()); return true;
    }
private:
    std::function<void(const wxString&, size_t)> load_;
};

// File > Recent: [recent] file1..file8 in pet.ini, most recent first.
std::string RecentKey(int i) { return "file" + std::to_string(i); }
bool SamePath(const std::string& a, const std::string& b) {
#ifdef __WXMSW__
    return wxString::FromUTF8(a).CmpNoCase(wxString::FromUTF8(b)) == 0;
#else
    return a == b;
#endif
}
std::vector<std::string> RecentLoad() {
    std::vector<std::string> list;
    for (int i = 1; i <= kRecentMax; ++i) {
        const std::string path = get_config_string(std::string("recent"), RecentKey(i), std::string());
        if (!path.empty() && std::none_of(list.begin(), list.end(), [&](const std::string& p) { return SamePath(p, path); })) list.push_back(path);
    }
    return list;
}
void RecentSave(const std::vector<std::string>& list) {
    for (int i = 1; i <= kRecentMax; ++i) {
        const std::string value = size_t(i) <= list.size() ? list[size_t(i) - 1] : std::string();
        if (get_config_string(std::string("recent"), RecentKey(i), std::string()) != value) set_config_string(std::string("recent"), RecentKey(i), value);
    }
}
void RecentRemove(const std::string& path) {
    auto list = RecentLoad();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) { return SamePath(p, path); }), list.end());
    RecentSave(list);
}
void RecentAdd(const std::string& path) {
    auto list = RecentLoad();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) { return SamePath(p, path); }), list.end());
    list.insert(list.begin(), path);
    if (list.size() > size_t(kRecentMax)) list.resize(size_t(kRecentMax));
    RecentSave(list);
}

wxString QuickStartText() {
    const wxString files = wxString::FromUTF8(data_path("files")), roms = wxString::FromUTF8(data_path("roms")),
                   shots = wxString::FromUTF8(data_path("screenshots"));
    return "LOADING A PROGRAM\n"
        "File > Load Program / Disk / Tape... (Ctrl+O), or drag a file onto the window. Pet-GPT loads .prg, .p00 (to .p99), .t64, .tap, .d64 and .d71 "
        "files; the last eight are under File > Recent.\n"
        "A BASIC program starts by itself: Pet-GPT loads it and types RUN for you. A machine-code program names the SYS command that "
        "starts it in the window title.\n\n"
        "DISK IMAGES (.d64, .d71)\n"
        "Loading a disk image mounts it as drive 8; nothing runs yet. Type LOAD\"$\",8 and press Return, then type LIST to see the "
        "directory. To run a program, type LOAD\"NAME\",8 and press Return, then type RUN. LOAD\"*\",8 loads the first program. "
        "File > Eject Disk (Ctrl+E) goes back to the files folder, whose programs also load by name with LOAD\"NAME\",8.\n\n"
        "TAPES (.tap)\n"
        "Loading a tape attaches it to the cassette. Type LOAD and press Return, then choose File > Tape > Play. When the tape has "
        "finished, type RUN. File > Tape also has Stop, Rewind and Eject.\n\n"
        "PET KEYS ON A PC KEYBOARD\n"
        "RUN/STOP: Caps Lock (Caps Lock + Shift is BREAK)\n"
        "Cursor keys: the arrow keys\n"
        "HOME / CLR: Home / Shift+Home\n"
        "DEL: Backspace or Delete\n"
        "RETURN: Enter\n"
        "Graphics characters: F12 switches between the graphics and business keyboards\n"
        "Esc: leaves fullscreen; in a window it is the PET's ESC key. Pause or Ctrl+P pauses, Ctrl+R resets, F6 saves a screenshot.\n"
        "Machine > Keyboard Mapping... shows the whole layout and lets you change any key.\n\n"
        "WHERE THINGS ARE (File > Open ... Folder opens them)\n"
        "Programs and disk images: " + files + "\nROM sets: " + roms + "\nScreenshots: " + shots;
}
void OpenFolder(const wxString& folder) {
    if (!wxDirExists(folder) && !wxFileName::Mkdir(folder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
        host_message("Open folder", ("Unable to create the folder " + folder.ToStdString(wxConvUTF8)).c_str(), true); return;
    }
    if (!smokeMode && !wxLaunchDefaultApplication(folder))
        host_message("Open folder", ("Unable to open the folder " + folder.ToStdString(wxConvUTF8)).c_str(), true);
}

class QuickStartDialog final : public wxDialog {
public:
    explicit QuickStartDialog(wxWindow* parent) : wxDialog(parent, wxID_ANY, "Quick Start", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* top = new wxBoxSizer(wxVERTICAL);
        auto* text = new wxTextCtrl(this, wxID_ANY, QuickStartText(), wxDefaultPosition, FromDIP(wxSize(620, 480)), wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        top->Add(text, 1, wxEXPAND | wxALL, FromDIP(10));
        top->Add(CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
        SetSizerAndFit(top); CentreOnParent();
    }
};


const char* const romSource = "https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/"; // what download-roms.ps1 fetches from
// Next to the executable (build tree, Windows install, portable zip), in a
// tools sub-folder, or in ../share/pet-gpt (the Linux install layout).
wxString FindRomScript() {
    const wxString dir = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    const wxString sep = wxFileName::GetPathSeparator();
    for (const wxString& folder : {dir, dir + sep + "tools", dir + sep + ".." + sep + "share" + sep + "pet-gpt"}) {
        wxFileName path(folder, "download-roms.ps1"); path.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
        if (path.FileExists()) return path.GetFullPath();
    }
    return {};
}

// The downloader's process: output is read through pipes (there is no console
// window on Linux, and none is wanted on Windows either), and OnTerminate
// records the exit code. If the dialog gives up on a process that has not
// ended, Abandon() hands ownership to the process itself.
class RomDownloadProcess final : public wxProcess {
public:
    RomDownloadProcess() : wxProcess(wxPROCESS_REDIRECT) {}
    bool done = false;
    int exitCode = -1;
    void Abandon() { abandoned_ = true; }
    void OnTerminate(int, int status) override {
        if (abandoned_) { delete this; return; }
        done = true; exitCode = status;
    }
private:
    bool abandoned_ = false;
};
wxString FindPowerShell() {
    wxPathList paths; paths.AddEnvList("PATH");
#ifdef __WXMSW__
    wxString found = paths.FindValidPath("powershell.exe");
    wxString root;
    if (found.empty() && wxGetEnv("SystemRoot", &root)) {
        const wxString fallback = root + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (wxFileExists(fallback)) found = fallback;
    }
    return found;
#else
    return paths.FindValidPath("pwsh");
#endif
}

// Shown when no ROM set can be loaded: explains where ROMs go and offers the
// bundled downloader, so a first run is never a dead end.
class RomSetupDialog final : public wxDialog {
public:
    RomSetupDialog(wxWindow* parent, const wxString& folder)
        : wxDialog(parent, wxID_ANY, "ROM setup", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
          folder_(folder), script_(FindRomScript()), shell_(FindPowerShell()) {
        auto* top = new wxBoxSizer(wxVERTICAL);
        const int width = FromDIP(520);
        auto text = [&](const wxString& label) {
            auto* line = new wxStaticText(this, wxID_ANY, label);
            line->Wrap(width); top->Add(line, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        };
        text("Pet-GPT needs Commodore PET ROM files to start, and they are not included: they are Commodore's copyrighted "
             "software, so you add them yourself, once.\n\nPet-GPT reads ROMs from this folder:");
        auto* path = new wxTextCtrl(this, wxID_ANY, folder_, wxDefaultPosition, wxSize(width, -1), wxTE_READONLY);
        top->Add(path, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        text("Each PET model has its own sub-folder there:\n"
             "    pet2001  (PET 2001, BASIC 1)\n    pet2001n  (PET 2001-N, BASIC 2)\n    pet4000-9  (PET 4000, BASIC 4)\n"
             "    pet4000-12  (PET 4032, 12-inch CRTC)\n    cbm8032  (CBM 8032, 80 columns)\n"
             "One complete model is enough to start; the README lists the exact file names.");
        canDownload_ = !script_.empty() && !shell_.empty();
        text(canDownload_
            ? wxString("Download ROMs fetches them for you from zimmers.net, the public Commodore firmware archive, and shows its "
                       "progress (an internet connection is needed).")
            : wxString("Automatic download is not available here (download-roms.ps1 or PowerShell was not found). Download the .bin files "
                       "yourself from zimmers.net (") + romSource + ") and copy them into the sub-folders above.");
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* download = new wxButton(this, wxID_ANY, "&Download ROMs");
        auto* open = new wxButton(this, wxID_ANY, "&Open ROM folder");
        auto* retry = new wxButton(this, wxID_OK, "&Try again");
        auto* quit = new wxButton(this, wxID_CANCEL, "E&xit");
        download->Enable(canDownload_);
        for (auto* button : {download, open, retry, quit}) buttons->Add(button, 0, wxRIGHT, FromDIP(6));
        top->Add(buttons, 0, wxALL | wxALIGN_RIGHT, FromDIP(10));
        SetSizerAndFit(top); CentreOnParent();
        download->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Download(); });
        open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxFileName::Mkdir(folder_, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
            wxLaunchDefaultApplication(folder_);
        });
        retry->SetDefault();
    }
private:
    wxString folder_, script_, shell_;
    bool canDownload_ = false;
    void Download() {
        wxFileName::Mkdir(folder_, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        auto quoted = [](const wxString& text) { return "\"" + text + "\""; };
        wxString command = quoted(shell_) + " -NoProfile";
#ifdef __WXMSW__
        command += " -ExecutionPolicy Bypass";
#endif
        command += " -File " + quoted(script_) + " -RomsDir " + quoted(folder_);
        LOG_INFO("Running ROM downloader: %s -> %s", script_.ToStdString(wxConvUTF8).c_str(), folder_.ToStdString(wxConvUTF8).c_str());
        // Asynchronous, with the script's own progress lines in a progress box:
        // a synchronous run left the window frozen and silent (on Linux there is
        // no console to show), with no way to cancel.
        auto* process = new RomDownloadProcess;
        const long pid = wxExecute(command, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, process);
        if (pid <= 0) {
            delete process;
            wxMessageBox(wxString::Format("The downloader could not be started. Download the files by hand from zimmers.net (%s).", romSource),
                         "ROM download", wxOK | wxICON_WARNING, this);
            return;
        }
        wxString line, last = "Contacting zimmers.net...", failures;
        auto drain = [&](wxInputStream* in) {
            while (in && in->CanRead()) {
                const int c = in->GetC();
                if (in->LastRead() == 0) break;
                if (c == '\n' || c == '\r') {
                    line.Trim(); line.Trim(false);
                    if (!line.empty()) {
                        last = line;
                        if (line.StartsWith("FAIL")) failures += line + "\n";
                        LOG_INFO("ROM download: %s", line.ToStdString(wxConvUTF8).c_str());
                    }
                    line.clear();
                } else line += wxUniChar(static_cast<unsigned char>(c));
            }
        };
        bool cancelled = false;
        {
            wxProgressDialog progress("ROM download", "Downloading the PET ROMs from zimmers.net...\n\n", 100, this,
                                      wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_AUTO_HIDE);
            while (!process->done) {
                drain(process->GetInputStream()); drain(process->GetErrorStream());
                if (!progress.Pulse("Downloading the PET ROMs from zimmers.net...\n\n" + last)) {
                    cancelled = true;
                    wxProcess::Kill(static_cast<int>(pid), wxSIGTERM, wxKILL_CHILDREN);
                    break;
                }
                wxTheApp->Yield(true);   // delivers the end-of-process notification
                wxMilliSleep(30);
            }
            // After a cancel, give the process a moment to end before letting go of it.
            for (int i = 0; cancelled && !process->done && i < 100; ++i) { wxTheApp->Yield(true); wxMilliSleep(20); }
        }
        if (!process->done) { process->Abandon(); process = nullptr; }
        long result = -1;
        if (process) {
            drain(process->GetInputStream()); drain(process->GetErrorStream());
            result = process->exitCode;
            delete process;
        }
        LOG_INFO("ROM downloader finished: %s (code %ld)", cancelled ? "cancelled" : "done", result);
        if (cancelled) return;
        if (result == 0) { EndModal(wxID_OK); return; } // retry loading
        wxMessageBox(wxString::Format("The download did not finish (code %ld). Check your internet connection and try again, or download the "
                                      "files by hand from zimmers.net (%s).%s", result, romSource,
                                      failures.empty() ? wxString() : "\n\n" + failures),
                     "ROM download", wxOK | wxICON_WARNING, this);
    }
};

class PetFrame final : public wxFrame {
public:
    PetFrame(std::vector<std::string> args, wxString screenshot, bool smoke)
        : wxFrame(nullptr, wxID_ANY, wxS("Commodore PET — Pet-GPT 2.7")), timer_(this),
          arguments_(std::move(args)), screenshot_(screenshot), smoke_(smoke) {
        frame = this;
#ifdef __WXMSW__
        SetIcon(wxIcon("appicon", wxBITMAP_TYPE_ICO_RESOURCE));
#endif
        BuildMenus();
        wxGLAttributes attrs;
        attrs.PlatformDefaults().RGBA().DoubleBuffer().Depth(0).EndList();
        canvas_ = new wxGLCanvas(this, attrs);
        wxGLContextAttrs contextAttrs;
        contextAttrs.PlatformDefaults().CoreProfile().OGLVersion(3, 3).EndList();
        context_ = std::make_unique<wxGLContext>(canvas_, nullptr, &contextAttrs);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(canvas_, 1, wxEXPAND); SetSizer(layout);
        ram_ = get_config_int("machine", "ram", 32);
        scale_ = get_config_int("video", "scale", 2);
        if (scale_ < 0 || scale_ > 3) scale_ = 2;
        int monitor = get_config_int("video", "starting_monitor", 1);
        bool fullscreen = get_config_bool("video", "fullscreen", false);
        std::string monitorName = get_config_string(std::string("video"), std::string("starting_monitor_id"), std::string());
        const auto options = parse_host_options(arguments_);
        startupProgram_ = options.rom;
        if (options.scale >= 0) scale_ = options.scale;
        if (options.fullscreen >= 0) fullscreen = options.fullscreen != 0;
        if (options.monitor > 0) { monitor = options.monitor; monitorName.clear(); }
        if (options.monitorIdSet) monitorName = options.monitorId;
#ifdef __WXMSW__
        // G-Sync/driver re-enumeration changes wx/GDI display order and names.
        // Reuse AAE's layout numbering and physical-device-path selection.
        std::string reason;
        const auto selected = monsel::Select(monitor, monitorName, &reason);
        LOG_INFO("Monitor list:\n%s", monsel::DescribeAll().c_str());
        LOG_INFO("Starting monitor %d: %s", selected.number, reason.c_str());
        Move(wxPoint(selected.workRect.left, selected.workRect.top) + wxPoint(FromDIP(24), FromDIP(24)));
#else
        --monitor; // wx display indexes are zero-based on the portable path.
        if (!monitorName.empty()) {
            for (unsigned i = 0; i < wxDisplay::GetCount(); ++i)
                if (host_lower(wxDisplay(i).GetName().ToStdString(wxConvUTF8)) == host_lower(monitorName)) monitor = static_cast<int>(i);
        }
        if (monitor < 0 || monitor >= static_cast<int>(wxDisplay::GetCount())) monitor = 0;
        if (wxDisplay::GetCount()) {
            const wxRect work = wxDisplay(static_cast<unsigned>(monitor)).GetClientArea();
            Move(work.GetTopLeft() + wxPoint(FromDIP(24), FromDIP(24)));
        }
#endif
        ApplyScale(scale_);
        canvas_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(canvas_);
            if (ready_ && !closing_) { PrepareViewport(); emu_present(); canvas_->SwapBuffers(); }
        });
        canvas_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { canvas_->Refresh(false); event.Skip(); });
        canvas_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { if (!HandleShortcut(event) && !userPaused_) pet_keyboard_handle_wx(event, true); });
        canvas_->Bind(wxEVT_CHAR, [this](wxKeyEvent& event) { if (!userPaused_) pet_keyboard_handle_wx_char(event); });
        canvas_->Bind(wxEVT_KEY_UP, [](wxKeyEvent& event) { pet_keyboard_handle_wx(event, false); });
        canvas_->Bind(wxEVT_KILL_FOCUS, [](wxFocusEvent& event) { pet_keyboard_wx_release_all(); event.Skip(); });
        canvas_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            canvas_->SetFocus(); pointerHeld_ = true; UpdateCursor(true);
        });
        for (auto type : {wxEVT_LEFT_UP, wxEVT_MIDDLE_UP})
            canvas_->Bind(type, [this](wxMouseEvent& event) { pointerHeld_ = event.ButtonIsDown(wxMOUSE_BTN_ANY); UpdateCursor(true); event.Skip(); });
        for (auto type : {wxEVT_RIGHT_DOWN, wxEVT_MIDDLE_DOWN})
            canvas_->Bind(type, [this](wxMouseEvent& event) { pointerHeld_ = true; UpdateCursor(true); event.Skip(); });
        canvas_->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            pointerInside_ = true;
            pointerActivity_.update(event.GetX(), event.GetY(), true, FromDIP(6));
            UpdateCursor(true); event.Skip();
        });
        canvas_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            pointerInside_ = false; pointerHeld_ = false; pointerActivity_.update(0, 0, false, 6);
            UpdateCursor(true); event.Skip();
        });
        canvas_->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            pointerInside_ = true;
            pointerHeld_ = event.ButtonIsDown(wxMOUSE_BTN_ANY);
            UpdateCursor(event.Dragging() || pointerActivity_.update(event.GetX(), event.GetY(), true, FromDIP(6)));
            event.Skip();
        });
        canvas_->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& event) {
            pointerHeld_ = event.ButtonIsDown(wxMOUSE_BTN_ANY); UpdateCursor(true);
            if (!ready_ || closing_) return;
            Suspension suspended(*this);
            UpdateMenus();
            // Popup menus own their descendants. Copy the complete menu tree
            // rather than attaching the menu bar's owned submenus twice.
            wxMenu popup;
            for (size_t i = 0; i < GetMenuBar()->GetMenuCount(); ++i)
                popup.AppendSubMenu(CloneMenu(*GetMenuBar()->GetMenu(i)), GetMenuBar()->GetMenuLabel(i));
            canvas_->PopupMenu(&popup, event.GetPosition());
            if (!closing_) canvas_->SetFocus();
        });
        // Loading can show modal boxes, which would block the Explorer window the
        // drop came from: copy the path and load after OnDropFiles has returned.
        canvas_->SetDropTarget(new ProgramDropTarget([this](const wxString& path, size_t count) {
            CallAfter([this, path, count] {
                if (!ready_ || closing_) return;
                Suspension suspended(*this);
                LoadPath(path);
                if (count > 1) SetNote((titleNote_.empty() ? wxString() : titleNote_ + "; ") + wxString::Format("%zu other dropped file(s) ignored", count - 1));
                UpdateMenus(); canvas_->SetFocus();
            });
        }));
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
            if (!event.GetActive()) pointerHeld_ = false;
            RawInput_ReleaseAll(); if (!event.GetActive()) mixer_flush();
            UpdateCursor(true);
            schedule_.Reset(Clock::now()); event.Skip();
        });
        Bind(wxEVT_MENU, &PetFrame::OnCommand, this);
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) { Pause(); UpdateMenus(); event.Skip(); });
        Bind(wxEVT_MENU_CLOSE, [this](wxMenuEvent& event) { Resume(); event.Skip(); });
        Bind(wxEVT_TIMER, &PetFrame::OnTimer, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            if (pendingWindowScale_ && !IsMaximized() && !IsFullScreen()) {
                pendingWindowScale_ = false;
                CallAfter([this] { if (!closing_) ApplyScale(scale_); });
            }
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, &PetFrame::OnClose, this);
        // ROM sets copied in while Pet-GPT runs show up the next time a menu opens.
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) {
            if (ready_ && !closing_) { RefreshRomAvailability(); UpdateMenus(); }
            event.Skip();
        });
        Show();
        if (fullscreen && !smoke_) ToggleFullscreen();
        canvas_->SetFocus();
        CallAfter([this] { Initialize(); });
    }

    void Pause() { if (paused_++ == 0) pause_audio(); RawInput_ReleaseAll(); mixer_flush(); }
    void Resume() {
        if (paused_ > 0 && --paused_ == 0) { mixer_flush(); restore_audio(); }
        schedule_.Reset(Clock::now());
    }
    void SetNote(const wxString& note) { titleNote_ = note; UpdateTitle(); }
    // Every way of loading software (dialog, drop, Recent, command line) comes through here.
    bool LoadPath(const wxString& path) {
        canvas_->SetCurrent(*context_);
        wxFileName name(path); name.MakeAbsolute();
        const std::string full = name.GetFullPath().ToStdString(wxConvUTF8);
        const bool ok = pet_load_software(full.c_str());
        if (ok) { RecentAdd(full); RebuildRecent(); }
        return ok;
    }
    bool InputFocused() const { return ready_ && !closing_ && !paused_ && wxWindow::FindFocus() == canvas_; }
    // For tests: GDK on Wayland reports blank and arrow cursors identically.
    bool CursorHidden() const { return !cursorVisible_; }

protected:
#ifdef __WXMSW__
    // A pad plugged in after launch: this only raises a flag that poll_joystick() acts on.
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override {
        if (message == WM_DEVICECHANGE) joystick_device_change();
        return wxFrame::MSWWindowProc(message, wParam, lParam);
    }
#endif
private:
    using Clock = std::chrono::steady_clock;
    wxMenu* recent_{};
    wxGLCanvas* canvas_{};
    std::unique_ptr<wxGLContext> context_;
    CrtDialog* crt_{};
    wxTimer timer_;
    std::vector<std::string> arguments_;
    wxString screenshot_;
    std::string startupProgram_;
    FrameSchedule schedule_;
    int paused_ = 0, ram_ = 32, scale_ = 2, knob_ = 0, smokeFrames_ = 0;
    bool ready_ = false, closing_ = false, smoke_ = false;
    bool userPaused_ = false;
    wxString titleNote_;
    wxSize windowedTrim_;
    FullscreenCursor cursorPolicy_;
    PointerActivity pointerActivity_;
    bool cursorVisible_ = true, pointerInside_ = false, pendingWindowScale_ = false;
    bool waylandMaximizeRequested_ = false;
    bool pointerHeld_ = false;
    static constexpr int kModelIds[5] = {1, 2, 4, 12, 8};   // Model1..Model8 order
    bool romAvailable_[5] = {};

    // Checks the ROM files of every model (a few dozen file-size queries). Run
    // at start-up and whenever a menu opens, so ROMs added while Pet-GPT runs
    // still enable their model, without checking the disk on every command.
    void RefreshRomAvailability() {
        for (int i = 0; i < 5; ++i) romAvailable_[i] = pet_rom_set_available(kModelIds[i]);
    }

    void UpdateCursor(bool activity = false) {
        if (!canvas_ || closing_) return;
        const bool interacting = paused_ > (userPaused_ ? 1 : 0) || pointerHeld_;
        const bool focused = IsActive() && pointerInside_ && wxWindow::FindFocus() == canvas_;
        const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        const bool visible = cursorPolicy_.update(IsFullScreen(), focused, activity, interacting, now);        if (visible != cursorVisible_) {
            cursorVisible_ = visible;
            canvas_->SetCursor(wxCursor(visible ? wxCURSOR_ARROW : wxCURSOR_BLANK));
        }
    }

    struct Suspension {
        PetFrame& owner;
        explicit Suspension(PetFrame& f) : owner(f) { owner.Pause(); }
        ~Suspension() { owner.Resume(); }
    };

    static wxMenu* CloneMenu(const wxMenu& source) {
        auto* copy = new wxMenu;
        for (auto* item : source.GetMenuItems()) {
            if (item->IsSeparator()) { copy->AppendSeparator(); continue; }
            wxMenuItem* added = item->GetSubMenu()
                ? copy->AppendSubMenu(CloneMenu(*item->GetSubMenu()), item->GetItemLabel(), item->GetHelp())
                : copy->Append(item->GetId(), item->GetItemLabel(), item->GetHelp(), item->GetKind());
            added->Enable(item->IsEnabled());
            if (added->IsCheckable()) added->Check(item->IsChecked());
        }
        return copy;
    }

    void BuildMenus() {
        auto* bar = new wxMenuBar;
        auto* file = new wxMenu;
        // Shortcuts are shown in the labels but are not native menu
        // accelerators: Windows accelerators bypass the canvas's IsAutoRepeat
        // guard, so a held key would repeat the command (fullscreen flipping
        // back and forth, repeated resets). HandleShortcut runs them all.
        file->Append(Load, "&Load Program / Disk / Tape... (Ctrl+O)");
        recent_ = new wxMenu; file->AppendSubMenu(recent_, "Rece&nt"); RebuildRecent();
        file->Append(Eject, "&Eject Disk (Ctrl+E)");
        auto* tape = new wxMenu;
        tape->AppendCheckItem(TapePlay, "&Play"); tape->Append(TapeStop, "&Stop");
        tape->Append(TapeRewind, "&Rewind"); tape->Append(TapeEject, "&Eject");
        file->AppendSubMenu(tape, "&Tape"); file->AppendSeparator();
        file->Append(Screenshot, "Save &Screenshot (F6)"); file->AppendSeparator();
        file->Append(OpenFiles, "Open Files Fol&der"); file->Append(OpenRoms, "Open ROM Folder");
        file->Append(OpenShots, "Open Screenshots Folder"); file->AppendSeparator();
        file->Append(Reset, "&Reset (Ctrl+R)"); file->AppendSeparator(); file->Append(wxID_EXIT, "E&xit");
        bar->Append(file, "&File");
        auto* machine = new wxMenu;
        machine->AppendCheckItem(TogglePause, "&Pause (Ctrl+P / Pause)", "Pause or resume emulation and audio");
        machine->AppendSeparator();
        machine->AppendRadioItem(Model1, wxS("PET 2001 — BASIC 1"));
        machine->AppendRadioItem(Model2, wxS("PET 2001-N — BASIC 2"));
        machine->AppendRadioItem(Model4, wxS("PET 4000 — BASIC 4"));
        machine->AppendRadioItem(Model12, wxS("PET 4032 — 12-inch CRTC"));
        machine->AppendRadioItem(Model8, wxS("CBM 8032 — 80 columns"));
        auto* memory = new wxMenu;
        memory->AppendRadioItem(Ram4, "4 KB"); memory->AppendRadioItem(Ram8, "8 KB");
        memory->AppendRadioItem(Ram16, "16 KB"); memory->AppendRadioItem(Ram32, "32 KB");
        machine->AppendSubMenu(memory, "&Memory"); machine->AppendSeparator();
        machine->AppendCheckItem(Speed, "&2x Speed");
        machine->AppendCheckItem(Graphics, "&Graphics Keyboard (F12)");
        machine->Append(Keyboard, "&Keyboard Mapping...");
        machine->AppendCheckItem(Snes, "&SNES Adapter");
        bar->Append(machine, "&Machine");
        auto* view = new wxMenu;
        view->AppendRadioItem(Scale1, "&1x (320 x 240)");
        view->AppendRadioItem(Scale2, "&2x (640 x 480)");
        view->AppendRadioItem(Scale3, "&3x (960 x 720)");
        view->AppendRadioItem(ScaleFit, "&Fit to Screen"); view->AppendSeparator();
        view->AppendCheckItem(Fullscreen, "&Fullscreen (F11 / Alt+Enter)"); view->AppendSeparator();
        view->AppendCheckItem(Crt, "&CRT (mono monitor) (F10)");
        auto* monitor = new wxMenu;
        monitor->AppendRadioItem(Green, "Green phosphor"); monitor->AppendRadioItem(White, "Black && white");
        view->AppendSubMenu(monitor, "&Monitor"); view->Append(CrtSettings, "CRT Monitor &Settings...");
        bar->Append(view, "&View");
        auto* help = new wxMenu; help->Append(QuickStart, "&Quick Start...");
        help->AppendSeparator(); help->Append(wxID_ABOUT, "&About Pet-GPT..."); bar->Append(help, "&Help");
        SetMenuBar(bar);
    }

    void RebuildRecent() {
        while (recent_->GetMenuItemCount()) recent_->Destroy(recent_->FindItemByPosition(0));
        const auto list = RecentLoad();
        for (size_t i = 0; i < list.size(); ++i) {
            const wxString path = wxString::FromUTF8(list[i]);
            wxString name = wxFileName(path).GetFullName(); name.Replace("&", "&&");
            recent_->Append(RecentFirst + int(i), wxString::Format("&%zu  %s", i + 1, name), path);
        }
        if (list.empty()) recent_->Append(wxID_ANY, "(none)")->Enable(false);
        recent_->AppendSeparator();
        recent_->Append(RecentClear, "&Clear")->Enable(!list.empty());
    }

    void OpenRecent(int index) {
        const auto list = RecentLoad();
        if (index < 0 || size_t(index) >= list.size()) return;
        const wxString path = wxString::FromUTF8(list[size_t(index)]);
        if (!wxFileExists(path)) {
            RecentRemove(list[size_t(index)]); RebuildRecent();
            host_message("File not found", ("This file no longer exists and was removed from the Recent list:\n\n" + path.ToStdString(wxConvUTF8)).c_str(), false);
            return;
        }
        LoadPath(path);
    }

    void Initialize() {
        if (!context_->IsOK() || !canvas_->SetCurrent(*context_)) {
            Fail("Unable to create an OpenGL 3.3 context. Check the display driver."); return;
        }
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK || !GLEW_VERSION_3_3) {
            Fail("OpenGL 3.3 is required by the PET CRT renderer."); return;
        }
        // GLEW probes a deprecated enum on some core-profile drivers.
        while (glGetError() != GL_NO_ERROR) {}
        std::vector<char*> argv;
        for (auto& arg : arguments_) argv.push_back(arg.data());
        joystick_set_host_window(canvas_->GetHandle());
        PrepareViewport();
        if (!emu_init(static_cast<int>(argv.size()), argv.data())) {
            applicationExitCode = 1; Close(); return;
        }
        ready_ = true;
        RefreshRomAvailability();
        pet_keyboard_load_settings();
        pet_set_speed(get_config_bool("machine", "speed2x", false) ? 2 : 1);
        pet_set_gfx_kbd(get_config_bool("input", "graphics_kbd", true));
        pet_set_crt(get_config_bool("video", "crt", false));
        pet_set_monitor(get_config_bool("video", "crt_tint", true));
        if (pet_get_basic() == 1) ram_ = 8;
        if (pet_get_basic() == 8) ram_ = 32;
        if (!startupProgram_.empty()) LoadPath(wxString::FromUTF8(startupProgram_));
        UpdateMenus(); schedule_.Reset(Clock::now()); timer_.Start(4);
    }

    void Fail(const char* message) {
        applicationExitCode = 1; host_message("Pet-GPT startup", message, true); Close();
    }

    void PrepareViewport() {
        canvas_->SetCurrent(*context_);
        const wxSize logical = canvas_->GetClientSize();
        const double dpi = canvas_->GetContentScaleFactor();
        const int width = std::max(1, static_cast<int>(logical.x * dpi));
        const int height = std::max(1, static_cast<int>(logical.y * dpi));
        glViewport(0, 0, width, height); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        const auto view = host_fit_viewport(width, height, 4, 3);
        glViewport(view.x, view.y, view.w, view.h);
    }

    void OnTimer(wxTimerEvent&) {
        UpdateCursor(); // Wall-clock UI behavior must continue while emulation is paused.
        if (!ready_ || closing_ || paused_ || IsIconized()) { schedule_.Reset(Clock::now()); return; }
        bool dropped = false;
        const int count = schedule_.Due(Clock::now(), &dropped);
        if (!count) return;
        if (dropped) mixer_flush();
        PrepareViewport();
        int ran = 0;
        while (ran < count) {
            emu_run_frame();
            ++ran;
        }
        canvas_->SwapBuffers();
        if (smoke_ && (smokeFrames_ += ran) >= 120) {
            VerifySmokeFrame();
            Close();
        }
    }

    // clearStaleErrors: discard GL errors left by earlier frames so the caller's
    // glGetError() reports only this capture. The smoke check keeps them, since
    // catching rendering errors is its purpose.
    wxImage CaptureScreen(bool clearStaleErrors = false) {
        if (clearStaleErrors) { canvas_->SetCurrent(*context_); while (glGetError() != GL_NO_ERROR) {} }
        PrepareViewport(); emu_present(); glFinish();
        const auto size = canvas_->GetClientSize();
        const double dpi = canvas_->GetContentScaleFactor();
        const int w = std::max(1, static_cast<int>(size.x * dpi)), h = std::max(1, static_cast<int>(size.y * dpi));
        wxImage image(w, h);
        if (!image.IsOk()) return {};
        GLint alignment = 4, readBuffer = GL_BACK;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment); glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK); glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, image.GetData());
        glPixelStorei(GL_PACK_ALIGNMENT, alignment); glReadBuffer(readBuffer);
        return image.Mirror(false);
    }

    void SaveScreenshot() {
        const wxImage image = CaptureScreen(/*clearStaleErrors=*/true);
        if (!image.IsOk() || glGetError() != GL_NO_ERROR) {
            host_message("Screenshot failed", "Unable to capture the PET display.", true); return;
        }
        const wxString directory = wxString::FromUTF8(data_path("screenshots"));
        if (!wxDirExists(directory) && !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            host_message("Screenshot failed", "Unable to create the screenshots folder.", true); return;
        }
        const wxString stem = "pet-gpt-" + wxDateTime::Now().Format("%Y%m%d-%H%M%S");
        wxString path;
        wxFile file;
        for (unsigned suffix = 0; ; ++suffix) {
            const wxString name = stem + (suffix ? wxString::Format("-%u", suffix) : wxString()) + ".png";
            path = wxFileName(directory, name).GetFullPath();
            if (wxFileExists(path)) continue;
            // Exclusive creation also protects captures from another instance.
            if (file.Create(path, false)) break;
            if (wxFileExists(path)) continue;
            host_message("Screenshot failed", "Unable to create the screenshot file.", true); return;
        }
        bool saved = false;
        {
            wxFileOutputStream output(file);
            saved = image.SaveFile(output, wxBITMAP_TYPE_PNG) && output.IsOk();
        }
        saved = file.Close() && saved;
        if (!saved) {
            wxRemoveFile(path);
            host_message("Screenshot failed", "Unable to write the PNG screenshot.", true); return;
        }
        titleNote_ = "Saved " + path;
        UpdateTitle();
        LOG_INFO("Screenshot saved: %s", path.ToStdString(wxConvUTF8).c_str());
    }

    void VerifySmokeFrame() {
        const wxImage image = CaptureScreen();
        if (!image.IsOk()) { applicationExitCode = 1; return; }
        size_t lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(image.GetWidth()) * image.GetHeight() * 3; ++i) if (image.GetData()[i] > 32) ++lit;
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR || lit < 100) {
            wxLogError("SMOKE: rendering failed (GL error %u, lit channels %zu)", static_cast<unsigned>(error), lit);
            applicationExitCode = 1;
        } else {
            wxLogMessage("SMOKE: %d frames, model %d, RAM %d KB, %zu lit channels, GL OK", smokeFrames_, pet_get_basic(), pet_get_ram(), lit);
        }
        if (!screenshot_.empty() && !image.SaveFile(screenshot_, wxBITMAP_TYPE_PNG)) applicationExitCode = 1;
    }

    void ApplyScale(int scale) {
        scale_ = scale;
        if (IsFullScreen()) return; // defer chosen preset until returning to a window
        if (native_wayland() && scale == 0) {
            // Wayland owns top-level placement and usable logical geometry.
            // Request its maximized configure instead of guessing a work area
            // from output pixels or attempting an unsupported global Move().
            pendingWindowScale_ = false;
            waylandMaximizeRequested_ = true;
            Maximize(true);
            return;
        }
        if (IsMaximized() || (native_wayland() && waylandMaximizeRequested_)) {
            waylandMaximizeRequested_ = false;
            Maximize(false);
            if (native_wayland() && IsMaximized()) {
                pendingWindowScale_ = true; // apply preset after the configure event
                return;
            }
        }
        pendingWindowScale_ = false;
        int display = wxDisplay::GetFromWindow(this);
        wxRect work = display == wxNOT_FOUND ? wxGetClientDisplayRect() : wxDisplay(static_cast<unsigned>(display)).GetClientArea();
        const wxSize trim = GetSize() - GetClientSize();
        const int maxW = std::max(1, work.width - trim.x), maxH = std::max(1, work.height - trim.y);
        const wxSize base = FromDIP(wxSize(320, 240));
        const int effective = host_effective_scale(scale, maxW, maxH, base.x, base.y);
        const auto fit = host_fit_viewport(maxW, maxH, 4, 3);
        wxSize wanted = effective ? wxSize(base.x * effective, base.y * effective) : wxSize(fit.w, fit.h);
        SetClientSize(wanted);
        if (!effective) Move(work.GetTopLeft() + wxPoint((work.width - GetSize().x) / 2, (work.height - GetSize().y) / 2));
        else if (!work.Contains(GetRect())) Move(wxPoint(
            std::clamp(GetPosition().x, work.x, std::max(work.x, work.GetRight() + 1 - GetSize().x)),
            std::clamp(GetPosition().y, work.y, std::max(work.y, work.GetBottom() + 1 - GetSize().y))));
    }

    void ToggleFullscreen() {
        if (!IsFullScreen()) { windowedTrim_ = GetSize() - GetClientSize(); ShowFullScreen(true); }
        else {
            ShowFullScreen(false);
            // Elsewhere ShowFullScreen(false) restores the fitted window; on
            // Wayland Fit must be requested again as compositor maximization.
            if (scale_ >= 1 || native_wayland()) ApplyScale(scale_);
        }
        canvas_->SetFocus();
        UpdateCursor(true);
    }

    void LeaveFullscreen() {
        // Consume the polled key as well as the wx event, so leaving fullscreen
        // cannot also type ESC into the PET on the following frame.
        RawInput_ReleaseKey(VK_ESCAPE);
        ToggleFullscreen(); schedule_.Reset(Clock::now()); UpdateMenus();
    }

    void UpdateMenus() {
        if (!ready_) return;
        auto* bar = GetMenuBar();
        bar->Check(TogglePause, userPaused_);
        ram_ = pet_get_ram();
        // Same GTK radio rule as the memory group: enable all, select, then grey out
        // the models whose ROM sets are missing (the running model stays enabled).
        // Availability comes from the cache RefreshRomAvailability() fills, so this
        // runs no file checks after every command.
        for (int i = 0; i < 5; ++i) bar->Enable(Model1 + i, true);
        for (int i = 0; i < 5; ++i) bar->Check(Model1 + i, pet_get_basic() == kModelIds[i]);
        for (int i = 0; i < 5; ++i) bar->Enable(Model1 + i, pet_get_basic() == kModelIds[i] || romAvailable_[i]);
        const int sizes[] = {4, 8, 16, 32};
        // GTK ignores radio-state callbacks for disabled items. Enable the
        // entire group before changing selection so its cached state stays
        // consistent when leaving a fixed-memory model.
        for (int i = 0; i < 4; ++i) bar->Enable(Ram4 + i, true);
        for (int i = 0; i < 4; ++i) if (ram_ == sizes[i]) bar->Check(Ram4 + i, true);
        for (int i = 0; i < 4; ++i) {
            bar->Enable(Ram4 + i, (pet_get_basic() != 1 || sizes[i] == 8) && (pet_get_basic() != 8 || sizes[i] == 32));
        }
        bar->Check(Speed, pet_get_speed() == 2); bar->Check(Graphics, pet_get_gfx_kbd() != 0);
        bar->Check(Snes, pet_get_snes() != 0); bar->Check(Crt, pet_get_crt() != 0);
        bar->Check(Green, pet_get_monitor() != 0); bar->Check(White, pet_get_monitor() == 0);
        bar->Check(Fullscreen, IsFullScreen());
        int display = wxDisplay::GetFromWindow(this);
        wxRect work = display == wxNOT_FOUND ? wxGetClientDisplayRect() : wxDisplay(static_cast<unsigned>(display)).GetClientArea();
        const wxSize trim = IsFullScreen() ? windowedTrim_ : GetSize() - GetClientSize();
        const wxSize base = FromDIP(wxSize(320, 240));
        const int effective = host_effective_scale(scale_, work.width - trim.x, work.height - trim.y, base.x, base.y);
        for (int i = 0; i < 3; ++i) bar->Check(Scale1 + i, effective == i + 1);
        bar->Check(ScaleFit, effective == 0);
        for (int i = 1; i <= 3; ++i) {
            auto size = FromDIP(wxSize(320 * i, 240 * i)) + trim;
            bar->Enable(Scale1 + i - 1, size.x <= work.width && size.y <= work.height);
        }
        bar->Enable(Eject, pet_get_disk_mounted() != 0);
        const int tape = pet_tape_state();
        bar->Enable(TapePlay, (tape & 1) && !(tape & 6)); bar->Check(TapePlay, (tape & 2) != 0);
        bar->Enable(TapeStop, (tape & 2) != 0); bar->Enable(TapeRewind, (tape & 1) != 0);
        bar->Enable(TapeEject, (tape & 1) != 0);
        if (crt_) crt_->Sync();
    }

    bool HandleShortcut(wxKeyEvent& event) {
        const int keycode = event.GetKeyCode();
        int command = 0;
        if (keycode == WXK_F10) command = Crt;
        else if (keycode == WXK_F11 || (keycode == WXK_RETURN && event.AltDown())) command = Fullscreen;
        else if (keycode == WXK_F12) command = Graphics;
        else if (keycode == WXK_PAUSE || (event.ControlDown() && keycode == 'P')) command = TogglePause;
        else if (keycode == WXK_F6) command = Screenshot;
        else if (event.ControlDown() && keycode == 'O') command = Load;
        else if (event.ControlDown() && keycode == 'E') command = Eject;
        else if (event.ControlDown() && keycode == 'R') command = Reset;
        if (command) {
            if (!event.IsAutoRepeat()) { wxCommandEvent action(wxEVT_MENU, command); OnCommand(action); }
            return true;
        }
        // Escape only leaves fullscreen; windowed, it belongs to the PET keyboard.
        if (keycode == WXK_ESCAPE && IsFullScreen()) { if (!event.IsAutoRepeat()) LeaveFullscreen(); return true; }
        if (keycode == WXK_F8) {
            wxString text("[video]\n");
            for (int i = 0; i < 7; ++i) text += wxString::Format("%s=%.3f\n", knobNames[i], pet_shader_get(i));
            LOG_INFO("%s", text.ToStdString(wxConvUTF8).c_str());
            wxLogMessage("%s", text); return true;
        }
        if (keycode == WXK_F9) { knob_ = (knob_ + 1) % 7; UpdateTuningTitle(); return true; }
        if (pet_get_crt() && (keycode == WXK_PAGEUP || keycode == WXK_PAGEDOWN)) {
            float lo, hi, step; pet_shader_range(knob_, &lo, &hi, &step);
            pet_shader_set(knob_, pet_shader_get(knob_) + step * (event.ShiftDown() ? 5 : 1) * (keycode == WXK_PAGEUP ? 1 : -1));
            UpdateTuningTitle(); if (crt_) crt_->Sync(); return true;
        }
        return false;
    }
    void UpdateTuningTitle() {
        titleNote_ = wxString::Format("%s = %.3f", knobNames[knob_], pet_shader_get(knob_));
        UpdateTitle();
    }
    void UpdateTitle() {
        wxString title = wxS("Commodore PET — Pet-GPT 2.7");
        if (userPaused_) title += " [Paused]";
        if (!titleNote_.empty()) title += wxS(" — ") + titleNote_;
        SetTitle(title);
    }

    void OnCommand(wxCommandEvent& event) {
        if (!ready_ || closing_) return;
        Suspension suspended(*this);
        canvas_->SetCurrent(*context_);
        const int id = event.GetId();
        if (id >= Model1 && id <= Model8) {
            const int models[] = {1, 2, 4, 12, 8}; const int want = models[id - Model1];
            if (want != pet_get_basic()) {
                if (pet_set_basic(want)) {
                    ram_ = pet_get_ram();
                    set_config_int("machine", "basic", want); set_config_int("machine", "ram", ram_);
                    SetNote({});   // the machine restarted: the last load/status note no longer applies
                } else {
                    RefreshRomAvailability();
                    host_message("Missing model ROMs", "Unable to load this model's ROMs. Check the roms directory. The previous model remains active.", true);
                }
            }
        } else if (id >= Ram4 && id <= Ram32) {
            const int sizes[] = {4, 8, 16, 32}; ram_ = sizes[id - Ram4];
            pet_set_ram(ram_); set_config_int("machine", "ram", ram_);
            SetNote({});
        } else if (id >= RecentFirst && id < RecentFirst + kRecentMax) OpenRecent(id - RecentFirst);
        else if (id >= TapePlay && id <= TapeEject) {
            pet_tape_command(id - TapePlay);
            if (id == TapeEject) SetNote({});   // "Tape X attached" no longer true
        }
        else if (id >= Scale1 && id <= ScaleFit) ApplyScale(id == ScaleFit ? 0 : id - Scale1 + 1);
        else switch (id) {
        case TogglePause:
            userPaused_ = !userPaused_;
            if (userPaused_) Pause(); else Resume();
            titleNote_.clear(); UpdateTitle(); break;
        case Screenshot: SaveScreenshot(); break;
        case Load: {
            const auto directory = get_config_string(std::string("paths"), std::string("lastromdir"), std::string());
            // First load: start in the programs folder (~/Pet-GPT/files on Linux).
            wxFileDialog dialog(this, "Load PET software", wxString::FromUTF8(directory.empty() ? data_path("files") : directory), "",
                "PET software (*.prg;*.p00;*.t64;*.tap;*.d64;*.d71)|*.prg;*.PRG;*.p00;*.P00;*.t64;*.T64;*.tap;*.TAP;*.d64;*.D64;*.d71;*.D71|All files|*",
                wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK) {
                set_config_string("paths", "lastromdir", dialog.GetDirectory().ToStdString(wxConvUTF8).c_str());
                LoadPath(dialog.GetPath());
            }
            break;
        }
        case RecentClear: RecentSave({}); RebuildRecent(); break;
        case OpenFiles: OpenFolder(wxString::FromUTF8(data_path("files"))); break;
        case OpenRoms: OpenFolder(wxString::FromUTF8(data_path("roms"))); break;
        case OpenShots: OpenFolder(wxString::FromUTF8(data_path("screenshots"))); break;
        case QuickStart: { QuickStartDialog dialog(this); dialog.ShowModal(); break; }
        case Eject:
            if (pet_get_disk_mounted()) { pet_eject_disk(); SetNote("Disk ejected: drive 8 is the files folder again"); }
            break;
        case Reset: pet_reset(); SetNote({}); break;
        case wxID_EXIT: Close(); break;
        case Fullscreen: ToggleFullscreen(); break;
        case Speed: pet_set_speed(pet_get_speed() == 2 ? 1 : 2); set_config_bool("machine", "speed2x", pet_get_speed() == 2); break;
        case Graphics: pet_set_gfx_kbd(!pet_get_gfx_kbd()); set_config_bool("input", "graphics_kbd", pet_get_gfx_kbd() != 0); break;
        case Snes: pet_set_snes(!pet_get_snes()); set_config_bool("input", "snes_adapter", pet_get_snes() != 0); break;
        case Keyboard: pet_keyboard_show_dialog(this); break;
        case Crt: pet_set_crt(!pet_get_crt()); set_config_bool("video", "crt", pet_get_crt() != 0); break;
        case Green: case White: pet_set_monitor(id == Green); set_config_bool("video", "crt_tint", id == Green); break;
        case CrtSettings:
            if (!crt_) crt_ = new CrtDialog(this);
            crt_->Sync(); crt_->Show(); crt_->Raise(); break;
        case wxID_ABOUT: {
            wxAboutDialogInfo info;
            info.SetName(wxS("Pet-GPT — Commodore PET Emulator")); info.SetVersion("2.7");
            info.SetDescription("Five PET/CBM models, tape and disk images, CB2 audio, SNES input, and OpenGL CRT display.\n\n"
                "F11 / Alt+Enter: fullscreen\nF10: CRT shader\nF12: graphics keyboard\n"
                "Pause/Break / Ctrl+P: pause or resume\nF6: save PNG screenshot\n"
                "F9: select CRT knob; Page Up/Down: adjust (Shift: coarse)\nF8: log shader settings");
            info.SetCopyright("Pet-GPT contributors"); info.SetWebSite("https://github.com/tcottrill/Pet-GPT");
            info.SetLicense("GNU General Public License, version 3.\nSee LICENSE in the distribution.\n\n"
                "Built with wxWidgets (wxWindows Library Licence) and GLEW (BSD/MIT licences).");
            wxAboutBox(info, this); break;
        }
        default: event.Skip(); break;
        }
        if (!closing_) { UpdateMenus(); if (id != CrtSettings) canvas_->SetFocus(); }
    }

    void OnClose(wxCloseEvent&) {
        if (closing_) return;
        closing_ = true; timer_.Stop(); RawInput_ReleaseAll(); mixer_flush();
        if (ready_) {
            if (!smoke_) {
                set_config_int("video", "scale", scale_); set_config_bool("video", "fullscreen", IsFullScreen());
                set_config_int("machine", "basic", pet_get_basic()); set_config_int("machine", "ram", ram_);
                set_config_bool("video", "crt", pet_get_crt() != 0); set_config_bool("video", "crt_tint", pet_get_monitor() != 0);
            }
            canvas_->SetCurrent(*context_); emu_end(); ready_ = false;
        }
        frame = nullptr;
        // GL resources were released above while the canvas/context still exist.
        context_.reset(); Destroy();
    }
};

class PetApp final : public wxApp {
public:
    PetApp() {
#if defined(__WXGTK__) && defined(__linux__)
        // App construction precedes GTK initialization; preserve an explicit
        // backend choice while preferring native Wayland in Wayland sessions.
        if (!std::getenv("GDK_BACKEND") && std::getenv("WAYLAND_DISPLAY"))
            ::setenv("GDK_BACKEND", "wayland,x11", 0);
        // Inside a Flatpak, name the program after the app ID so the Wayland
        // app_id / X11 WM_CLASS match the installed .desktop file and the
        // taskbar shows the Pet-GPT icon instead of a generic one.
        if (const char* flatpakId = std::getenv("FLATPAK_ID"); flatpakId && *flatpakId)
            g_set_prgname(flatpakId);
#endif
    }
    bool OnInit() override {
        SetAppName("Pet-GPT"); wxInitAllImageHandlers();
        // Avoid unsolicited modal log windows during initialization or smoke runs.
        wxLog::SetActiveTarget(new wxLogStderr());
        initialize_paths(wxStandardPaths::Get().GetExecutablePath().ToStdString(wxConvUTF8));
        LogOpen(data_path("petemu-log.txt"));
#ifdef __WXGTK__
        bool wayland = false;
#ifdef GDK_WINDOWING_WAYLAND
        wayland = GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
#endif
        LOG_INFO("Display backend: %s", wayland ? "native Wayland" : "X11 (possibly XWayland)");
        wxLogMessage("Display backend: %s", wayland ? "native Wayland" : "X11 (possibly XWayland)");
#endif
        std::vector<std::string> args; wxString screenshot;
        for (int i = 0; i < argc; ++i) {
            const wxString arg(argv[i]);
            if (arg == "--smoke-test") smokeMode = true;
            else if (arg.StartsWith("--smoke-test=")) { smokeMode = true; screenshot = arg.Mid(13); }
            else args.push_back(arg.ToStdString(wxConvUTF8));
        }
        if (parse_host_options(args).help) {
            wxMessageBox("pet-gpt [options] [program]\n\n"
                "-rom <file>  Load a program or media image\n"
                "-fullscreen / -window  Override saved window mode\n"
                "-scale <1|2|3|fit>  Override saved scale\n"
                "-monitor <n>  Select starting monitor (Windows: 1 = primary, others by layout)\n"
                "-monitorid <id>  Windows: physical monitor ID from log; Linux: display name\n"
                "-pet2001 / -basic2 / -basic4 / -4032 / -8032  Select model\n"
                "-disk <image>  Mount a disk image\n"
                "-h  Show this help", "Pet-GPT", wxOK | wxICON_INFORMATION);
            LogClose(); return false;
        }
        new PetFrame(std::move(args), screenshot, smokeMode);
        return true;
    }
    int OnRun() override { const int result = wxApp::OnRun(); return applicationExitCode ? applicationExitCode : result; }
    int OnExit() override { LogClose(); return wxApp::OnExit(); }
};
} // namespace

void host_message(const char* title, const char* message, bool error) {
    lastMessageTitle = wxString::FromUTF8(title); lastMessageText = wxString::FromUTF8(message);
    if (smokeMode) {
        wxLogError("%s: %s", wxString::FromUTF8(title), wxString::FromUTF8(message));
        if (error) applicationExitCode = 1;
        return;
    }
    if (frame) frame->Pause();
    wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8(title), wxOK | (error ? wxICON_ERROR : wxICON_INFORMATION), frame);
    if (frame) frame->Resume();
}

int host_select_program(const std::vector<std::string>& labels) {
    if (smokeMode) return -1;
    if (frame) frame->Pause();
    wxArrayString choices; for (const auto& text : labels) choices.Add(wxString::FromUTF8(text));
    wxSingleChoiceDialog dialog(frame, "Select a program to load:", "Load program from T64", choices);
    const int result = dialog.ShowModal() == wxID_OK ? dialog.GetSelection() : -1;
    if (frame) frame->Resume();
    return result;
}
void host_status(const char* text) {
    LOG_INFO("Status: %s", text);
    if (frame) frame->SetNote(wxString::FromUTF8(text));
}

bool host_rom_setup() {
    if (smokeMode) {
        host_message("Missing ROMs", "No PET ROM set could be loaded. Check the roms directory.", true);
        return false;
    }
    if (frame) frame->Pause();
    RomSetupDialog dialog(frame, wxString::FromUTF8(data_path("roms")));
    const bool retry = dialog.ShowModal() == wxID_OK;
    if (frame) frame->Resume();
    return retry;
}

bool host_is_focused() { return frame && frame->InputFocused(); }

wxIMPLEMENT_APP(PetApp);
