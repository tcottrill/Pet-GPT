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
    TapePlay, TapeStop, TapeRewind, TapeEject, TogglePause, Screenshot
};
class PetFrame;
PetFrame* frame = nullptr;
bool smokeMode = false;
int applicationExitCode = 0;
const char* knobNames[] = {"mono_blur_h", "mono_blur_v", "mono_halation", "mono_halation_radius",
                          "mono_scanline", "mono_contrast", "mono_brightness"};
class ProgramDropTarget final : public wxFileDropTarget {
public:
    explicit ProgramDropTarget(std::function<void(const wxString&)> load) : load_(std::move(load)) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& paths) override {
        if (paths.empty()) return false;
        load_(paths[0]); return true;
    }
private:
    std::function<void(const wxString&)> load_;
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
        canvas_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { canvas_->SetFocus(); });
        canvas_->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent& event) {
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
        canvas_->SetDropTarget(new ProgramDropTarget([this](const wxString& path) {
            if (!ready_ || closing_) return;
            Suspension suspended(*this);
            canvas_->SetCurrent(*context_); pet_load_software(path.ToStdString(wxConvUTF8).c_str());
            UpdateMenus(); canvas_->SetFocus();
        }));
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
            RawInput_ReleaseAll(); if (!event.GetActive()) mixer_flush();
            schedule_.Reset(Clock::now()); event.Skip();
        });
        Bind(wxEVT_MENU, &PetFrame::OnCommand, this);
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& event) { Pause(); UpdateMenus(); event.Skip(); });
        Bind(wxEVT_MENU_CLOSE, [this](wxMenuEvent& event) { Resume(); event.Skip(); });
        Bind(wxEVT_TIMER, &PetFrame::OnTimer, this);
        Bind(wxEVT_CLOSE_WINDOW, &PetFrame::OnClose, this);
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
    bool InputFocused() const { return ready_ && !closing_ && !paused_ && wxWindow::FindFocus() == canvas_; }

private:
    using Clock = std::chrono::steady_clock;
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
        file->Append(Eject, "&Eject Disk (Ctrl+E)");
        auto* tape = new wxMenu;
        tape->AppendCheckItem(TapePlay, "&Play"); tape->Append(TapeStop, "&Stop");
        tape->Append(TapeRewind, "&Rewind"); tape->Append(TapeEject, "&Eject");
        file->AppendSubMenu(tape, "&Tape"); file->AppendSeparator();
        file->Append(Screenshot, "Save &Screenshot (F6)"); file->AppendSeparator();
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
        auto* help = new wxMenu; help->Append(wxID_ABOUT, "&About Pet-GPT..."); bar->Append(help, "&Help");
        SetMenuBar(bar);
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
        pet_keyboard_load_settings();
        pet_set_speed(get_config_bool("machine", "speed2x", false) ? 2 : 1);
        pet_set_gfx_kbd(get_config_bool("input", "graphics_kbd", true));
        pet_set_crt(get_config_bool("video", "crt", false));
        pet_set_monitor(get_config_bool("video", "crt_tint", true));
        if (pet_get_basic() == 1) ram_ = 8;
        if (pet_get_basic() == 8) ram_ = 32;
        if (!startupProgram_.empty()) pet_load_software(startupProgram_.c_str());
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
        if (!ready_ || closing_ || paused_ || IsIconized()) { schedule_.Reset(Clock::now()); return; }
        bool dropped = false;
        const int count = schedule_.Due(Clock::now(), &dropped);
        if (!count) return;
        if (dropped) mixer_flush();
        PrepareViewport();
        int ran = 0;
        while (ran < count) {
            if (!emu_run_frame()) { HandleEscape(); return; }
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
        if (IsMaximized()) Maximize(false);
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
        else { ShowFullScreen(false); if (scale_ >= 1) ApplyScale(scale_); }
        canvas_->SetFocus();
    }

    void HandleEscape() {
        // Consume the polled key as well as the wx event, so leaving fullscreen
        // cannot turn into a second Escape/quit on the following frame.
        RawInput_ReleaseKey(VK_ESCAPE);
        if (IsFullScreen() && arguments_.size() <= 1) {
            ToggleFullscreen(); schedule_.Reset(Clock::now()); UpdateMenus();
        } else Close();
    }

    void UpdateMenus() {
        if (!ready_) return;
        auto* bar = GetMenuBar();
        bar->Check(TogglePause, userPaused_);
        ram_ = pet_get_ram();
        const int modelIds[] = {1, 2, 4, 12, 8};
        for (int i = 0; i < 5; ++i) bar->Check(Model1 + i, pet_get_basic() == modelIds[i]);
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
        if (keycode == WXK_ESCAPE) { if (!event.IsAutoRepeat()) HandleEscape(); return true; }
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
                } else host_message("Missing model ROMs", "Unable to load this model's ROMs. Check the roms directory. The previous model remains active.", true);
            }
        } else if (id >= Ram4 && id <= Ram32) {
            const int sizes[] = {4, 8, 16, 32}; ram_ = sizes[id - Ram4];
            pet_set_ram(ram_); set_config_int("machine", "ram", ram_);
        } else if (id >= TapePlay && id <= TapeEject) pet_tape_command(id - TapePlay);
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
                "PET software (*.prg;*.t64;*.tap;*.d64;*.d71)|*.prg;*.PRG;*.t64;*.T64;*.tap;*.TAP;*.d64;*.D64;*.d71;*.D71|All files|*",
                wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK) {
                set_config_string("paths", "lastromdir", dialog.GetDirectory().ToStdString(wxConvUTF8).c_str());
                pet_load_software(dialog.GetPath().ToStdString(wxConvUTF8).c_str());
            }
            break;
        }
        case Eject: pet_eject_disk(); break;
        case Reset: pet_reset(); break;
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
bool host_is_focused() { return frame && frame->InputFocused(); }

wxIMPLEMENT_APP(PetApp);
