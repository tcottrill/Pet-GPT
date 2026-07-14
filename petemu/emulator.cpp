#include "framework.h"
#include "glew.h"
#include "wglew.h"
#include "sys_log.h"
#include "rawinput.h"
#include "fileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <process.h>
#include <memory.h>
#include "cpu_6502.h"
#include "fast_poly.h"

#include "sys_gl.h"

#include "pet_machine.h"
#include "pet2001io.h"      // for io.setKeyrows(...)
#include "pet_mem.h"
#include "pet_gl.h"
#include "pet_kbd_input.h"
#include "ieee_helpers.h"
#include "pet2001ieee.h"
#include "basic_prg.h"       // basic_relink (rebuild BASIC line links after PRG inject)
#include "mixer.h"
#include "pet_roms.h"        // ROM-set loaders (load_pet2_romset, etc.)

#include <iterator> // required for istreambuf_iterator
#include <filesystem>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include "joystick.h"        // XInput/WinMM gamepad layer
#include "iniFile.h"         // pet.ini config
#include "snes_adapter.h"    // SNES button bit positions

#pragma warning( disable : 4996 4244)

// -----------------------------------------------------------------
// SNES adapter (user-port joystick) host bridge
// -----------------------------------------------------------------
static bool g_snes_enabled = true;

// Map the host gamepad (Joystick.cpp, player 1) to the 12 SNES button bits.
// Xbox layout: joy_b1=A joy_b2=B joy_b3=X joy_b4=Y joy_b5=LB joy_b6=RB
//              joy_b7=Back joy_b8=Start.
static uint16_t map_joy_to_snes()
{
	if (!joystick_any_connected()) return 0;
	uint16_t m = 0;
	if (joy_up)    m |= (1u << SnesAdapter::BTN_UP);
	if (joy_down)  m |= (1u << SnesAdapter::BTN_DOWN);
	if (joy_left)  m |= (1u << SnesAdapter::BTN_LEFT);
	if (joy_right) m |= (1u << SnesAdapter::BTN_RIGHT);
	if (joy_b1)    m |= (1u << SnesAdapter::BTN_B);      // A   -> B (fire)
	if (joy_b2)    m |= (1u << SnesAdapter::BTN_A);      // B   -> A
	if (joy_b3)    m |= (1u << SnesAdapter::BTN_Y);      // X   -> Y
	if (joy_b4)    m |= (1u << SnesAdapter::BTN_X);      // Y   -> X
	if (joy_b5)    m |= (1u << SnesAdapter::BTN_L);      // LB  -> L
	if (joy_b6)    m |= (1u << SnesAdapter::BTN_R);      // RB  -> R
	if (joy_b7)    m |= (1u << SnesAdapter::BTN_SELECT); // Back-> Select
	if (joy_b8)    m |= (1u << SnesAdapter::BTN_START);  // Start->Start
	return m;
}

int16_t* stream_data = nullptr;
const UINT32 sampleRate = 44100;
const UINT32 frameCount = 735;

// Audio State
static bool s_last_cb2_level = false;
static double s_dc_offset = 0.0;
static double s_lpf_output = 0.0; // Low Pass Filter state

// CB2 -> PCM reconstruction (extracted, testable module: sys_audio/cb2_render.h).
#include "cb2_render.h"
static Cb2Render g_cb2render;

// NOTE: the CB2 duty/reconstruction logic now lives in sys_audio/cb2_render.h
// (Cb2Render). It was moved out of this file so it can be unit-tested and so
// there is a single authoritative copy (no more editing a dead duplicate).

// PET machine (owns 6502, I/O (PIA+VIA), video, bus)
static PetMachine* g_pet = nullptr;
static PetGL* g_gl = nullptr;

// Emulation speed: run N x the authentic cycles per host frame. Retrace IRQ,
// keyboard scan, and the CB2 audio timebase all derive from cycles, so the
// whole machine speeds up coherently (sound pitches up, like fast-forward).
static int g_speed_mult = 1;

//Audio mixer

// Somewhere global / per-PET
static int16_t g_cb2_frame_buffer[2048]; // choose a size matching your audio frame
static int     g_cb2_frame_samples = 0;  // number of samples per frame
static int     g_cb2_volume = 128; // mid volume 0..255

void pet_audio_init(int device_rate, int fps)
{
	// Pick how many samples you want per emu frame.
	// For example, device_rate / fps.
	g_cb2_frame_samples = device_rate / fps;
}
// FIX: Add this function to reset audio state
void reset_audio()
{
	s_last_cb2_level = false;
	s_dc_offset = 0.0;
	g_cb2render.reset();   // clear reconstruction filter state
	// Clear buffer
	if (stream_data) {
		memset(stream_data, 0, frameCount * sizeof(int16_t));
	}
}

int screenw;
int screenh;
int windowed;

UINT32 frames = 0;
UINT32 dwElapsedTicks = 0;

UINT32 dwResult = 0;
UINT32 dwDisplayInterval = 0;
UINT32 dwIntTotal = 0;

// Simple boolean flag scan (e.g., -basic4)
static bool arg_has(int argc, char** argv, const char* key) {
	for (int i = 1; i < argc; ++i) {
		if (_stricmp(argv[i], key) == 0) return true;
	}
	return false;
}

// Simple arg scan
static std::string arg_get(int argc, char** argv, const char* key) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (_stricmp(argv[i], key) == 0) return std::string(argv[i + 1]);
	}
	return {};
}

static bool ends_with_icase(const std::string& s, const char* suffix) {
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; ++i) {
		char a = s[s.size() - n + i], b = suffix[i];
		if (a >= 'a' && a <= 'z') a -= 32;
		if (b >= 'a' && b <= 'z') b -= 32;
		if (a != b) return false;
	}
	return true;
}

static int g_basic_set = 2;  // 2, 4, or 8 (= 8032 80-column)
static int g_ram_kb    = 32; // configured RAM size in KB (4/8/16/32)

static bool load_basic_set(int which) {
	if (!g_pet) {
		LOG_ERROR("[PET] load_basic_set: g_pet is null");
		return false;
	}
	// Each ROM set lives in its own self-contained subfolder (own copy of the
	// shared character ROMs too), so a set can be swapped or archived as a unit.
	const std::string romdir = (which == 8) ? "./roms/8032/"
	                          : (which == 4) ? "./roms/basic4/"
	                                         : "./roms/basic2/";
	// BASIC 4 maps $B000-$BFFF; BASIC 2 does not. Unmap it when switching
	// down, or stale BASIC-4 bytes stay readable (and write-protected) there
	// and ROM-detection code misidentifies the machine.
	if (which == 2) g_pet->bus().clearROM(0xB000, 0x1000);
	bool ok = (which == 8) ? load_pet8032_romset(*g_pet, romdir)
	        : (which == 4) ? load_pet4_romset(*g_pet, romdir)
	                       : load_pet2_romset(*g_pet, romdir, true); // BASIC 2, "N" editor; zimmers.net-native filenames
	if (!ok) { LOG_ERROR("[PET] Failed to load BASIC %d ROM set from %s", which, romdir.c_str()); return false; }
	// Machine geometry follows the ROM set: 8032 = 80 cols + 2 KB screen +
	// business keyboard matrix; 40-col models the reverse.
	const bool is8032 = (which == 8);
	g_pet->bus().setScreenWindow(is8032);
	g_pet->video().setColumns(is8032 ? 80 : 40);
	set_pet_business_kbd(is8032);
	g_basic_set = which;
	return true;
}

void reset_all()
{
	if (g_pet) g_pet->reset();
	reset_audio();
}


///////////////////////  MAIN LOOP /////////////////////////////////////
bool emu_run_frame()
{
	g_pet->io().cb2ResetEdgeLog();

	// Only feed host input to the PET while the emulator window is in the
	// foreground. RawInput uses RIDEV_INPUTSINK (so releases are tracked even
	// unfocused), but without this gate everything typed into OTHER apps was
	// also typed into BASIC (and CapsLock fired RUN/STOP).
	HWND win_get_window(); // host_window.cpp
	const bool focused = (GetForegroundWindow() == win_get_window());
	if (focused) {
		update_keyboard(g_pet);
	}
	else {
		uint8_t idle[10];
		memset(idle, 0xFF, sizeof(idle));   // active-low: all released
		g_pet->io().setKeyrows(idle);
	}

	// Poll the host gamepad and feed the emulated SNES adapter.
	if (g_snes_enabled) {
		poll_joystick();
		g_pet->io().setSnesButtons(focused ? map_joy_to_snes() : 0);
	}

	// 2) Run ~1/60 sec of CPU afterward (x2 when Machine > 2x Speed is on)
	const int cycles_per_frame = (1000000 / 60) * g_speed_mult;
	if (g_pet) g_pet->runCycles(cycles_per_frame);

	// Service the video blank timer in emulated time (PIA1 CA2 blanking:
	// the screen goes dark 100 emulated ms after software requests it).
	if (g_pet) g_pet->video().update(cycles_per_frame / 1000);

	// 3) Present video
	if (g_gl && g_pet) {
		const uint32_t* fb = g_pet->video().framebuffer();
		const int fbW = g_pet->video().fbWidth();
		const int fbH = g_pet->video().fbHeight();
		g_gl->present(fb, fbW, fbH);
	}
	// -----------------------------------------------------------------
	// AUDIO GENERATION
	// -----------------------------------------------------------------
	const uint32_t edgeCount = g_pet->io().cb2GetEdgeCount();
	const CB2Edge* edges = g_pet->io().cb2GetEdges();
	const bool frameStartLevel = g_pet->io().cb2GetFrameStartLevel();

	// Map the frame's transitions over the ACTUAL number of cycles that ran this
	// frame (not a fixed nominal span), so no tail transition is dropped and the
	// audio time-base matches game time exactly.
	const double frameCycles = (double)g_pet->io().cb2GetTickCounter();

	if (stream_data) {
		g_cb2render.render(edges, edgeCount, frameStartLevel, stream_data, (int)frameCount, frameCycles);

		// Send to sound driver
		stream_update(1, stream_data); // 1 channel
	}

	mixer_update();

	extern unsigned char key[256];
	if (key[VK_ESCAPE]) return false;   // host decides: fullscreen->windowed, else quit
	return true;
}

void emu_init(int argc, char** argv)
{
	// Init mixer.
	mixer_init(44100, 60);
	
	// --- Create PET machine (CPU + memory bus + I/O + video) ---
	g_pet = new PetMachine();

	// RAM size from pet.ini [machine] ram (KB); default 32K. (4/8/16/32 only.)
	g_ram_kb = get_config_int("machine", "ram", 32);
	if (g_ram_kb != 4 && g_ram_kb != 8 && g_ram_kb != 16 && g_ram_kb != 32) g_ram_kb = 32;
	g_pet->bus().setRamSize((size_t)g_ram_kb * 1024);

	// --- Load ROM set from disk ---
	int want = arg_has(argc, argv, "-basic4") ? 4
	         : arg_has(argc, argv, "-basic2") ? 2
	         : get_config_int("machine", "basic", 2);
	if (!load_basic_set(want)) std::exit(1);

	// --- Sanity: verify ROM mapping and vectors ---
	auto& mem = g_pet->bus();

	// Check a few bytes in KERNAL at $F000
	uint8_t f000 = mem.readByte(0xF000);
	uint8_t f001 = mem.readByte(0xF001);
	LOG_INFO("KERNAL $F000 bytes: %02X %02X ...", f000, f001);

	// Confirm CPU reset vector bytes at $FFFC/$FFFD
	uint8_t rv_lo = mem.readByte(0xFFFC);
	uint8_t rv_hi = mem.readByte(0xFFFD);
	uint16_t rv = uint16_t(rv_hi) << 8 | rv_lo;
	LOG_INFO("Reset vector @ $FFFC = %02X %02X -> $%04X", rv_lo, rv_hi, rv);

	// (Optional) check IRQ/NMI vectors too
	uint8_t ir_lo = mem.readByte(0xFFFE), ir_hi = mem.readByte(0xFFFF);
	LOG_INFO("IRQ/BRK vector @ $FFFE = %02X %02X -> $%04X", ir_lo, ir_hi, (ir_hi << 8) | ir_lo);
	uint8_t nm_lo = mem.readByte(0xFFFA), nm_hi = mem.readByte(0xFFFB);
	LOG_INFO("NMI vector  @ $FFFA = %02X %02X -> $%04X", nm_lo, nm_hi, (nm_hi << 8) | nm_lo);

	// --- Reset PET ---
	g_pet->reset();

	LOG_INFO("BASIC $C000 bytes: %02X %02X %02X", mem.readByte(0xC000), mem.readByte(0xC001), mem.readByte(0xC002));
	LOG_INFO("BASIC $D000 bytes: %02X %02X %02X", mem.readByte(0xD000), mem.readByte(0xD001), mem.readByte(0xD002));

	// --- Minimal IEEE virtual drive setup ---
	const std::string filesRoot = "./files";
	g_pet->bus().io().setIeeeHostRoot(filesRoot);   // always mount the folder

	// Parse: -disk "filename.xxx" (resolved relative to ./files)
	const std::string diskArg = arg_get(argc, argv, "-disk");
	if (!diskArg.empty()) {
		const std::string hostPath = filesRoot + "/" + diskArg;

		if (ends_with_icase(diskArg, ".d64") || ends_with_icase(diskArg, ".d71")) {
			// Mount a disk image (.d64 = 1541, .d71 = 1571 double-sided)
			if (!g_pet->bus().io().setIeeeD64Image(hostPath)) {
				LOG_ERROR("[IEEE] Failed to mount disk image: %s", hostPath.c_str());
			}
			else {
				LOG_INFO("[IEEE] Disk image mounted: %s", hostPath.c_str());
			}
		}
		else {
			// Keep D64 disabled; folder backend will serve files by name.
			// Optional: auto-prime a PRG so LOAD\"\",8 works immediately.
			// If you prefer manual LOAD\"NAME\",8, simply comment this out.
			ieee_helpers::LoadPrgIntoIEEE(*g_pet, hostPath);   // requires pet_ieee_helpers.h
			LOG_INFO("[IEEE] Folder backend active. Use LOAD\"%s\",8  (or LOAD\"$\",8 + LIST)",
				diskArg.c_str());
		}
	}

	// --- Create the GL presenter (no swap/poll inside) ---
	g_gl = PetGL::create(SCREEN_W, SCREEN_H, "Commodore PET 2001", /*onKey*/nullptr);
	if (!g_gl) {
		LOG_ERROR("PetGL::create failed");
		std::exit(1);
	}

	set_pet_graphics_mode(false);

	// Tell the IEEE device about host root
	g_pet->bus().io().setIeeeHostRoot(filesRoot);

	//Setup the audio stream
	int frames_per_update = 44100 / 60;
	stream_data = (int16_t*)std::malloc(frames_per_update * sizeof(int16_t));

	for (int i = 0; i < frames_per_update; ++i)
	{
		if (stream_data) stream_data[i] = 0;
	}
	stream_start(1, 1, 16, 60, 0);

	// Configure the CB2 reconstruction module (1 MHz PET CPU -> 44100 output).
	g_cb2render.configure(/*fs*/ (double)sampleRate, /*cpuHz*/ 1000000.0, /*vol*/ 8000.0);
	g_cb2render.reset();

	// --- Input: SNES user-port adapter (optional, via pet.ini) ---
	SetIniFile("pet.ini");
	g_snes_enabled = get_config_bool("input", "snes_adapter", true);
	const bool snesInvert = get_config_bool("input", "snes_invert", false);
	install_joystick();
	g_pet->io().setSnesInvert(snesInvert);
	g_pet->io().setSnesEnabled(g_snes_enabled);
	LOG_INFO("SNES adapter: %s (invert=%d), joystick driver=%s",
		g_snes_enabled ? "enabled" : "disabled", snesInvert ? 1 : 0, joystick_driver_name());
}

// Eject a mounted .d64: clearing the image leaves hostRoot ("./files") in place,
// so device 8 falls straight back to the folder vdrive. No machine reset.
void pet_eject_disk() {
	if (!g_pet) return;
	if (!g_pet->bus().io().ieeeIsD64Mounted()) {
		LOG_INFO("[PET] eject: no disk mounted");
		return;
	}
	g_pet->bus().io().setIeeeD64Image("");   // unmount -> ./files folder vdrive
	LOG_INFO("[PET] disk ejected; device 8 back to ./files virtual drive");
}

int pet_get_disk_mounted() {
	return (g_pet && g_pet->bus().io().ieeeIsD64Mounted()) ? 1 : 0;
}

void pet_reset() { if (g_pet) g_pet->reset(); }

void pet_set_basic(int which) {
	if (which != 2 && which != 4 && which != 8) return;
	if (!load_basic_set(which)) return;   // PetMachine::loadRom keeps the CPU MEM mirror in sync
	g_pet->reset();
	LOG_INFO("[PET] switched to BASIC %d", which);
}

void pet_set_ram(int kb) {
	if (kb != 4 && kb != 8 && kb != 16 && kb != 32) return;
	if (!g_pet) return;
	g_pet->bus().setRamSize((size_t)kb * 1024);
	g_ram_kb = kb;
	g_pet->reset();
	LOG_INFO("[PET] RAM size set to %dK", kb);
}

void pet_set_crt(int on) { if (g_gl) g_gl->setCrtEnabled(on != 0); }
int  pet_get_crt()       { return (g_gl && g_gl->getCrtEnabled()) ? 1 : 0; }

void pet_set_monitor(int green) { if (g_gl) g_gl->setTintEnabled(green != 0); }
int  pet_get_monitor()          { return (g_gl && g_gl->getTintEnabled()) ? 1 : 0; }

void pet_set_speed(int mult) { g_speed_mult = (mult == 2) ? 2 : 1; }
int  pet_get_speed()         { return g_speed_mult; }

void pet_set_gfx_kbd(int on) { set_pet_graphics_mode(on != 0); }
int  pet_get_gfx_kbd()       { return get_pet_graphics_mode() ? 1 : 0; }

// SNES user-port adapter (gamepad input on the user port). Machine menu toggle.
void pet_set_snes(int on) {
	g_snes_enabled = (on != 0);
	if (g_pet) g_pet->io().setSnesEnabled(g_snes_enabled);
	LOG_INFO("[PET] SNES adapter %s", g_snes_enabled ? "enabled" : "disabled");
}
int  pet_get_snes()          { return g_snes_enabled ? 1 : 0; }

// View > CRT Monitor Settings dialog (live apply + ini save in PetGL)
float pet_shader_get(int idx)              { return g_gl ? g_gl->getKnob(idx) : 0.0f; }
void  pet_shader_set(int idx, float v)     { if (g_gl) g_gl->setKnob(idx, v); }
void  pet_shader_range(int idx, float* lo, float* hi, float* st) { if (g_gl) g_gl->knobRange(idx, lo, hi, st); }
void  pet_shader_defaults(void)            { if (g_gl) g_gl->restoreKnobDefaults(); }

// Reset to a clean BASIC and run the boot forward until the screen shows the
// "READY." prompt, the way VICE's autostart detects readiness (scan the screen,
// don't guess a delay). Needed because right after reset() the CPU has only had
// its PC set to the reset vector; BASIC's cold-start (RAM test, NEW, screen
// clear) runs over the next frames and would wipe a program injected too early.
// Returns true if "READY." was seen; false if the frame cap was hit first (we
// still inject - cold-start has almost certainly finished by then).
static bool reset_and_wait_for_ready()
{
	if (!g_pet) return false;
	g_pet->reset();

	// Reset preserves screen RAM (like real hardware), so the pre-reset
	// "READY." prompt is still sitting in $8000 and the scan below would
	// match it on frame 1 - before cold-start has run - and the subsequent
	// NEW would wipe the injected program. Blank the screen bytes first
	// (the KERNAL clears the screen during cold start anyway).
	for (uint16_t a = VIDRAM_ADDR; a <= VIDRAM_END; ++a)
		g_pet->bus().writeByte(a, 0x20);

	static const uint8_t READY[6] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2E }; // "READY." screen codes
	const int cyclesPerFrame = 1000000 / 60;
	const int maxFrames = 300;   // ~5 s emulated backstop; the loop exits as soon as READY appears

	auto screen_has_ready = []() -> bool {
		for (uint16_t base = VIDRAM_ADDR; base + 6 <= VIDRAM_END + 1; ++base) {
			bool hit = true;
			for (int k = 0; k < 6; ++k)
				if (g_pet->bus().readByte((uint16_t)(base + k)) != READY[k]) { hit = false; break; }
			if (hit) return true;
		}
		return false;
	};

	for (int f = 0; f < maxFrames; ++f) {
		g_pet->runCycles(cyclesPerFrame);
		if (screen_has_ready()) {
			LOG_INFO("[PET] reset: READY after %d frame(s)", f + 1);
			return true;
		}
	}
	LOG_WARN("[PET] reset: 'READY.' not seen within %d frames; injecting anyway", maxFrames);
	return false;
}

void pet_load_software(const char* utf8_path) {
	if (!g_pet || !utf8_path) return;
	std::string p = utf8_path;
	std::string ext; { size_t d = p.find_last_of('.'); if (d != std::string::npos) ext = p.substr(d + 1); }
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)tolower(c); });

	if (ext == "d64" || ext == "d71") {
		if (g_pet->bus().io().setIeeeD64Image(p))
			LOG_INFO("[PET] mounted %s '%s'", ext.c_str(), p.c_str());
		else
			LOG_ERROR("[PET] failed to mount %s '%s'", ext.c_str(), p.c_str());
		return;
	}
	// .prg: deposit the program directly into PET RAM at its own load address
	// (equivalent to LOAD"name",8,1). The CPU executes from the same RAM buffer
	// PetMem::writeByte() stores into, so the bytes are visible immediately.
	std::vector<uint8_t> file;
	if (!ieee_helpers::read_all_file(p, file) || file.size() <= 2) {
		// <= 2: a load address with zero payload is a truncated/corrupt PRG;
		// injecting it would leave BASIC with VARTAB==TXTTAB and garbage links.
		LOG_ERROR("[PET] failed to read PRG '%s' (missing or empty)", p.c_str());
		return;
	}

	auto& mem = g_pet->bus();
	const uint16_t loadAddr = (uint16_t)file[0] | ((uint16_t)file[1] << 8);
	const size_t   nbytes   = file.size() - 2;
	const uint32_t endAddr  = (uint32_t)loadAddr + (uint32_t)nbytes;   // exclusive

	// Reject rather than truncate: a partial load "succeeds" and then fails
	// bizarrely at run time (writeByte drops the tail, VARTAB points past RAM,
	// the first variable store corrupts BASIC state). Checked BEFORE the
	// reset below so a bad file leaves the running session untouched.
	if (endAddr > mem.ramConfiguredBytes()) {
		LOG_ERROR("[PET] PRG '%s' needs RAM to $%04X but only %zuK is configured - "
		          "not loaded (Machine > Memory to raise it)",
		          p.c_str(), (unsigned)endAddr, mem.ramConfiguredBytes() / 1024);
		return;
	}

	// Reboot to a clean BASIC before injecting, so loading a new program can't
	// corrupt/crash whatever is currently running. The ./files vdrive persists
	// across reset, and we wait for the READY prompt so cold-start doesn't wipe
	// the program we're about to inject.
	reset_and_wait_for_ready();

	for (size_t i = 0; i < nbytes; ++i) {
		const uint32_t a = (uint32_t)loadAddr + (uint32_t)i;
		if (a > 0xFFFF) break;                          // never wrap past $FFFF
		mem.writeByte((uint16_t)a, file[i + 2]);
	}

	if (loadAddr == 0x0401) {
		// BASIC program: make it LIST/RUN-able by setting the BASIC zero-page
		// pointers. PET layout (constant across BASIC 2/4): TXTTAB=$28/$29,
		// VARTAB=$2A/$2B, ARYTAB=$2C/$2D, STREND=$2E/$2F. VARTAB is the byte
		// after the program (start of variables = end of program).
		const uint16_t before = (uint16_t)(mem.readByte(0x28) | (mem.readByte(0x29) << 8));
		LOG_INFO("[PET] (sanity) TXTTAB $28/$29 read $%04X before load (expect $0401 once booted)", before);

		const uint16_t txttab = 0x0401;

		// Rebuild the forward line links for this load address, exactly as a
		// relocating LOAD does. Programs saved for a different start (e.g. a
		// $0801 C64-style image that nonetheless loads at $0401) carry links
		// that are wrong here; without relinking, RUN follows a broken chain.
		// (Run for the side effect; VARTAB stays at end-of-file so any trailing
		// ML after a BASIC SYS stub isn't clobbered by variables.)
		basic_relink(mem.ramData(), txttab, endAddr);
		const uint16_t vartab = (uint16_t)endAddr;
		mem.writeByte(0x28, (uint8_t)(txttab & 0xFF)); mem.writeByte(0x29, (uint8_t)(txttab >> 8));
		mem.writeByte(0x2A, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2B, (uint8_t)(vartab >> 8));
		mem.writeByte(0x2C, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2D, (uint8_t)(vartab >> 8)); // ARYTAB (defensive)
		mem.writeByte(0x2E, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2F, (uint8_t)(vartab >> 8)); // STREND (defensive)

		LOG_INFO("[PET] loaded BASIC PRG '%s' @ $%04X (%zu bytes); VARTAB=$%04X - type RUN",
		         p.c_str(), loadAddr, nbytes, vartab);
	}
	else {
		LOG_INFO("[PET] loaded ML PRG '%s' @ $%04X (%zu bytes) - run with SYS %u",
		         p.c_str(), loadAddr, nbytes, (unsigned)loadAddr);
	}
}

void emu_end()
{
	LOG_INFO("Calling Exit");
	remove_joystick();
	stream_stop(1, 1);
	mixer_end();
	std::free(stream_data);
	stream_data = nullptr;
	delete g_pet;
	g_pet = nullptr;
}