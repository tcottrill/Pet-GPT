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
		uint8_t pa_in = m_pia2.getPIA_PA_in();
		const uint8_t pa_out = m_pia2.getPIA_PA_out();

		if (cra & 0x04)
		{
			// Clear selected IRQ flags (as prior code did)
			if (cra & 0xC0)
			{
				const uint8_t newcra = (uint8_t)(cra & 0x3F);
				m_pia2.write(PIA2_CRA, newcra);
			}

			const uint8_t bus = m_ieee.DIOin();

			// If DAV asserted, the drive is holding data; otherwise allow bus read.
			if (!m_ieee.DAVin())
				return bus;

			pa_in = (uint8_t)((pa_in & ddra) | (bus & ~ddra));
			m_pia2.setPIA_PA_in(pa_in);

			return (uint8_t)((pa_in & ~ddra) | (pa_out & ddra));
		}

		return ddra;
	}

	case PIA2_PB:
	{
		const uint8_t crb = m_pia2.getPIA_CRB();
		const uint8_t ddrb = m_pia2.getPIA_DDRB();
		uint8_t pb_in = m_pia2.getPIA_PB_in();
		const uint8_t pb_out = m_pia2.getPIA_PB_out();

		if (crb & 0x04)
		{
			if ((crb & 0x3F) != 0)
			{
				const uint8_t newcrb = (uint8_t)(crb & 0x3F);
				m_pia2.write(PIA2_CRB, newcrb);
			}

			if (ddrb != 0xFF)
			{
				const uint8_t bus = m_ieee.DIOin();
				pb_in = (uint8_t)((pb_in & ddrb) | (bus & ~ddrb));
				m_pia2.setPIA_PB_in(pb_in);
			}

			return (uint8_t)((pb_in & ~ddrb) | (pb_out & ddrb));
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
		const uint8_t ddrb = m_via.readReg(0x03);
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

	case VIA_DDRB: return m_via.readReg(0x03);
	case VIA_DDRA: return m_via.readReg(0x02);

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
		const uint8_t ddra = m_via.readReg(0x02);
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

		const uint8_t ddrb = m_via.readReg(0x03);
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

	case VIA_DDRB:
		m_via.writeReg(0x03, d8);
		return;

	case VIA_DDRA:
		m_via.writeReg(0x02, d8);
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
		// Preserve PET-specific charset toggle behavior
		const uint8_t old_pcr = m_via.readReg(0x0C);
		const bool old_is_toggle = ((old_pcr & 0x0C) == 0x0C);
		const bool new_is_toggle = ((d8 & 0x0C) == 0x0C);

		if (old_is_toggle && new_is_toggle && ((old_pcr ^ d8) & 0x02))
			m_video.setCharset((d8 & 0x02) != 0);

		m_via.writeReg(0x0C, d8);
		updateIrq(false);
		return;
	}

	case VIA_IFR: m_via.writeReg(0x0D, d8); updateIrq(false); return;
	case VIA_IER: m_via.writeReg(0x0E, d8); updateIrq(false); return;

	case VIA_ANH:
		m_via.writeReg(0x0F, d8);
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
	if (m_videoCycle >= kCyclesPerFrame)
	{
		m_videoCycle = 0;
	}

	// Update Signals on transitions
	if (m_videoCycle == 0)
	{
		// Cycle 0: Start of V-BLANK.
		// VIDEO ON goes LOW.
		// Falling edge on PIA1 CB1 triggers System Interrupt (IRQ).
		setVideoOnSignal(false);
	}
	else if (m_videoCycle == kVBlankEnd)
	{
		// Cycle 3840: End of V-BLANK.
		// VIDEO ON goes HIGH.
		setVideoOnSignal(true);
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