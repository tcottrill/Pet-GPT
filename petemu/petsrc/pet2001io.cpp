//
// Copyright (c) 2012,2014 Thomas Skibo.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//

// pet2001io.c
//
//      This is the hard part: modeling the PET hardware.  I first did this
//      in Verilog, then C, and now Javascript.
//

#include "pet2001io.h"
#include <algorithm>
#include <cstring>
#include "sys_log.h"

Pet2001IO::Pet2001IO(PetIEEE& ieeeBus,
	Pet2001Video& video,
	std::function<void(bool)> setIrqLine)
	: m_ieee(ieeeBus)
	, m_video(video)
	, m_setIrq(std::move(setIrqLine))
{
	reset();
}

void Pet2001IO::setIeeeHostRoot(const std::string& dir) {
	m_ieee.setHostRoot(dir);
}

bool Pet2001IO::setIeeeD64Image(const std::string& path) {
	return m_ieee.setD64Image(path);
}

void Pet2001IO::setCpuClockHz(double hz) { cpu_hz_ = (hz > 1.0 ? hz : 1.0); recomputeSyncDda(); }
void Pet2001IO::setRefreshHz(double hz) { refresh_hz_ = (hz > 1e-6 ? hz : 60.0); recomputeSyncDda(); }
void Pet2001IO::setSyncDutyLow(double frac) {
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;
	duty_low_ = frac;
	recomputeSyncDda();
}

void Pet2001IO::recomputeSyncDda()
{
	// phase increments by: refresh_hz / cpu_hz per CPU cycle, in Q0.32
	// inc_ = round( (refresh_hz / cpu_hz) * 2^32 )
	const long double ratio = (long double)refresh_hz_ / (long double)cpu_hz_;
	const unsigned long long full = 1ull << 32;
	unsigned long long inc64 = (unsigned long long)(ratio * (long double)full + 0.5L);
	if (inc64 == 0) inc64 = 1; // ensure forward progress even for extreme values
	inc_ = (uint32_t)inc64;

	// threshold within the cycle when we switch to low (false)
	// duty_low_ is the fraction of the period for which SYNC is low
	// We make SYNC high from 0 .. (1 - duty_low_) and low from duty threshold .. wrap
	unsigned long long thr64 = (unsigned long long)((1.0 - duty_low_) * (long double)full + 0.5L);
	if (thr64 >= full) thr64 = full - 1;
	duty_thr_ = (uint32_t)thr64;
}

void Pet2001IO::reset()
{
	keyrow.fill(0xFF);

	pia1_pa_in = 0xF0;
	pia1_pa_out = 0x00;
	pia1_ddra = 0x00;
	pia1_cra = 0x00;
	pia1_pb_in = 0xFF;
	pia1_pb_out = 0x00;
	pia1_ddrb = 0x00;
	pia1_crb = 0x00;
	pia1_ca2 = 0x00;
	pia1_cb1 = 0x00;

	pia2_pa_in = 0x00;
	pia2_pa_out = 0x00;
	pia2_ddra = 0x00;
	pia2_cra = 0x00;
	pia2_pb_in = 0x00;
	pia2_pb_out = 0x00;
	pia2_ddrb = 0x00;
	pia2_crb = 0x00;

	via_drb_in = 0x00;
	via_drb_out = 0x00;
	via_dra_in = 0x00;
	via_dra_out = 0x00;
	via_ddrb = 0x00;
	via_ddra = 0x00;

	via_t1cl = 0xFF;
	via_t1ch = 0xFF;
	via_t1_1shot = 0x00;
	via_t1ll = 0xFF;
	via_t1lh = 0xFF;

	via_t2cl = 0xFF;
	via_t2ch = 0xFF;
	via_t2_1shot = 0x00;
	via_t2ll = 0x00;

	via_sr = 0x00;
	via_acr = 0x00;
	via_pcr = 0x00;
	via_ifr = 0x00;
	via_ier = 0x80;

	video_cycle = 0;

	m_ieee.reset();

	// Initialize sync DDA
	phase_ = 0;
	//sync_level_ = true;   // we start periods as "true" (high)
	video_cycle = 0;
	recomputeSyncDda();

	updateIrq();
}

void Pet2001IO::updateIrq()
{
	bool irq = false;

	// PIA1 IRQ conditions (match JS)
	if (((pia1_cra & 0x81) == 0x81) || ((pia1_cra & 0x48) == 0x48) ||
		((pia1_crb & 0x81) == 0x81) || ((pia1_crb & 0x48) == 0x48))
	{
		irq = true;
	}

	// VIA IRQ combine: if any enabled IFR bit set, set IFR7 and assert
	if ((via_ifr & via_ier & 0x7F) != 0) {
		via_ifr |= 0x80;
		irq = true;
	}
	else {
		via_ifr &= ~0x80;
	}

	if (m_setIrq) m_setIrq(irq);
}

void Pet2001IO::setKeyrows(const uint8_t rows[10])
{
	for (int i = 0; i < 10; ++i) keyrow[i] = rows[i];

	// Update PIA1.PB input from currently selected row (low nibble of PA_out)
	if ((pia1_pa_out & 0x0F) < 10)
		pia1_pb_in = keyrow[pia1_pa_out & 0x0F];
}

void Pet2001IO::sync(bool sig)
{
	// SYNC feeds PIA1.CB1 edge logic and VIA.PB5 input bit
	if (sig != (pia1_cb1 != 0)) {
		// PIA1.CRB bit1 selects edge polarity
		const bool rising = sig;
		const bool fall = !sig;
		const bool posEdge = (pia1_crb & 0x02) != 0;

		if ((posEdge && rising) || (!posEdge && fall)) {
			pia1_crb |= 0x80; // IRQ flag
			if ((pia1_crb & 0x01) != 0) updateIrq();
		}
		pia1_cb1 = sig ? 1 : 0;
	}

	// VIA PB5 reflects sync level as an input (leave outputs in via_drb_out alone)
	if (sig) via_drb_in |= 0x20;
	else     via_drb_in &= ~0x20;
}

uint8_t Pet2001IO::read(uint16_t a)
{
	const uint8_t addr = static_cast<uint8_t>(a & 0xFF);

	//LOG_DEBUG("IO READ: addr=%02X", addr);

	switch (addr) {
		// ---------------- PIA1 ----------------
	case PIA1_PA:
		if (pia1_cra & 0x04) {
			// side-effect: clear CRA IRQ flags on read
			if (pia1_cra & 0xC0) {
				//LOG_DEBUG("PIA1_PA read: clearing CRA IRQ flags (old=%02X)", pia1_cra);
				pia1_cra &= 0x3F;
				updateIrq();
			}
			// Bit6 can be EOI (input) if configured as input (ddra bit6 = 0)
			if ((pia1_ddra & 0x40) == 0) {
				const bool eoi = m_ieee.EOIin();
				const uint8_t prev_in = pia1_pa_in;
				if (eoi) pia1_pa_in |= 0x40;
				else     pia1_pa_in &= (uint8_t)~0x40;
				//LOG_DEBUG("PIA1_PA read: EOIin=%d pa_in:%02X->%02X", eoi ? 1 : 0, prev_in, pia1_pa_in);
			}
			const uint8_t composed = (uint8_t)((pia1_pa_in & ~pia1_ddra) | (pia1_pa_out & pia1_ddra));
			//LOG_DEBUG("PIA1_PA read: in=%02X out=%02X ddr=%02X -> %02X",	pia1_pa_in, pia1_pa_out, pia1_ddra, composed);
			return composed;
		}
		//LOG_DEBUG("PIA1_DDRA read -> %02X", pia1_ddra);
		return pia1_ddra;

	case PIA1_CRA:
		//LOG_DEBUG("PIA1_CRA read -> %02X", pia1_cra);
		return pia1_cra;

	case PIA1_PB:
		if (pia1_crb & 0x04) {
			// side-effect: clear CRB IRQ flags on read
			if (pia1_crb & 0xC0) {
				//LOG_DEBUG("PIA1_PB read: clearing CRB IRQ flags (old=%02X)", pia1_crb);
				pia1_crb &= 0x3F;
				updateIrq();
			}
			const uint8_t composed = (uint8_t)((pia1_pb_in & ~pia1_ddrb) | (pia1_pb_out & pia1_ddrb));
			//LOG_DEBUG("PIA1_PB read: in=%02X out=%02X ddr=%02X -> %02X",pia1_pb_in, pia1_pb_out, pia1_ddrb, composed);
			return composed;
		}
		//LOG_DEBUG("PIA1_DDRB read -> %02X", pia1_ddrb);
		return pia1_ddrb;

	case PIA1_CRB:
		//LOG_DEBUG("PIA1_CRB read -> %02X", pia1_crb);
		return pia1_crb;

		// ---------------- PIA2 ----------------
	case PIA2_PA:
		// Layer 1/2: PET reads IEEE-488 DIO here.
		// IMPORTANT: When a device is TALKing (DAV low) the PET samples the live
		// 8-bit bus, regardless of DDRA. Returning latch bits on 'output' pins
		// creates the "every other bit" artifact you observed.
		if (pia2_cra & 0x04) {
			// Clear CRA IRQ flags on read.
			if (pia2_cra & 0xC0) {
				//LOG_DEBUG("PIA2_PA read: clearing CRA IRQ flags (old=%02X)", pia2_cra);
				pia2_cra &= 0x3F;
				updateIrq();
			}

			const uint8_t bus = m_ieee.DIOin();
			const bool talk_data_phase = (m_ieee.DAVin() == false); // DAV low means data valid from talker
			//LOG_DEBUG("PIA2_PA read: DIOin=%02X DAVin=%d (talk_data_phase=%d) ddra=%02X pa_in=%02X pa_out=%02X",	bus, m_ieee.DAVin() ? 1 : 0, talk_data_phase ? 1 : 0, pia2_ddra, pia2_pa_in, pia2_pa_out);

			if (talk_data_phase) {
				// Device is TALKing: return the live bus, all 8 bits.
				// This matches how the PET actually samples IEEE-488 during GET#/INPUT#.
				//LOG_DEBUG("PIA2_PA read: TALK phase -> returning BUS=%02X", bus);
				return bus;
			}

			// Not in a talking data phase: refresh input bits and compose like a vanilla PIA.
			const uint8_t prev_in = pia2_pa_in;
			pia2_pa_in = (uint8_t)((pia2_pa_in & pia2_ddra) | (bus & ~pia2_ddra));
			const uint8_t composed = (uint8_t)((pia2_pa_in & ~pia2_ddra) | (pia2_pa_out & pia2_ddra));
			//LOG_DEBUG("PIA2_PA read: idle phase update pa_in:%02X->%02X; out=%02X ddr=%02X -> %02X",	prev_in, pia2_pa_in, pia2_pa_out, pia2_ddra, composed);
			return composed;
		}
		//LOG_DEBUG("PIA2_DDRA read -> %02X", pia2_ddra);
		return pia2_ddra;

	case PIA2_CRA:
		//LOG_DEBUG("PIA2_CRA read -> %02X", pia2_cra);
		return pia2_cra;

	case PIA2_PB:
		// Layer 1/2 data path (DIO), controller side:
		// PB is used by PET to *drive* the bus when sending (addressing/filename).
		// When reading PB, mirror live bus on input bits so mixed DDRs behave.
		if (pia2_crb & 0x04) {
			// side-effect: clear CRB IRQ flags on read (if any of the low 6 set)
			if ((pia2_crb & 0x3F) != 0) {
				//LOG_DEBUG("PIA2_PB read: clearing CRB IRQ flags (old=%02X)", pia2_crb);
				pia2_crb &= 0x3F;
				updateIrq();
			}

			if ((pia2_ddrb & 0xFF) != 0xFF) {
				const uint8_t bus = m_ieee.DIOin();
				const uint8_t prev_in = pia2_pb_in;
				// Only update the input (0) bits; keep latched outputs in pb_in
				pia2_pb_in = (uint8_t)((pia2_pb_in & pia2_ddrb) | (bus & ~pia2_ddrb));
				//LOG_DEBUG("PIA2_PB read: DIOin=%02X pb_in:%02X->%02X ddr=%02X", bus, prev_in, pia2_pb_in, pia2_ddrb);
			}
			else {
				//LOG_DEBUG("PIA2_PB read: DDRB=FF (all outputs), pb_in left unchanged=%02X", pia2_pb_in);
			}

			const uint8_t composed = (uint8_t)((pia2_pb_in & ~pia2_ddrb) | (pia2_pb_out & pia2_ddrb));
			//LOG_DEBUG("PIA2_PB read: in=%02X out=%02X ddr=%02X -> %02X",pia2_pb_in, pia2_pb_out, pia2_ddrb, composed);
			return composed;
		}
		//LOG_DEBUG("PIA2_DDRB read -> %02X", pia2_ddrb);
		return pia2_ddrb;

	case PIA2_CRB:
		if (m_ieee.SRQin()) pia2_crb |= 0x80;
		else                pia2_crb &= (uint8_t)~0x80;
		//LOG_DEBUG("PIA2_CRB read: SRQin=%d -> %02X", m_ieee.SRQin() ? 1 : 0, pia2_crb);
		return pia2_crb;

		// ---------------- VIA ----------------
	case VIA_DRB: {
		// Side effects: clear CB2 (IFR3) if not independent, and CB1 (IFR4)
		if ((via_pcr & 0xA0) != 0x20) { // CB2 not independent
			if (via_ifr & 0x08) {
				//LOG_DEBUG("VIA_DRB read: clearing IFR3 (CB2)");
				via_ifr &= (uint8_t)~0x08;
				if (via_ier & 0x08) updateIrq();
			}
		}
		if (via_ifr & 0x10) {
			//LOG_DEBUG("VIA_DRB read: clearing IFR4 (CB1)");
			via_ifr &= (uint8_t)~0x10;
			if (via_ier & 0x10) updateIrq();
		}

		// Live-in on inputs: PB7:DAVin (active-low), PB6:NRFDin (active-low), PB0:NDACin (active-low)
		if ((via_ddrb & 0x80) == 0) {
			const bool v = m_ieee.DAVin();
			if (v) via_drb_in |= 0x80; else via_drb_in &= (uint8_t)~0x80;
			//LOG_DEBUG("VIA_DRB read: DAVin=%d -> in=%02X", v ? 1 : 0, via_drb_in);
		}
		if ((via_ddrb & 0x40) == 0) {
			const bool v = m_ieee.NRFDin();
			if (v) via_drb_in |= 0x40; else via_drb_in &= (uint8_t)~0x40;
			//LOG_DEBUG("VIA_DRB read: NRFDin=%d -> in=%02X", v ? 1 : 0, via_drb_in);
		}
		if ((via_ddrb & 0x01) == 0) {
			const bool v = m_ieee.NDACin();
			if (v) via_drb_in |= 0x01; else via_drb_in &= (uint8_t)~0x01;
			//LOG_DEBUG("VIA_DRB read: NDACin=%d -> in=%02X", v ? 1 : 0, via_drb_in);
		}

		const uint8_t composed = (uint8_t)((via_drb_in & ~via_ddrb) | (via_drb_out & via_ddrb));
		//LOG_DEBUG("VIA_DRB read: in=%02X out=%02X ddr=%02X -> %02X",	via_drb_in, via_drb_out, via_ddrb, composed);
		return composed;
	}

	case VIA_DRA: {
		// Side effects: clear CA2 (IFR0) if not independent, and CA1 (IFR1)
		if ((via_pcr & 0x0A) != 0x02) {
			if (via_ifr & 0x01) {
				//LOG_DEBUG("VIA_DRA read: clearing IFR0 (CA2)");
				via_ifr &= (uint8_t)~0x01;
				if (via_ier & 0x01) updateIrq();
			}
		}
		if (via_ifr & 0x02) {
			//LOG_DEBUG("VIA_DRA read: clearing IFR1 (CA1)");
			via_ifr &= (uint8_t)~0x02;
			if (via_ier & 0x02) updateIrq();
		}
		const uint8_t composed = (uint8_t)((via_dra_in & ~via_ddra) | (via_dra_out & via_ddra));
		//LOG_DEBUG("VIA_DRA read: in=%02X out=%02X ddr=%02X -> %02X",	via_dra_in, via_dra_out, via_ddra, composed);
		return composed;
	}

	case VIA_DDRB:
		//LOG_DEBUG("VIA_DDRB read -> %02X", via_ddrb);
		return via_ddrb;

	case VIA_DDRA:
		//LOG_DEBUG("VIA_DDRA read -> %02X", via_ddra);
		return via_ddra;

	case VIA_T1CL:
		// Clear T1 IFR6 on read T1CL
		if (via_ifr & 0x40) {
			//LOG_DEBUG("VIA_T1CL read: clearing IFR6 (T1)");
			via_ifr &= (uint8_t)~0x40;
			if (via_ier & 0x40) updateIrq();
		}
		//LOG_DEBUG("VIA_T1CL read -> %02X", via_t1cl);
		return via_t1cl;

	case VIA_T1CH:
		//LOG_DEBUG("VIA_T1CH read -> %02X", via_t1ch);
		return via_t1ch;

	case VIA_T1LL:
		//LOG_DEBUG("VIA_T1LL read -> %02X", via_t1ll);
		return via_t1ll;

	case VIA_T1LH:
		//LOG_DEBUG("VIA_T1LH read -> %02X", via_t1lh);
		return via_t1lh;

	case VIA_T2CL:
		// Clear T2 IFR5 on read T2CL
		if (via_ifr & 0x20) {
			//LOG_DEBUG("VIA_T2CL read: clearing IFR5 (T2)");
			via_ifr &= (uint8_t)~0x20;
			if (via_ier & 0x20) updateIrq();
		}
		//LOG_DEBUG("VIA_T2CL read -> %02X", via_t2cl);
		return via_t2cl;

	case VIA_T2CH:
		//LOG_DEBUG("VIA_T2CH read -> %02X", via_t2ch);
		return via_t2ch;

	case VIA_SR:
		if (via_ifr & 0x04) {
			//LOG_DEBUG("VIA_SR read: clearing IFR2 (SR)");
			via_ifr &= (uint8_t)~0x04;
			if (via_ier & 0x04) updateIrq();
		}
		//LOG_DEBUG("VIA_SR read -> %02X", via_sr);
		return via_sr;

	case VIA_ACR:
		//LOG_DEBUG("VIA_ACR read -> %02X", via_acr);
		return via_acr;

	case VIA_PCR:
		//LOG_DEBUG("VIA_PCR read -> %02X", via_pcr);
		return via_pcr;

	case VIA_IFR:
		//LOG_DEBUG("VIA_IFR read -> %02X", via_ifr);
		return via_ifr;

	case VIA_IER:
		//LOG_DEBUG("VIA_IER read -> %02X", via_ier);
		return via_ier;

	case VIA_ANH: {
		const uint8_t composed = (uint8_t)((via_dra_in & ~via_ddra) | (via_dra_out & via_ddra));
		//LOG_DEBUG("VIA_ANH read: in=%02X out=%02X ddr=%02X -> %02X",	via_dra_in, via_dra_out, via_ddra, composed);
		return composed;
	}

	default:
		//LOG_DEBUG("IO READ: addr=%02X default -> 00", addr);
		return 0x00;
	}
}

void Pet2001IO::write(uint16_t a, uint8_t d8)
{
	const uint8_t addr = static_cast<uint8_t>(a & 0xFF);
	//LOG_DEBUG("IO WRITE: addr=%02X data=%02X", addr, d8);

	switch (addr) {
		// ---------------- PIA1 ----------------
	case PIA1_PA:
		if (pia1_cra & 0x04) {
			//LOG_DEBUG("PIA1_PA write: out %02X -> %02X", pia1_pa_out, d8);
			pia1_pa_out = d8;
			// Keyboard row select uses PA low nibble
			if ((pia1_pa_out & 0x0F) < 10) {
				const uint8_t row = (uint8_t)(pia1_pa_out & 0x0F);
				const uint8_t old_pb_in = pia1_pb_in;
				pia1_pb_in = keyrow[row];
				//LOG_DEBUG("PIA1 keyboard row=%u pb_in %02X -> %02X", row, old_pb_in, pia1_pb_in);
			}
		}
		else {
			//LOG_DEBUG("PIA1_DDRA write: %02X -> %02X", pia1_ddra, d8);
			pia1_ddra = d8;
		}
		break;

	case PIA1_CRA: {
		const uint8_t old = pia1_cra;
		pia1_cra = (uint8_t)((pia1_cra & 0xC0) | (d8 & 0x3F));
		//LOG_DEBUG("PIA1_CRA write: %02X -> %02X", old, pia1_cra);
		// CA2 change -> screen blank toggle (matches JS)
		const bool ca2_mode_is_output = ((pia1_cra & 0x38) == 0x38);
		if (ca2_mode_is_output && !pia1_ca2) {
			// CA2 rising -> Screen On, EOIout HIGH
			//LOG_DEBUG("PIA1_CA2 rising: Screen ON, EOIout=1");
			pia1_ca2 = 1;
			m_video.setVideoBlank(false);
			m_ieee.EOIout(true);
		}
		else if (((pia1_cra & 0x38) == 0x30) && pia1_ca2) {
			// CA2 low -> Screen Blank, EOIout LOW
			//LOG_DEBUG("PIA1_CA2 low: Screen BLANK, EOIout=0");
			pia1_ca2 = 0;
			m_video.setVideoBlank(true);
			m_ieee.EOIout(false);
		}
	} break;

	case PIA1_PB:
		if (pia1_crb & 0x04) {
			//LOG_DEBUG("PIA1_PB write: out %02X -> %02X", pia1_pb_out, d8);
			pia1_pb_out = d8;
		}
		else {
			//LOG_DEBUG("PIA1_DDRB write: %02X -> %02X", pia1_ddrb, d8);
			pia1_ddrb = d8;
		}
		break;

	case PIA1_CRB: {
		const uint8_t old = pia1_crb;
		pia1_crb = (uint8_t)((pia1_crb & 0xC0) | (d8 & 0x3F));
		//LOG_DEBUG("PIA1_CRB write: %02X -> %02X", old, pia1_crb);
	} break;

				 // ---------------- PIA2 ----------------
	case PIA2_PA:
		if (pia2_cra & 0x04) {
			//LOG_DEBUG("PIA2_PA write: out %02X -> %02X (PA does not drive DIO)", pia2_pa_out, d8);
			pia2_pa_out = d8;
		}
		else {
			//LOG_DEBUG("PIA2_DDRA write: %02X -> %02X (PA does not drive DIO)", pia2_ddra, d8);
			pia2_ddra = d8;
		}
		break;

	case PIA2_CRA: {
		const uint8_t old = pia2_cra;
		pia2_cra = (uint8_t)((pia2_cra & 0xC0) | (d8 & 0x3F));
		//LOG_DEBUG("PIA2_CRA write: %02X -> %02X", old, pia2_cra);
		// NDACout is controlled from CRA bit3 in the JS
		const bool ndac = (pia2_cra & 0x08) != 0;
		m_ieee.NDACout(ndac);
		//LOG_DEBUG("NDACout=%d", ndac ? 1 : 0);
	} break;

	case PIA2_PB:
		if (pia2_crb & 0x04) {
			//LOG_DEBUG("PIA2_PB write: out %02X -> %02X", pia2_pb_out, d8);
			pia2_pb_out = d8;

			// IMPORTANT: Always forward PB writes to IEEE so address/command bytes
			// under ATN and data-phase bytes (e.g., filename after OPEN) are seen.
			// The IEEE layer decides how to treat the byte based on ATN/state.
			m_ieee.DIOout(pia2_pb_out);

			// If you want extra tracing:
			//LOG_DEBUG("PIA2_PB -> IEEE DIOout=%02X (DDRB=%02X)", pia2_pb_out, pia2_ddrb);
		}
		else {
			//LOG_DEBUG("PIA2_DDRB write: %02X -> %02X", pia2_ddrb, d8);
			pia2_ddrb = d8;
		}
		break;

	case PIA2_CRB: {
		const uint8_t old = pia2_crb;
		pia2_crb = (uint8_t)((pia2_crb & 0xC0) | (d8 & 0x3F));
		//LOG_DEBUG("PIA2_CRB write: %02X -> %02X", old, pia2_crb);
		// DAVout is controlled from CRB bit3 in the JS
		const bool dav = (pia2_crb & 0x08) != 0;
		m_ieee.DAVout(dav);
		//LOG_DEBUG("DAVout=%d", dav ? 1 : 0);
	} break;

				 // ---------------- VIA ----------------
	case VIA_DRB: {
		// Clear CB2 (IFR3) if not independent; clear CB1 (IFR4)
		if ((via_pcr & 0xA0) != 0x20) {
			if (via_ifr & 0x08) {
				//LOG_DEBUG("VIA_DRB write: clearing IFR3 (CB2)");
				via_ifr &= (uint8_t)~0x08;
				if (via_ier & 0x08) updateIrq();
			}
		}
		if (via_ifr & 0x10) {
			//LOG_DEBUG("VIA_DRB write: clearing IFR4 (CB1)");
			via_ifr &= (uint8_t)~0x10;
			if (via_ier & 0x10) updateIrq();
		}

		//LOG_DEBUG("VIA_DRB write: out %02X -> %02X", via_drb_out, d8);
		via_drb_out = d8;

		// IEEE outputs: PB2 ATN, PB1 NRFD are driven only if configured output
		if (via_ddrb & 0x04) {
			const bool atn = (via_drb_out & 0x04) != 0;
			m_ieee.ATNout(atn);
			//LOG_DEBUG("ATNout=%d (VIA PB2)", atn ? 1 : 0);
		}
		else {
			//LOG_DEBUG("VIA PB2 (ATN) is input; not driving ATN");
		}

		if (via_ddrb & 0x02) {
			const bool nrfd = (via_drb_out & 0x02) != 0;
			m_ieee.NRFDout(nrfd);
			//LOG_DEBUG("NRFDout=%d (VIA PB1)", nrfd ? 1 : 0);
		}
		else {
			//LOG_DEBUG("VIA PB1 (NRFD) is input; not driving NRFD");
		}
	} break;

	case VIA_DRA: {
		// Clear CA2 (IFR0) if not independent; clear CA1 (IFR1)
		if ((via_pcr & 0x0A) != 0x02) {
			if (via_ifr & 0x01) {
				//LOG_DEBUG("VIA_DRA write: clearing IFR0 (CA2)");
				via_ifr &= (uint8_t)~0x01;
				if (via_ier & 0x01) updateIrq();
			}
		}
		if (via_ifr & 0x02) {
			//LOG_DEBUG("VIA_DRA write: clearing IFR1 (CA1)");
			via_ifr &= (uint8_t)~0x02;
			if (via_ier & 0x02) updateIrq();
		}
		//LOG_DEBUG("VIA_DRA write: out %02X -> %02X", via_dra_out, d8);
		via_dra_out = d8;
	} break;

	case VIA_DDRB:
		//LOG_DEBUG("VIA_DDRB write: %02X -> %02X", via_ddrb, d8);
		via_ddrb = d8;
		break;

	case VIA_DDRA:
		//LOG_DEBUG("VIA_DDRA write: %02X -> %02X", via_ddra, d8);
		via_ddra = d8;
		break;

	case VIA_T1CL:
		//LOG_DEBUG("VIA_T1CL write: T1LL %02X -> %02X", via_t1ll, d8);
		via_t1ll = d8; // latch
		break;

	case VIA_T1CH:
		if (via_ifr & 0x40) {
			//LOG_DEBUG("VIA_T1CH write: clearing IFR6 (T1)");
			via_ifr &= (uint8_t)~0x40;
			if (via_ier & 0x40) updateIrq();
		}
		//LOG_DEBUG("VIA_T1CH write: T1LH %02X -> %02X, T1CH %02X -> %02X; 1-shot=1; T1CL=%02X",via_t1lh, d8, via_t1ch, d8, via_t1ll);
		via_t1lh = d8;
		via_t1ch = d8;
		via_t1_1shot = 1;
		via_t1cl = via_t1ll;
		break;

	case VIA_T1LL:
		//LOG_DEBUG("VIA_T1LL write: %02X -> %02X", via_t1ll, d8);
		via_t1ll = d8;
		break;

	case VIA_T1LH:
		if (via_ifr & 0x40) {
			//LOG_DEBUG("VIA_T1LH write: clearing IFR6 (T1)");
			via_ifr &= (uint8_t)~0x40;
			if (via_ier & 0x40) updateIrq();
		}
		//LOG_DEBUG("VIA_T1LH write: %02X -> %02X", via_t1lh, d8);
		via_t1lh = d8;
		break;

	case VIA_T2CL:
		//LOG_DEBUG("VIA_T2CL write: T2LL %02X -> %02X", via_t2ll, d8);
		via_t2ll = d8; // latch
		break;

	case VIA_T2CH:
		if (via_ifr & 0x20) {
			//LOG_DEBUG("VIA_T2CH write: clearing IFR5 (T2)");
			via_ifr &= (uint8_t)~0x20;
			if (via_ier & 0x20) updateIrq();
		}
		if ((via_acr & 0x20) == 0) {
			//LOG_DEBUG("VIA_T2CH write: T2 one-shot set");
			via_t2_1shot = 1;
		}
		//LOG_DEBUG("VIA_T2CH write: T2CL %02X -> %02X, T2CH %02X -> %02X",  via_t2cl, via_t2ll, via_t2ch, d8);
		via_t2cl = via_t2ll;
		via_t2ch = d8;
		break;

	case VIA_SR:
		if (via_ifr & 0x04) {
			//LOG_DEBUG("VIA_SR write: clearing IFR2 (SR)");
			via_ifr &= (uint8_t)~0x04;
			if (via_ier & 0x04) updateIrq();
		}
		//LOG_DEBUG("VIA_SR write: %02X -> %02X", via_sr, d8);
		via_sr = d8;
		break;

	case VIA_ACR:
		//LOG_DEBUG("VIA_ACR write: %02X -> %02X", via_acr, d8);
		via_acr = d8;
		break;

	case VIA_PCR: {
		// CA2 output “toggle” (PCR bits 3..2 = 11 and bit1 flips) switches charset
		const bool old_is_toggle = ((via_pcr & 0x0C) == 0x0C);
		const bool new_is_toggle = ((d8 & 0x0C) == 0x0C);
		if (old_is_toggle && new_is_toggle && ((via_pcr ^ d8) & 0x02)) {
			//LOG_DEBUG("VIA_PCR write: charset toggle %d -> %d", (via_pcr & 0x02) ? 1 : 0, (d8 & 0x02) ? 1 : 0);
			m_video.setCharset((d8 & 0x02) != 0);
		}
		//LOG_DEBUG("VIA_PCR write: %02X -> %02X", via_pcr, d8);
		via_pcr = d8;
	} break;

	case VIA_IFR:
		// Clear flags by writing 1s to bits 0..6
		//LOG_DEBUG("VIA_IFR write: clear bits %02X (old IFR=%02X)", (uint8_t)(d8 & 0x7F), via_ifr);
		via_ifr &= (uint8_t)~(d8 & 0x7F);
		updateIrq();
		break;

	case VIA_IER:
		//LOG_DEBUG("VIA_IER write: %s bits %02X (old IER=%02X)",(d8 & 0x80) ? "enable" : "disable", (uint8_t)(d8 & 0x7F), via_ier);
		if (d8 & 0x80) via_ier |= (d8 & 0x7F);
		else           via_ier &= ~(d8 & 0x7F);
		updateIrq();
		break;

	case VIA_ANH:
		// DRA without handshake
		//LOG_DEBUG("VIA_ANH write: DRA (no handshake) %02X -> %02X", via_dra_out, d8);
		via_dra_out = d8;
		break;

	default:
		//LOG_DEBUG("IO WRITE: addr=%02X data=%02X default (no action)", addr, d8);
		break;
	}
}



void Pet2001IO::cycle()
{
	
	// Crude ~60Hz SYNC generator (exact values from the JS)
	if (++video_cycle == 5000) {
		sync(false);
	}
	else if (video_cycle == 16667) {
		sync(true);
		video_cycle = 0;
	}
	
	// This code commented out since it doesn't work and I did not get the change to go back and iterate it until it does.
	// Improved SYNC generator using DDA
    // Accurate SYNC generator using a fixed-point phase accumulator (per CPU cycle).
    // Phase advances at refresh_hz / cpu_hz. On wrap -> new period (sync true).
	// When phase crosses duty_thr_ within the period -> low (sync false).
	/*
	uint32_t prev = phase_;
	phase_ += inc_;

	if (phase_ < prev) {
		// Wrapped -> start of a new frame/field
		if (!sync_level_) { sync(true); sync_level_ = true; }
	}
	else if (prev < duty_thr_ && phase_ >= duty_thr_) {
		if (sync_level_) { sync(false); sync_level_ = false; }
	}
	*/
	// ---- VIA TIMER1 ----
	if ((via_t1cl--) == 0) {
		if ((via_t1ch--) == 0) {
			// Underflow
			if (via_acr & 0x40) {
				// Continuous: reload from latch
				via_t1cl = via_t1ll;
				via_t1ch = via_t1lh;
				via_t1_1shot = 1;
			}
			if (via_t1_1shot) {
				via_ifr |= 0x40; // IFR6
				if (via_ier & 0x40) updateIrq();
				via_t1_1shot = 0;
			}
		}
	}
	via_t1cl &= 0xFF;
	via_t1ch &= 0xFF;

	// ---- VIA TIMER2 ----
	if ((via_acr & 0x20) == 0) {
		if ((via_t2cl--) == 0) {
			if ((via_t2ch--) == 0) {
				if (via_t2_1shot) {
					via_ifr |= 0x20; // IFR5
					if (via_ier & 0x20) updateIrq();
					via_t2_1shot = 0;
				}
			}
		}
		via_t2cl &= 0xFF;
		via_t2ch &= 0xFF;
	}
}

void Pet2001IO::ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes)
{
	m_ieee.ieeeLoadData(addr, bytes);
}