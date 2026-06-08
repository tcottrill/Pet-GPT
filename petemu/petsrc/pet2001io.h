// -----------------------------------------------------------------------------
// pet2001io.h
// -----------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <vector>

#include "pet2001ieee.h"
#include "pet2001video.h"
#include "via6522.h"
#include "pia6520.h"
#include "snes_adapter.h"

// -----------------------------------------------------------------------------
// Pet2001IO
// -----------------------------------------------------------------------------
// Models:
//   - PIA1 and PIA2 via PIA6520 (keyboard, sync, IEEE glue).
//   - VIA 6522 via VIA6522 (timers, handshake, charset toggle, CB2 edges).
//
// TIMING IMPLEMENTATION:
//   Based on "PET 2001 Graphics — Theory of Operation".
//   The video logic consists of discrete TTL counters driven by the system clock.
//   - Scanline: 64 Cycles.
//   - Frame: 260 Scanlines (16,640 Cycles).
//   - V-Blank: 3,840 Cycles (Cycles 0-3839).
//   - Active: 12,800 Cycles (Cycles 3840-16639).
//
//   Signals:
//   - VIDEO ON (VIA PB5, PIA1 CB1): 
//       LOW  during V-Blank (0..3839).
//       HIGH during Active (3840..16639).
//   - IRQ: Triggered on the High->Low transition of VIDEO ON (Cycle 0).
// -----------------------------------------------------------------------------
class Pet2001IO {
public:
	// Low-8-bit register addresses
	enum : uint8_t {
		// PIA1
		PIA1_PA = 0x10,
		PIA1_CRA = 0x11,
		PIA1_PB = 0x12,
		PIA1_CRB = 0x13,

		// PIA2
		PIA2_PA = 0x20,
		PIA2_CRA = 0x21,
		PIA2_PB = 0x22,
		PIA2_CRB = 0x23,

		// VIA
		VIA_DRB = 0x40,
		VIA_DRA = 0x41,
		VIA_DDRB = 0x42,
		VIA_DDRA = 0x43,
		VIA_T1CL = 0x44,
		VIA_T1CH = 0x45,
		VIA_T1LL = 0x46,
		VIA_T1LH = 0x47,
		VIA_T2CL = 0x48,
		VIA_T2CH = 0x49,
		VIA_SR = 0x4A,
		VIA_ACR = 0x4B,
		VIA_PCR = 0x4C,
		VIA_IFR = 0x4D,
		VIA_IER = 0x4E,
		VIA_ANH = 0x4F
	};

public:
	Pet2001IO(PetIEEE& ieeeBus,
		Pet2001Video& video,
		std::function<void(bool)> setIrqLine);

	void reset();

	// Host IEEE helpers
	void setIeeeHostRoot(const std::string& dir);
	bool setIeeeD64Image(const std::string& path);
	bool ieeeIsD64Mounted() const;            // true while a .d64 image is mounted

	// Keyboard rows (10 rows), each byte is active-low bitmask.
	void setKeyrows(const uint8_t rows[10]);

	// CPU bus access
	uint8_t read(uint16_t io_lo8);
	void    write(uint16_t io_lo8, uint8_t data);

	// Called once per CPU cycle
	void cycle();

	// VIA CB2 audio helpers
	void cb2ResetEdgeLog();
	uint32_t cb2GetEdgeCount() const;
	const CB2Edge* cb2GetEdges() const;
	uint32_t cb2GetTickCounter() const;

	// SNES adapter (user-port joystick) emulation.
	void setSnesEnabled(bool e) { m_snes.setEnabled(e); refreshSnesData(); }
	void setSnesInvert(bool inv) { m_snes.setInvert(inv); refreshSnesData(); }
	void setSnesButtons(uint16_t pressedMask) { m_snes.setButtons(pressedMask); }
	bool cb2GetFrameStartLevel() const { return m_via.GetCB2LevelAtFrameStart(); }

	// VIA CA2 helpers
	bool getViaCA2Output() const { return m_via.getCA2Output(); }
	void setViaCA2Input(bool level) { m_via.setCA2(level); }

	// IEEE data preloading
	void ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes);

private:
	// Recompute combined IRQ and drive m_setIrq only on change.
	void updateIrq(bool force);

	// Ensure PIA1.PB_in reflects current keyboard row selected by PIA1.PA_out.
	void refreshPIA1KeyboardPB();

	// Push the SNES adapter's current DATA level onto VIA Port A bit 6 (PA6).
	void refreshSnesData();

	// Update the VIDEO ON signal driving PIA1 CB1 and VIA PB5
	void setVideoOnSignal(bool active);

private:
	PetIEEE& m_ieee;
	Pet2001Video& m_video;
	PIA6520 m_pia1;
	PIA6520 m_pia2;
	VIA6522 m_via;
	SnesAdapter m_snes;

	// Keyboard matrix rows (active-low)
	uint8_t keyrow_[10] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

	// IRQ sink (level-sensitive)
	std::function<void(bool)> m_setIrq;
	bool m_lastIrqLevel = false;

	// VIA external port input state (inputs only)
	uint8_t via_drb_in = 0x00;
	uint8_t via_dra_in = 0x00;

	// -------------------------------------------------------------------------
	// Video Timing State
	// -------------------------------------------------------------------------
	// Total cycles per frame: 25 rows * 8 lines * 64 cycles = 12,800 active
	// + 3,840 V-Blank = 16,640 total.
	static constexpr uint32_t kCyclesPerFrame = 16640;
	static constexpr uint32_t kVBlankEnd = 3840;

	uint32_t m_videoCycle = 0;
};