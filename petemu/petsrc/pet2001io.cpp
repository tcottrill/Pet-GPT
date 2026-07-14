// -----------------------------------------------------------------------------
// pet2001io.cpp
// -----------------------------------------------------------------------------
#include "pet2001io.h"

#include <algorithm>
#include <cstring>

#include "sys_log.h"
#include "pet_kbd_input.h"

// -----------------------------------------------------------------------------
// Pet2001IO::Pet2001IO
// -----------------------------------------------------------------------------
Pet2001IO::Pet2001IO(PetIEEE& ieeeBus,
	Pet2001Video& video,
	std::function<void(bool)> setIrqLine)
	: m_ieee(ieeeBus)
	, m_video(video)
	, m_pia1()
	, m_pia2()
	, m_via()
	, m_setIrq(std::move(setIrqLine))
{
	std::memset(keyrow_, 0xFF, sizeof(keyrow_));

	// Keyboard is driven by pet_kbd_input.cpp's update_keyboard() -> setKeyrows().
	reset();
}

// -----------------------------------------------------------------------------
// Pet2001IO::reset
// -----------------------------------------------------------------------------
void Pet2001IO::reset()
{
	m_pia1.reset();
	m_pia2.reset();

	// PIA2 idle high by default
	m_pia2.setPIA_PA_in(0xFF);
	m_pia2.setPIA_PB_in(0xFF);

	via_drb_in = 0x00;
	// User port (VIA Port A) idles HIGH via pull-ups on real hardware. Games like
	// pet-invaders auto-detect the controller by reading these bits; defaulting
	// them low made detection misfire and read "all buttons pressed" (stuck fire).
	via_dra_in = 0xFF;

	m_via.reset();
	m_snes.reset();
	refreshSnesData();   // PA6 idles "released" (no stuck fire)
	m_ieee.reset();

	// Initialize Video Logic
	// Cycle 0 = Start of V-BLANK (Video OFF)
	m_videoCycle = 0;

	// Set initial signal state: Low (Blank)
	// This will also prime the IRQ/PIA edge detectors
	setVideoOnSignal(false);

	// Force a clean IRQ drive after reset (even if false)
	m_lastIrqLevel = false;
	updateIrq(true);
}

// -----------------------------------------------------------------------------
// Pet2001IO::setIeeeHostRoot
// -----------------------------------------------------------------------------
void Pet2001IO::setIeeeHostRoot(const std::string& dir)
{
	m_ieee.setHostRoot(dir);
}

// -----------------------------------------------------------------------------
// Pet2001IO::setIeeeD64Image
// -----------------------------------------------------------------------------
bool Pet2001IO::setIeeeD64Image(const std::string& path)
{
	return m_ieee.setD64Image(path);
}

// -----------------------------------------------------------------------------
// Pet2001IO::ieeeIsD64Mounted
// -----------------------------------------------------------------------------
bool Pet2001IO::ieeeIsD64Mounted() const
{
	return m_ieee.isD64Mounted();
}

// -----------------------------------------------------------------------------
// Pet2001IO::setKeyrows
// -----------------------------------------------------------------------------
void Pet2001IO::setKeyrows(const uint8_t rows[10])
{
	for (int i = 0; i < 10; ++i)
		keyrow_[i] = rows[i];
}

// -----------------------------------------------------------------------------
// Pet2001IO::refreshPIA1KeyboardPB
// -----------------------------------------------------------------------------
void Pet2001IO::refreshPIA1KeyboardPB()
{
	uint8_t cra = m_pia1.getPIA_CRA();

	// Only meaningful when PA is configured as data port.
	if (!(cra & 0x04))
		return;

	uint8_t paOut = m_pia1.getPIA_PA_out();
	uint8_t row = (uint8_t)(paOut & 0x0F);

	uint8_t mask = 0xFF;
	if (row < 10)
		mask = keyrow_[row];

	m_pia1.setPIA_PB_in(mask);
}

// -----------------------------------------------------------------------------
// Pet2001IO::refreshSnesData
// Drive VIA Port A bit 6 (PA6 = SNES DATA) from the adapter's current bit.
// -----------------------------------------------------------------------------
void Pet2001IO::refreshSnesData()
{
	if (m_snes.dataBit()) via_dra_in |= SnesAdapter::DATA;
	else                  via_dra_in &= (uint8_t)~SnesAdapter::DATA;
	m_via.setPortAInput(via_dra_in);
}

// -----------------------------------------------------------------------------
// Pet2001IO::updateIrq
// -----------------------------------------------------------------------------
void Pet2001IO::updateIrq(bool force)
{
	const bool piaIrq = m_pia1.getIRQ();
	const bool viaIrq = m_via.irqLine();
	const bool irq = (piaIrq || viaIrq);

	if (!force && irq == m_lastIrqLevel)
		return;

	m_lastIrqLevel = irq;

	// Excessive logging removed for performance in cycle-exact mode
	if (force) {
		LOG_DEBUG("[PETIO] IRQ init: PIA1=%d VIA=%d -> IRQ=%d",
			piaIrq ? 1 : 0, viaIrq ? 1 : 0, irq ? 1 : 0);
	}

	if (m_setIrq)
		m_setIrq(irq);
}

// -----------------------------------------------------------------------------
// Pet2001IO::setVideoOnSignal
// -----------------------------------------------------------------------------
void Pet2001IO::setVideoOnSignal(bool active)
{
	// From "Theory of Operation":
	// The VIDEO ON signal is:
	//   LOW  during V-BLANK
	//   HIGH during Active Video
	//
	// Connections:
	//   1. PIA1 CB1: Trigger IRQ on High->Low transition (Start of V-Blank).
	//   2. VIA PB5:  Reads '0' during V-Blank (Snow check).

	// 1. Drive PIA1 CB1
	m_pia1.setSyncCB1(active);

	// 6545 status bit 7 = vertical retrace (LOW video-on phase = VBLANK)
	m_crtc.setVerticalRetrace(!active);

	// 2. Drive VIA PB5 (bit 5) input
	uint8_t pb_in = via_drb_in;
	if (active) pb_in |= 0x20;
	else        pb_in &= (uint8_t)~0x20;

	via_drb_in = pb_in;
	m_via.setPortBInput(pb_in);

	// Propagate any IRQ changes resulting from PIA edge detection
	updateIrq(false);
}

// -----------------------------------------------------------------------------
// Pet2001IO::read
// -----------------------------------------------------------------------------
uint8_t Pet2001IO::read(uint16_t a)
{
	const uint8_t addr = (uint8_t)(a & 0xFF);

	switch (addr) {
	// ---- 6545 CRTC ($E880/$E881, 8032) ----
	case 0x80: return m_crtc.readStatus();
	case 0x81: return m_crtc.readData();

		// ---------------- PIA1 ----------------
	case PIA1_PA:
	{
		// Update PA6 from IEEE EOI line if PA6 is input (DDRA bit6 == 0).
		const bool eoi = m_ieee.EOIin();

		uint8_t pa_in = m_pia1.getPIA_PA_in();
		const uint8_t ddra = m_pia1.getPIA_DDRA();

		if ((ddra & 0x40) == 0)
		{
			if (eoi) pa_in |= 0x40;
			else     pa_in &= (uint8_t)~0x40;
			m_pia1.setPIA_PA_in(pa_in);
		}

		const uint8_t cra = m_pia1.getPIA_CRA();
		if (cra & 0x04)
		{
			const uint8_t v = m_pia1.readPIA_PA();
			updateIrq(false);
			return v;
		}

		return m_pia1.getPIA_DDRA();
	}

	case PIA1_CRA:
		return m_pia1.read(PIA1_CRA);

	case PIA1_PB:
	{
		refreshPIA1KeyboardPB();
		const uint8_t v = m_pia1.read(PIA1_PB);
		updateIrq(false);
		return v;
	}

	case PIA1_CRB:
		return m_pia1.read(PIA1_CRB);

		// ---------------- PIA2 ----------------
	case PIA2_PA:
	{
		const uint8_t cra = m_pia2.getPIA_CRA();
		const uint8_t ddra = m_pia2.getPIA_DDRA();

		if (cra & 0x04)
		{
			const uint8_t bus = m_ieee.DIOin();

			// Prime the input pins from the IEEE bus, then read through the
			// PIA proper so the A-side IRQ flags (irq_a1/irq_a2) actually
			// clear. The old path wrote CRA&0x3F back, which cannot clear
			// them (readPIA_CRA synthesizes bits 7/6 from the booleans), so
			// an ATN edge left CRA bit 7 stuck high forever.
			uint8_t pa_in = m_pia2.getPIA_PA_in();
			pa_in = (uint8_t)((pa_in & ddra) | (bus & ~ddra));
			m_pia2.setPIA_PA_in(pa_in);

			const uint8_t v = m_pia2.readPIA_PA();
			updateIrq(false);

			// If DAV is released, no talker holds data: present the raw bus
			// (preserves the long-standing behavior the KERNAL relies on).
			if (!m_ieee.DAVin())
				return bus;

			return v;
		}

		return ddra;
	}

	case PIA2_PB:
	{
		const uint8_t crb = m_pia2.getPIA_CRB();
		const uint8_t ddrb = m_pia2.getPIA_DDRB();

		if (crb & 0x04)
		{
			if (ddrb != 0xFF)
			{
				const uint8_t bus = m_ieee.DIOin();
				uint8_t pb_in = m_pia2.getPIA_PB_in();
				pb_in = (uint8_t)((pb_in & ddrb) | (bus & ~ddrb));
				m_pia2.setPIA_PB_in(pb_in);
			}

			// Read through the PIA proper so irq_b1/irq_b2 clear (the old
			// CRB write-back could not clear the synthesized flag bits).
			const uint8_t v = m_pia2.readPIA_PB();
			updateIrq(false);
			return v;
		}

		return ddrb;
	}

	case PIA2_CRA:
		return m_pia2.read(PIA2_CRA);

	case PIA2_CRB:
	{
		uint8_t crb = m_pia2.getPIA_CRB();

		if (m_ieee.SRQin()) crb |= 0x80;
		else                crb &= (uint8_t)~0x80;

		m_pia2.write(PIA2_CRB, crb);
		return crb;
	}

	// ---------------- VIA ----------------
	case VIA_DRB:
	{
		// Live-in on inputs: PB7:DAVin, PB6:NRFDin, PB0:NDACin.
		// PB5 is maintained by setVideoOnSignal logic.
		const uint8_t ddrb = m_via.readReg(0x02);
		uint8_t pb_in = via_drb_in;

		if ((ddrb & 0x80) == 0) {
			const bool v = m_ieee.DAVin();
			if (v) pb_in |= 0x80; else pb_in &= (uint8_t)~0x80;
		}
		if ((ddrb & 0x40) == 0) {
			const bool v = m_ieee.NRFDin();
			if (v) pb_in |= 0x40; else pb_in &= (uint8_t)~0x40;
		}
		if ((ddrb & 0x01) == 0) {
			const bool v = m_ieee.NDACin();
			if (v) pb_in |= 0x01; else pb_in &= (uint8_t)~0x01;
		}

		via_drb_in = pb_in;
		m_via.setPortBInput(pb_in);

		const uint8_t val = m_via.readReg(0x00);
		updateIrq(false);
		return val;
	}

	case VIA_DRA:
	{
		refreshSnesData();   // present current SNES DATA bit on PA6
		const uint8_t val = m_via.readReg(0x01);
		updateIrq(false);
		return val;
	}

	case VIA_DDRB: return m_via.readReg(0x02);
	case VIA_DDRA: return m_via.readReg(0x03);

		// Match each access to its VIA register offset. Reading T1C-L (reg 0x04)
		// clears the T1 interrupt flag (IFR6) - the Method-A IRQ handler does
		// `lda VIA_T1CL` to ack the timer; previously this hit the latch register and
		// never cleared the flag.
	case VIA_T1CL: { const uint8_t v = m_via.readReg(0x04); updateIrq(false); return v; } // T1C-L (+clears IFR6)
	case VIA_T1CH: { const uint8_t v = m_via.readReg(0x05); updateIrq(false); return v; } // T1C-H
	case VIA_T1LL: { const uint8_t v = m_via.readReg(0x06); updateIrq(false); return v; } // T1L-L
	case VIA_T1LH: { const uint8_t v = m_via.readReg(0x07); updateIrq(false); return v; } // T1L-H

	case VIA_T2CL: { const uint8_t v = m_via.readReg(0x08); updateIrq(false); return v; }
	case VIA_T2CH: { const uint8_t v = m_via.readReg(0x09); updateIrq(false); return v; }

	case VIA_SR: { const uint8_t v = m_via.readReg(0x0A); updateIrq(false); return v; }

	case VIA_ACR: return m_via.readReg(0x0B);
	case VIA_PCR: return m_via.readReg(0x0C);
	case VIA_IFR: return m_via.readReg(0x0D);
	case VIA_IER: return m_via.readReg(0x0E);

	case VIA_ANH:
	{
		refreshSnesData();   // present current SNES DATA bit on PA6
		const uint8_t ddra = m_via.readReg(0x03);
		const uint8_t oraOut = m_via.getPortAOutput();
		const uint8_t v = (uint8_t)((via_dra_in & ~ddra) | (oraOut & ddra));
		return v;
	}

	default:
		return 0x00;
	}
}

// -----------------------------------------------------------------------------
// Pet2001IO::write
// -----------------------------------------------------------------------------
void Pet2001IO::write(uint16_t a, uint8_t d8)
{
	const uint8_t addr = (uint8_t)(a & 0xFF);

	switch (addr) {
		// ---------------- PIA1 ----------------
	case PIA1_PA:
	{
		const uint8_t cra = m_pia1.getPIA_CRA();
		if (cra & 0x04) m_pia1.writePIA_PA(d8);
		else            m_pia1.setPIA_DDRA(d8);

		refreshPIA1KeyboardPB();
		updateIrq(false);
		return;
	}

	case PIA1_PB:
		m_pia1.write(PIA1_PB, d8);
		updateIrq(false);
		return;

	case PIA1_CRA:
	{
		m_pia1.write(PIA1_CRA, d8);

		// CA2 high -> screen ON, CA2 low -> blank.
		const bool ca2 = m_pia1.getPIA_CA2_out();
		m_video.setVideoBlank(!ca2);

		// CA2 also drives IEEE EOIout.
		m_ieee.EOIout(ca2);

		updateIrq(false);
		return;
	}

	case PIA1_CRB:
		m_pia1.write(PIA1_CRB, d8);
		updateIrq(false);
		return;

		// ---------------- PIA2 ----------------
	case PIA2_PA:
		m_pia2.write(PIA2_PA, d8);
		return;

	case PIA2_PB:
	{
		const uint8_t crb = m_pia2.getPIA_CRB();
		m_pia2.write(PIA2_PB, d8);

		if (crb & 0x04)
		{
			const uint8_t out = m_pia2.getPIA_PB_out();
			m_ieee.DIOout(out);
		}
		return;
	}

	case PIA2_CRA:
	{
		m_pia2.write(PIA2_CRA, d8);
		const bool ndac = (d8 & 0x08) != 0;
		m_ieee.NDACout(ndac);
		return;
	}

	case PIA2_CRB:
	{
		m_pia2.write(PIA2_CRB, d8);
		const bool dav = (d8 & 0x08) != 0;
		m_ieee.DAVout(dav);
		return;
	}

	// ---------------- VIA ----------------
	case VIA_DRB:
	{
		m_via.writeReg(0x00, d8);
		updateIrq(false);

		const uint8_t ddrb = m_via.readReg(0x02);
		const uint8_t orb = m_via.getPortBOutput();

		if (ddrb & 0x04) {
			const bool atn = (orb & 0x04) != 0;
			m_ieee.ATNout(atn);
		}
		if (ddrb & 0x02) {
			const bool nrfd = (orb & 0x02) != 0;
			m_ieee.NRFDout(nrfd);
		}
		return;
	}

	case VIA_DRA:
		m_via.writeReg(0x01, d8);
		// SNES adapter: the game drives LATCH(PA5)/CLOCK(PA3) here; update DATA(PA6).
		m_snes.onPortAWrite(m_via.getPortAOutput());
		refreshSnesData();
		updateIrq(false);
		return;

	// DDR offsets now pass through 1:1 - the VIA core uses the real 6522
	// register map (reg 2 = DDRB, reg 3 = DDRA); no more cross-mapping.
	case VIA_DDRB:
		m_via.writeReg(0x02, d8);
		return;

	case VIA_DDRA:
		m_via.writeReg(0x03, d8);
		return;

		// VIA register offset == address low nibble. T1C-H (reg 0x05) is the write
		// that reloads + ARMS Timer 1; these were previously swapped with the latch
		// registers (0x06/0x07), so T1 never armed and Method-A (bass) notes were
		// silent. Map each access to its matching VIA register offset.
	case VIA_T1CL: m_via.writeReg(0x04, d8); updateIrq(false); return; // T1C-L (latch low)
	case VIA_T1CH: m_via.writeReg(0x05, d8); updateIrq(false); return; // T1C-H (reload + arm)
	case VIA_T1LL: m_via.writeReg(0x06, d8); updateIrq(false); return; // T1L-L
	case VIA_T1LH: m_via.writeReg(0x07, d8); updateIrq(false); return; // T1L-H

	case VIA_T2CL: m_via.writeReg(0x08, d8); updateIrq(false); return;
	case VIA_T2CH: m_via.writeReg(0x09, d8); updateIrq(false); return;

	case VIA_SR:   m_via.writeReg(0x0A, d8); updateIrq(false); return;

	case VIA_ACR:  m_via.writeReg(0x0B, d8); updateIrq(false); return;

	case VIA_PCR:
	{
		// CA2 is the charset line (chargen A10), and the line is PULLED UP on
		// the PET. So the effective level is high in EVERY CA2 mode except
		// manual-output-LOW (mode 110): input modes leave the pin undriven
		// (pull-up wins -> text set), handshake/pulse idle high, manual-high
		// is high. "A Bright Shining Star" (GP 2022) selects lowercase with
		// PCR=$12 - CA2 independent-INPUT mode - not manual-high; an earlier
		// version only honored manual modes and kept the demo in graphics.
		const uint8_t ca2mode = (uint8_t)((d8 >> 1) & 0x07);
		m_video.setCharset(ca2mode != 6);

		m_via.writeReg(0x0C, d8);
		updateIrq(false);
		return;
	}

	case VIA_IFR: m_via.writeReg(0x0D, d8); updateIrq(false); return;
	case VIA_IER: m_via.writeReg(0x0E, d8); updateIrq(false); return;

	case 0x80:  // 6545 CRTC address (8032)
		m_crtc.writeAddr(d8);
		return;
	case 0x81:  // 6545 CRTC data
		m_crtc.writeData(d8);
		if (m_crtc.geometryEpoch() != m_crtcEpoch) {
			m_crtcEpoch = m_crtc.geometryEpoch();
			if (m_crtc.cols() > 0)
				m_video.setColumns(m_crtc.cols() * 2);  // 8032: R1 counts 2-byte units
		}
		return;

	case VIA_ANH:
		m_via.writeReg(0x0F, d8);
		// SNES adapter: $E84F is the CONVENTIONAL way to drive user-port
		// LATCH(PA5)/CLOCK(PA3) - it's ORA without the CA2 handshake ($E841
		// accesses touch CA2, the charset line). Mirror the VIA_DRA hook or
		// the adapter never sees the edges and the pad reads dead/stuck.
		m_snes.onPortAWrite(m_via.getPortAOutput());
		refreshSnesData();
		return;

	default:
		LOG_ERROR("[PETIO] Write to unmapped IO address %04X data %02X", a, d8);
		return;
	}
}

// -----------------------------------------------------------------------------
// Pet2001IO::cycle
// -----------------------------------------------------------------------------
void Pet2001IO::cycle()
{
	// Tick both PIAs and VIA once per CPU cycle.
	m_pia1.tick();
	m_pia2.tick();

	m_via.setPortAInput(via_dra_in);
	m_via.setPortBInput(via_drb_in);
	m_via.tick();

	// IMPORTANT: IRQ sources (timers/edges) change during tick().
	// Recompute combined IRQ each cycle so CPU sees level IRQ correctly.
	updateIrq(false);

	// -------------------------------------------------------------------------
	// Video Timing Logic (Theory of Operation)
	// -------------------------------------------------------------------------
	m_videoCycle++;
	if (m_videoCycle >= m_frameCycles)
	{
		m_videoCycle = 0;
		// Timing source can change (8032 editor programs the CRTC; ROM-set
		// switch flips 40<->80 columns). Re-evaluate at the frame boundary so
		// a frame in flight never sees its geometry move under it.
		refreshFrameTiming();
	}

	// Update Signals on transitions
	if (m_videoCycle == 0)
	{
		// Cycle 0: Start of V-BLANK.
		// VIDEO ON goes LOW.
		// Falling edge on PIA1 CB1 triggers System Interrupt (IRQ).
		setVideoOnSignal(false);
	}
	else if (m_videoCycle == m_vblankEnd)
	{
		// End of V-BLANK. VIDEO ON goes HIGH.
		setVideoOnSignal(true);
	}
}

// -----------------------------------------------------------------------------
// Pet2001IO::refreshFrameTiming
// 40-col machines use the fixed discrete-logic frame (16640/3840). In
// 80-column mode the CRTC generates sync, so derive the frame from its
// register file: this makes the 60 Hz editor's real cadence (and a 50 Hz
// editor ROM's 50 Hz) come out automatically. The sanity window guards
// against a half-programmed register file mid-init.
// -----------------------------------------------------------------------------
void Pet2001IO::refreshFrameTiming()
{
	uint32_t frame = kCyclesPerFrame;
	uint32_t vblank = kVBlankEnd;

	if (m_video.columns() == 80)
	{
		const int fc = m_crtc.frameCycles();
		const int vb = m_crtc.vblankCycles();
		// Accept ~40..83 Hz worth of frame and a nonempty blank window.
		if (fc >= 12000 && fc <= 25000 && vb > 0 && vb < fc)
		{
			frame = (uint32_t)fc;
			vblank = (uint32_t)vb;
		}
	}

	if (frame != m_frameCycles || vblank != m_vblankEnd)
	{
		LOG_INFO("[PETIO] frame timing: %u cycles/frame (%.3f Hz), vblank %u cycles",
			frame, 1000000.0 / frame, vblank);
		m_frameCycles = frame;
		m_vblankEnd = vblank;
	}
}

// -----------------------------------------------------------------------------
// Pet2001IO::ieeeLoadData
// -----------------------------------------------------------------------------
void Pet2001IO::ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes)
{
	m_ieee.ieeeLoadData(addr, bytes);
}

// -----------------------------------------------------------------------------
// VIA CB2 audio helpers
// -----------------------------------------------------------------------------
void Pet2001IO::cb2ResetEdgeLog()
{
	m_via.cb2_reset_edge_log();
}

uint32_t Pet2001IO::cb2GetEdgeCount() const
{
	return m_via.cb2_get_edge_count();
}

const CB2Edge* Pet2001IO::cb2GetEdges() const
{
	return m_via.cb2_get_edges();
}

uint32_t Pet2001IO::cb2GetTickCounter() const
{
	return m_via.cb2_get_tick_counter();
}