// -----------------------------------------------------------------------------
// via6522.cpp
// Portable MOS 6522 VIA implementation.
// Modern C++17, cycle-accurate, matching via6522.h.
// -----------------------------------------------------------------------------

#include "via6522.h"
#include "sys_log.h"

/*
// at start of video/audio frame
via.cb2_reset_edge_log();

// ...run emulation for that frame...

uint32_t edgeCount = via.cb2_get_edge_count();
const CB2Edge* edges = via.cb2_get_edges();

// edges[0..edgeCount-1] is the complete edge list for this frame
*/

// -----------------------------------------------------------------------------
// Simplified PCR decode for CB2 (bits 7..5 of PCR)
// Based on actual 6522 behavior but kept much simpler than MAME.
// -----------------------------------------------------------------------------

// PCR bits: 7 6 5   (for CB2)
inline uint8_t cb2_mode(uint8_t pcr) { return (pcr >> 5) & 0x07; }

// Meaning:
// 000: Input, negative edge IRQ
// 001: Input, positive edge IRQ
// 010: Output, handshake (high until ORB access, then pulse low)
// 011: Output, pulse low (one-shot) manual
// 100: Output, pulse high (one-shot) manual
// 101: Reserved / behaves as pulse mode
// 110: Reserved / behaves as pulse mode
// 111: Output, constant high

// Returns true when CB2 is in "auto handshake / pulse" output modes
// (PCR bits 7..6 = 10, i.e. modes 4 and 5 in your cb2_mode()).
inline bool cb2_auto_handshake(uint8_t pcr)
{
	return (pcr & 0xC0) == 0x80;
}

// -----------------------------------------------------------------------------
// Optional debug logging controls
// -----------------------------------------------------------------------------
// Set these to true to enable very lightweight logging. Counters prevent
// runaway log spam on a 1 MHz machine.
static bool g_via_log_irq = true;      // log IRQ line transitions
static int  g_via_log_irq_max = 64;    // max lines to log

// -----------------------------------------------------------------------------
// CB2 debug logging
// When enabled, each CB2 output transition is logged once, with the current
// tick, mode, and basic VIA state. The counter prevents runaway logs.
// -----------------------------------------------------------------------------
static bool g_via_log_cb2 = true;     // set to true to enable CB2 edge logging
static int  g_via_log_cb2_max = 512;   // max CB2 edge lines to log

// -----------------------------------------------------------------------------
// Constructor / Reset
// -----------------------------------------------------------------------------

VIA6522::VIA6522()
{
	reset();
}

// -----------------------------------------------------------------------------
// reset
// Reset all VIA6522 internal state to power-on defaults.
// This includes timers, shift register, port latches, and CA2/CB2 outputs.
// -----------------------------------------------------------------------------
void VIA6522::reset()
{
	// Registers
	ora = 0;
	orb = 0;
	ddra = 0;
	ddrb = 0;
	acr = 0;
	pcr = 0;
	ifr = 0;
	ier = 0;
	sr = 0;

	// Timers
	t1_counter = 0;
	t1_latch = 0;
	t1_free_running = false;
	t1_enabled = false;
	t1_pb7_toggle = true;
	t1_pb7_output = false;

	t1_reload_pending = false;
	t2_reload_pending = false;
	t2_oneshot_fired = false;

	t2_counter = 0;
	t2_latch = 0;
	t2_enabled = false;
	t2_counts_external = false;

	// Shift register state
	sr_latch = 0;
	sr_bits_remaining = 0;
	sr_bits_left = 0;
	sr_mode = 0;
	sr_clock_external = false;
	sr_shift_out = false;
	sr_t2_phase = false;
	sr_reload_pending = false;

	// Inputs
	portA_in = 0xFF;
	portB_in = 0xFF;

	// CA1/CB1
	ca1_in = false;
	old_ca1 = false;
	cb1_in = false;
	old_cb1 = false;

	// CA2/CB2
	ca2_in = false;
	cb2_in = false;
	ca2_out = true;
	cb2_out = true;
	old_ca2 = true;
	old_cb2 = true;
	ca2_pulse_count = 0;
	cb2_pulse_count = 0;

	// CB2 output control helpers
	cb2_out_state = false;
	cb2_is_output = false;
	cb2_shift_override = false;
	last_cb2_out = cb2_out;

	// CB2 audio edge log
	cb2_edge_count = 0;
	cb2_tick_counter = 0;
	cb2_last_logged = cb2_out;
}


// -----------------------------------------------------------------------------
// logCB2Transition
// Helper for logging CB2 level changes. This is debug-only now and does
// not touch the CB2 audio edge log. Edge logging is handled only by
// logCB2Edge() so cb2_edge_count always matches the entries in cb2_edges[].
// -----------------------------------------------------------------------------
void VIA6522::logCB2Transition()
{
	if (cb2_out != last_cb2_out)
	{
		//LOG_DEBUG("VIA6522: CB2 %d -> %d", last_cb2_out ? 1 : 0,cb2_out ? 1 : 0);
		// Debug transition tracking only
		last_cb2_out = cb2_out;
		// NOTE: do not modify cb2_last_logged or cb2_edge_count here.
	}
}

// -----------------------------------------------------------------------------
// driveCB2
// Drive CB2 output. In shift-register output modes (sr_shift_out &&
// cb2_shift_override), CB2 is ALWAYS driven regardless of PCR CB2 direction.
// -----------------------------------------------------------------------------
void VIA6522::driveCB2(bool level)
{
	// Shift-register output overrides PCR direction.
	bool force_shift = (sr_shift_out && cb2_shift_override);

	// If not in shift mode, honor PCR direction.
	if (!force_shift)
	{
		if (!cb2_is_output)
			return;
	}

	bool prev = cb2_out;
	cb2_out = level;

	if (prev != level)
	{
		//LOG_DEBUG("VIA6522: CB2 %d -> %d (shift_override=%d, cb2_is_output=%d)", prev ? 1 : 0, level ? 1 : 0, force_shift ? 1 : 0,cb2_is_output ? 1 : 0);

		// Record this transition in the CB2 audio edge log.
		// This covers all SR-driven audio edges (PHI2 and T2 modes)
		// and any other place that calls driveCB2().
		logCB2Edge();
	}
}

// -----------------------------------------------------------------------------
// Port / pin wiring
// -----------------------------------------------------------------------------

void VIA6522::setPortAInput(uint8_t value)
{
	portA_in = value;
}

void VIA6522::setPortBInput(uint8_t value)
{
	portB_in = value;
}

uint8_t VIA6522::getPortAOutput() const
{
	return ora;
}

uint8_t VIA6522::getPortBOutput() const
{
	return orb;
}

void VIA6522::setCA1(bool level)
{
	ca1_in = level;
}

void VIA6522::setCB1(bool level)
{
	cb1_in = level;
}

void VIA6522::setCA2(bool level)
{
	ca2_in = level;
}

void VIA6522::setCB2(bool level)
{
	cb2_in = level;
}

bool VIA6522::getCA2Output() const
{
	return ca2_out;
}

bool VIA6522::getCB2Output() const
{
	return cb2_out;
}

// -----------------------------------------------------------------------------
// Register Access
// -----------------------------------------------------------------------------

uint8_t VIA6522::readPortA()
{
	uint8_t out = (ora & ddra) | (portA_in & ~ddra);

	// Reading ORA clears IFR bit 1 (CA1)
	if (ifr & 0x02) {
		ifr &= ~0x02;
		updateIFR();
	}
	return out;
}

uint8_t VIA6522::readPortB()
{
	// Base pins: outputs from ORB where DDRB=1, inputs from external world where DDRB=0
	uint8_t pins = (orb & ddrb) | (portB_in & ~ddrb);

	// If PB7 is under Timer 1 control (ACR7=1) and configured as output (DDRB7=1),
	// override pin 7 with the internal PB7 timer output state.
	if ((acr & 0x80) && (ddrb & 0x80))
	{
		if (t1_pb7_output)
			pins |= 0x80;
		else
			pins &= ~0x80;
	}

	//	LOG_DEBUG("VIA6522 readPortB: ORB=%02X DDRB=%02X IN=%02X PB7=%d -> %02X", orb, ddrb, portB_in, (int)t1_pb7_output, pins);
	return pins;
}
uint8_t VIA6522::readReg(uint8_t offset)
{
	offset &= 0x0F;

	switch (offset)
	{
	case 0x00: // ORB / IRB
		// Return the composite PB pins (outputs + inputs + PB7 override)
		return readPortB();

	case 0x01: // ORA / IRA
		// Return the composite PA pins and clear CA1 flag
		return readPortA();

	case 0x0F: // ORA duplicate
		// Mirror of ORA
		return readPortA();

	case 0x02: return ddra;
	case 0x03: return ddrb;

	case 0x04: // T1 low counter
	{
		// Reading T1C-L returns the low byte of the counter and
		// clears the T1 interrupt flag (IFR bit 6) on a real 6522.
		uint8_t val = static_cast<uint8_t>(t1_counter & 0x00FF);

		// Clear IFR6 (T1) on read.
		if (ifr & 0x40)
		{
			ifr &= static_cast<uint8_t>(~0x40);
			updateIFR();
		}

		return val;
	}

	case 0x05: // T1 high counter
		// High byte of the current counter; does NOT affect IFR.
		return static_cast<uint8_t>((t1_counter >> 8) & 0x00FF);

	case 0x06: // T1 low latch
		// Readback of the latch, not the counter; no IFR changes.
		return static_cast<uint8_t>(t1_latch & 0x00FF);

	case 0x07: // T1 high latch
		// Readback of the latch, not the counter; no IFR changes.
		return static_cast<uint8_t>((t1_latch >> 8) & 0x00FF);

	case 0x08: // T2C-L read
	{
		uint8_t val = static_cast<uint8_t>(t2_counter & 0x00FF);

		// Real 6522: reading T2C-L clears IFR5 (Timer 2 interrupt flag).
		if (ifr & 0x20) {
			ifr &= (uint8_t)~0x20;
			updateIFR();
		}

		return val;
	}

	case 0x09: // T2 high counter
		return static_cast<uint8_t>((t2_counter >> 8) & 0xFF);

	case 0x0A: // SR
		// Reading SR clears IFR bit 2
		if (ifr & 0x04) {
			ifr &= ~0x04;
			updateIFR();
		}
		return sr;

	case 0x0B: // ACR
		return acr;

	case 0x0C: // PCR
		return pcr;

	case 0x0D: // IFR
		return ifr;

	case 0x0E: // IER
		return ier | 0x80;

	case 0x10: // IRB (not mirrored on PET but needed for accuracy)
		return readPortB();

	default:
		return 0xFF;
	}
}

void VIA6522::writeReg(uint8_t offset, uint8_t data)
{
	offset &= 0x0F;

	switch (offset)
	{
	case 0x00: // ORB
	{
		uint8_t old = orb;
		(void)old;
		orb = data;

		// Update output pins based on DDRB.
		updatePBOutput();

		// Handle CB2 handshake/pulse behavior for ORB write.
		handleCB2WriteSideEffects();
	}
	break;

	case 0x01: // ORA
	case 0x0F: // duplicate ORA
	{
		uint8_t old_ora = ora;
		ora = data;

		LOG_DEBUG(
			"VIA6522: ORA write %02X (old=%02X, PCR=%02X, CA2mode=%u)",
			(unsigned)ora,
			(unsigned)old_ora,
			(unsigned)pcr,
			(unsigned)((pcr >> 1) & 0x07)
		);

		handleCA2WriteSideEffects();
		break;
	}

	case 0x02: // DDRA
	{
		uint8_t old = ddra;
		(void)old;
		ddra = data;
		break;
	}

	case 0x03: // DDRB
	{
		uint8_t old = ddrb;
		(void)old;
		ddrb = data;

		// Changing direction affects visible pins.
		updatePBOutput();
		break;
	}

	case 0x04: // T1 Low latch
		t1_latch = (uint16_t)((t1_latch & 0xFF00) | data);
		break;

	case 0x05: // T1 High latch (reload)
	{
		t1_latch = (uint16_t)((t1_latch & 0x00FF) | (uint16_t)(data << 8));

		// Transfer latch into counter.
		t1_counter = t1_latch;

		// Clear IFR6 (T1 interrupt).
		if (ifr & 0x40) {
			ifr &= (uint8_t)~0x40;
			updateIFR();
		}

		// Arm Timer 1.
		t1_enabled = true;
		t1_free_running = (acr & 0x40) != 0;
		break;
	}

	case 0x06: // T1LL
		t1_latch = (uint16_t)((t1_latch & 0xFF00) | data);
		break;

	case 0x07: // T1LH
		t1_latch = (uint16_t)((t1_latch & 0x00FF) | (uint16_t)(data << 8));
		break;

	case 0x08: // T2LL
	{
		t2_latch = (uint16_t)((t2_latch & 0xFF00) | data);

		// Auto-start T2 hack for PET audio compatibility if needed.
		// (Specifically for SR-driven sound where T2CH might not be written by some routines)
		if (!t2_enabled)
		{
			uint8_t sr_mode_local = (uint8_t)((acr >> 2) & 0x07);
			bool t2_sr_mode = (sr_mode_local == 0x01) || (sr_mode_local == 0x04) || (sr_mode_local == 0x05);
			if (t2_sr_mode)
			{
				// SR-under-T2: 8-bit timer, reload LOW latch byte only.
				t2_counter = (uint16_t)(t2_latch & 0x00FF);
				t2_reload_pending = false;
				sr_t2_phase = false;      // start a new note in a known phase
				t2_enabled = true;
				if (ifr & 0x20) {
					ifr &= (uint8_t)~0x20;
					updateIFR();
				}
			}
		}
		break;
	}

	case 0x09: // T2CH
	{
		t2_latch = (uint16_t)((t2_latch & 0x00FF) | (uint16_t)(data << 8));

		// Reload counter.
		t2_counter = t2_latch;

		// Enable T2.
		t2_enabled = true;
		t2_oneshot_fired = false;   // a fresh T2CH write re-arms the one-shot
		t2_reload_pending = false;

		// Clear IFR5 immediately.
		if (ifr & 0x20) {
			ifr &= (uint8_t)~0x20;
			updateIFR();
		}

		LOG_DEBUG("VIA6522: T2CH write T2_ENABLED:%02X T2:%x", t2_enabled, data);
		break;
	}

	case 0x0A: // SR Write
	{
		// Update latch and clear the SR interrupt flag (IFR2).
		sr_latch = data;
		if (ifr & 0x04) {
			ifr &= (uint8_t)~0x04;
			updateIFR();
		}

		if (sr_mode == 0x04 || sr_mode == 0x05)
		{
			// T2-clocked shift-OUT modes: writing SR (re)starts the shift
			// sequence (peskytone: "reload shift register to start shifting").
			// Make the restart fully deterministic so a note resumes cleanly even
			// if T2 wandered and the bit counters froze while SR was disabled
			// during a Method-A / PCR low note:
			//   - load the new pattern and re-arm both bit counters,
			//   - restart the T2 shift clock from the LOW latch byte,
			//   - reset the divide-by-2 phase,
			//   - present the MSB on CB2 immediately (SR bit7 -> CB2).
			// Sustained notes do not rewrite SR (the player only writes it on a
			// note/waveform change), so this does not disturb a held tone.
			sr = sr_latch;
			sr_reload_pending = false;
			sr_bits_left = 8;
			sr_bits_remaining = 8;
			sr_t2_phase = false;

			t2_counter = (uint16_t)(t2_latch & 0x00FF);
			t2_reload_pending = false;
			t2_enabled = true;

			if (cb2_shift_override && sr_shift_out)
				driveCB2((sr & 0x80) != 0);
		}
		else
		{
			// Input / PHI2-controlled modes: latch the byte and apply at the next
			// byte boundary, unless the SR is idle on both clock paths.
			sr_reload_pending = true;
			if (sr_bits_left == 0 && sr_bits_remaining == 0)
			{
				sr = sr_latch;
				sr_reload_pending = false;
				sr_bits_left = 8;
				sr_bits_remaining = 8;
			}
		}

		break;
	}

	case 0x0B: // ACR
	{
		uint8_t old_mode = sr_mode;

		acr = data;

		// SR mode from ACR bits 4..2
		uint8_t mode = (uint8_t)((acr >> 2) & 0x07);
		sr_mode = mode;

		// In your model, SR output overrides CB2 when mode >= 4.
		cb2_shift_override = (mode >= 4);
		sr_shift_out = (mode >= 4);

		// If SR mode changes, reset the T2 phase so the divide-by-2 is stable.
		if (old_mode != sr_mode)
			sr_t2_phase = false;

		LOG_INFO("VIA: ACR=%02X SRmode=%u T2latch=%04X", (unsigned)acr, (unsigned)sr_mode, (unsigned)t2_latch);
		break;
	}

	case 0x0C: // PCR
	{
		pcr = data;

		// CB2 mode is PCR bits 7..5 (NOT 5..3).
		uint8_t cb2_mode = (uint8_t)((pcr >> 5) & 0x07);
		cb2_is_output = (cb2_mode >= 4);

		// Force logic update immediately. Critical for Manual PCR sound toggling.
		handleCB2Logic();
		break;
	}

	case 0x0D: // IFR
	{
		// Writing a 1 bit into IFR clears that bit (except bit 7).
		uint8_t mask = data & 0x7F;
		uint8_t before = ifr;
		ifr &= (uint8_t)~mask;
		LOG_DEBUG("VIA6522: IFR write: %02X -> %02X after mask %02X", before, ifr, mask);
		// CRITICAL: Recalculate IFR7 immediately so IRQ line reflects the change
		updateIFR();
		return;
	}

	case 0x0E: // IER
	{
		if (data & 0x80)
			ier |= (data & 0x7F);
		else
			ier &= (uint8_t)~(data & 0x7F);
		// CRITICAL: Recalculate IFR7 since enabled interrupts changed
		updateIFR();
		break;
	}

	default:
		break;
	}
}


// -----------------------------------------------------------------------------
// Tick / IRQ
// -----------------------------------------------------------------------------

void VIA6522::tick()
{
	// Capture previous IRQ state before advancing timers/shift register.
	const bool prev_irq = (ifr & 0x80) != 0;

	// Handle edge detection for CA1/CB1 each cycle
	handleCA1Edge();
	handleCB1Edge();

	// Update timers
	runTimer1();
	runTimer2();

	// Update SR
	runShiftRegister();

	// Handle CA2/CB2 logic
	handleCA2Logic();
	handleCB2Logic();

	// PB7 output from Timer 1 (if enabled)
	handlePB7Output();

	// Update IFR summary bit based on individual flags and IER
	updateIFR();

	// ---------------------------------------------------------------------
	// CB2 audio logging
	// ---------------------------------------------------------------------
	// Each VIA tick represents one time step in the CB2 edge log.
	// We advance the tick counter and record an edge whenever the CB2
	// output changes. The log is capped at CB2_EDGE_MAX entries.
	++cb2_tick_counter;

	// ---------------------------------------------------------------------
	// Optional IRQ transition logging
	// ---------------------------------------------------------------------
	const bool curr_irq = (ifr & 0x80) != 0;
	if (g_via_log_irq && (curr_irq != prev_irq) && g_via_log_irq_max > 0)
	{
		LOG_INFO("[VIA6522] IRQ %s (IFR=%02X IER=%02X ACR=%02X PCR=%02X)",
			curr_irq ? "ASSERT" : "CLEAR",
			static_cast<unsigned>(ifr),
			static_cast<unsigned>(ier),
			static_cast<unsigned>(acr),
			static_cast<unsigned>(pcr));
		--g_via_log_irq_max;
	}
}

bool VIA6522::getIRQ() const
{
	// IFR7 is set when any enabled IFR bit is set.
	return (ifr & 0x80) != 0;
}

// -----------------------------------------------------------------------------
// CA1/CB1 edge handling
// -----------------------------------------------------------------------------

void VIA6522::handleCA1Edge()
{
	bool rising = (!old_ca1 && ca1_in);
	bool falling = (old_ca1 && !ca1_in);

	bool trigger = false;
	if ((pcr & 0x01) == 0) { // negative edge
		trigger = falling;
	}
	else {
		trigger = rising;
	}

	if (trigger) {
		ifr |= 0x02; // IFR1
	}

	old_ca1 = ca1_in;
}

void VIA6522::handleCB1Edge()
{
	bool rising = (!old_cb1 && cb1_in);
	bool falling = (old_cb1 && !cb1_in);

	bool trigger = false;
	if ((pcr & 0x10) == 0) { // negative edge
		trigger = falling;
	}
	else {
		trigger = rising;
	}

	if (trigger)
	{
		// MAME: via_set_int(which, INT_CB1)
		ifr |= 0x10; // IFR4

		// MAME CB2_AUTO_HS: CB1 active edge drives CB2 back HIGH.
		if (cb2_auto_handshake(pcr))
		{
			if (!cb2_out)
			{
				cb2_out = true;

				// Keep your logging and edge buffer in sync.
				logCB2Transition();
				logCB2Edge();
			}
		}
	}

	old_cb1 = cb1_in;
}

// -----------------------------------------------------------------------------
// Timers
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// runTimer1
// Advance Timer 1 by one PHI2 cycle.
// On underflow, sets IFR bit 6 and handles one-shot vs free-run and PB7 output.
// Also logs the underflow and PB7 state changes.
// -----------------------------------------------------------------------------
void VIA6522::runTimer1()
{
	if (!t1_enabled)
		return;

	if (t1_reload_pending)
	{
		// One-cycle reload phase after underflow -> total period N+2.
		t1_counter = t1_latch;
		t1_reload_pending = false;
		return;
	}

	if (t1_counter == 0x0000)
	{
		// Underflow: set IFR6
		ifr |= 0x40;

		if (acr & 0x40) // free-run
		{
			// Pass through 0xFFFF for one cycle, then reload from latch next cycle.
			t1_counter = 0xFFFF;
			t1_reload_pending = true;
			t1_pb7_toggle = !t1_pb7_toggle;
		}
		else
		{
			// One-shot: fire once, keep counting from 0xFFFF, force PB7 high.
			t1_counter = 0xFFFF;
			t1_enabled = false;
			t1_pb7_toggle = true;
		}
	}
	else
	{
		--t1_counter;
	}
}

void VIA6522::runTimer2()
{
	// Honor the enable flag. When disabled, T2 does not count or underflow.
	if (!t2_enabled)
		return;

	const bool sr_t2_mode = (sr_mode == 0x01 || sr_mode == 0x04 || sr_mode == 0x05);

	if (t2_reload_pending)
	{
		// One-cycle reload phase after underflow -> base period N+2.
		// In SR-under-T2 modes, T2 is an 8-bit timer: reload LOW latch byte only.
		t2_counter = (uint16_t)(t2_latch & 0x00FF);
		t2_reload_pending = false;
		return;
	}

	if (t2_counter == 0x0000)
	{
		if (sr_t2_mode)
		{
			// T2-driven SR modes:
			//   001: shift IN under T2 control
			//   100: shift OUT under T2 rate      (PET CB2 sound)
			//   101: shift OUT under T2 control
			ifr |= 0x20;             // IFR5 set (Timer 2)
			t2_counter = 0xFFFF;
			t2_reload_pending = true;

			// Divide-by-2: a CB2 shift happens every 2nd T2 timeout. This is real
			// 6522 SR-under-T2 behavior (bit period = 2*(T2L+2)); the Faulty Robots
			// note table depends on it. MAME 0.228 omits it and plays an octave high.
			sr_t2_phase = !sr_t2_phase;

			if (sr_t2_phase)
			{
				runShiftRegister_T2();

				// Byte-boundary handling.
				if (sr_bits_left == 0)
				{
					// Free-run mode 4 recirculates the pattern CONTINUOUSLY: the SR
					// counter is disabled (no IFR2) and there is NO byte-boundary
					// action - CB2 is purely the recirculating bits driven per-bit by
					// runShiftRegister_T2(). Driving CB2 high here injected a phantom
					// pulse once per byte, which turned an all-zero SR (a silent state,
					// e.g. pet-invaders' "sound off") into an audible ~243 Hz tone.
					// Only the controlled mode (5) flags IFR2 / idles CB2 high.
					if (sr_mode != 0x04)
					{
						ifr |= 0x04; // IFR2 (SR) - controlled mode only
						if (cb2_shift_override && sr_mode == 0x05)
							driveCB2(true);
					}
				}
			}
		}
		else
		{
			// Plain timed T2: one-shot. Set IFR5 once; keep counting from 0xFFFF
			// without reloading from latch (matches real 6522).
			if (!t2_oneshot_fired)
			{
				ifr |= 0x20;
				t2_oneshot_fired = true;
			}
			t2_counter = 0xFFFF;
		}
	}
	else
	{
		--t2_counter;
	}
}

void VIA6522::runShiftRegister_T2()
{
	// Mode 1 (001): Shift IN using T2
	// Mode 4 (100): Shift OUT using T2 (free run) - Used for PET Audio
	// Mode 5 (101): Shift OUT using T2 (controlled)
	if (sr_mode != 0x01 && sr_mode != 0x04 && sr_mode != 0x05)
		return;

	// Byte-boundary handling
	if (sr_bits_left == 0)
	{
		if (sr_mode == 0x04)
		{
			// Free-run audio mode:
			// Keep running continuously. If CPU wrote a new SR value, apply it
			// cleanly at the byte boundary.
			if (sr_reload_pending)
			{
				sr = sr_latch;
				sr_reload_pending = false;
			}

			sr_bits_left = 8;
		}
		else
		{
			// Controlled mode (5) and input mode (1):
			// Only begin a new byte if CPU wrote SR since the last completion.
			if (!sr_reload_pending)
				return;

			sr = sr_latch;
			sr_reload_pending = false;
			sr_bits_left = 8;
		}
	}

	// Shift one bit
	if (sr_mode == 0x01)
	{
		// INPUT MODE (001): shift in CB2 pin level into LSB after shifting left.
		uint8_t in_bit = cb2_in ? 0x01 : 0x00;
		sr = (uint8_t)((sr << 1) | in_bit);
	}
	else
	{
		// OUTPUT MODES (100, 101):
		// Output current MSB to CB2 before shifting.
		bool out_bit = (sr & 0x80) != 0;

		if (cb2_shift_override)
			driveCB2(out_bit);

		if (sr_mode == 0x04)
		{
			// Free-run: recirculate MSB into LSB (rotate-left) to preserve waveform.
			uint8_t msb = (sr & 0x80) ? 1u : 0u;
			sr = (uint8_t)((sr << 1) | msb);
		}
		else
		{
			// Controlled: shift in zeros.
			sr = (uint8_t)(sr << 1);
		}
	}

	// Decrement bit counter. Caller checks for completion.
	if (sr_bits_left > 0)
		--sr_bits_left;
}


// -----------------------------------------------------------------------------
// runShiftRegister
// PHI2-driven SR clock (modes 010, 110).
// T2-driven modes are clocked from runTimer2() via runShiftRegister_T2().
// -----------------------------------------------------------------------------
void VIA6522::runShiftRegister()
{
	// SR mode from ACR bits 4..2
	uint8_t mode = (uint8_t)((acr >> 2) & 0x07);

	// Nothing to do if SR disabled or not armed
	if (mode == 0 || sr_bits_remaining == 0)
		return;

	bool clock_now = false;

	switch (mode)
	{
		// T2-driven modes: we do not clock them here.
		// 001: shift in under T2 control
		// 100: shift out under T2 rate
		// 101: shift out under T2 control
	case 0b001:
	case 0b100:
	case 0b101:
		return;

		// PHI2-driven modes:
		// 010: shift in under PHI2
		// 110: shift out under PHI2
	case 0b010:
	case 0b110:
		clock_now = true;
		break;

		// External CB1 clock modes (not handled here)
		// 011: shift in under external CB1
		// 111: shift out under external CB1
	case 0b011:
	case 0b111:
	default:
		return;
	}

	if (!clock_now)
		return;

	if (sr_shift_out)
	{
		bool outbit = (sr & 0x80) != 0;
		if (cb2_shift_override)
			driveCB2(outbit);

		// For mode 6 (PHI2 shift-out), we could add recirculation here
		// if needed for continuous audio. But mode 6 is "controlled" mode
		// per datasheet, so technically zeros should shift in.
		// Uncomment below for free-run behavior:
		// uint8_t msb = outbit ? 1u : 0u;
		// sr = (uint8_t)((sr << 1) | msb);

		sr = (uint8_t)(sr << 1);  // Standard controlled mode
	}
	else
	{
		// INPUT: CB2 is serial input
		uint8_t inbit = cb2_in ? 1u : 0u;
		sr = (uint8_t)((sr << 1) | inbit);
	}

	if (sr_bits_remaining > 0)
		--sr_bits_remaining;

	if (sr_bits_remaining == 0)
	{
		// Completed an 8-bit transfer, set SR interrupt flag (IFR2).
		ifr |= 0x04;

		// In shift-out modes, CB2 returns high after the byte.
		if (cb2_shift_override)
			driveCB2(true);

		LOG_DEBUG("VIA SR complete (PHI2). mode=%d sr=%02X", mode, sr);
	}
}

// -----------------------------------------------------------------------------
// CA2/CB2 / PB7 helpers
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// handleCA2Logic
// Implements CA2 behavior per PCR bits 5:3.
//
// Modes (PCR[5:3]):
//   000: Input, negative edge, handshake
//   001: Input, positive edge, handshake
//   010: Output, manual low
//   011: Output, manual high
//   100: Output, handshake (usually ORA accesses)
//   101: Output, pulse low       (not fully timed yet)
//   110: Input, negative edge, independent interrupt
//   111: Input, positive edge, independent interrupt
//
// Notes:
// - IFR bit for CA2 is bit 0 (0x01).
// - Handshake modes: IFR0 is normally cleared by ORA accesses in the PET
//   glue code or by writing IFR directly.
// - Independent-interrupt modes: IFR0 should only be cleared by writes to IFR.
// -----------------------------------------------------------------------------
void VIA6522::handleCA2Logic()
{
	// CA2 mode is encoded in PCR bits 3..1
	uint8_t mode = (pcr >> 1) & 0x07;

	bool prev_out = ca2_out;

	bool prev = old_ca2;
	bool curr = ca2_in;

	bool neg_edge = (prev && !curr);     // 1 -> 0
	bool pos_edge = (!prev && curr);     // 0 -> 1

	switch (mode)
	{
	case 0: // Input, negative edge, handshake
		if (neg_edge) {
			ifr |= 0x01; // IFR0 (CA2)
		}
		break;

	case 1: // Input, positive edge, handshake
		if (pos_edge) {
			ifr |= 0x01;
		}
		break;

	case 2: // Output, manual low
		ca2_out = false;
		break;

	case 3: // Output, manual high
		ca2_out = true;
		break;

	case 4: // Output, handshake (DAV on PET)
	case 5: // Output, pulse low (one-shot)
		// Pulse timing handled by updateCA2State()
		updateCA2State(mode);
		break;

	case 6: // Input, negative edge, independent interrupt
		if (neg_edge) {
			ifr |= 0x01;
		}
		// Clearing only via IFR write.
		break;

	case 7: // Input, positive edge, independent interrupt
		if (pos_edge) {
			ifr |= 0x01;
		}
		// Clearing only via IFR write.
		break;
	}

	if (ca2_out != prev_out)
	{
		LOG_DEBUG(
			"VIA6522: CA2 output change -> %d (mode=%u, PCR=%02X, pulse=%u)",
			ca2_out ? 1 : 0,
			(unsigned)mode,
			(unsigned)pcr,
			(unsigned)ca2_pulse_count
		);
	}

	old_ca2 = ca2_in;
}

// -----------------------------------------------------------------------------
// handleCB2Logic
// Accurate CB2 mode behavior based on PCR bits 7..5 (cb2_mode):
//
//   Input modes (bit 7 = 0, cb2_mode 0..3):
//     000 (0): handshake input, negative edge IRQ
//     010 (2): handshake input, positive edge IRQ
//     001 (1): independent IRQ input, negative edge
//     011 (3): independent IRQ input, positive edge
//
//   Output modes (bit 7 = 1, cb2_mode 4..7):
//     100 (4): handshake output (idle high, ORB write triggers low pulse)
//     101 (5): pulse output (manual pulse)
//     110 (6): fixed output LOW
//     111 (7): fixed output HIGH
//
// IFR bit 3 is the CB2 flag.
// In independent-IRQ input modes, IFR3 should only be cleared by writing IFR.
// This function only sets IFR3; clearing is handled elsewhere.
// -----------------------------------------------------------------------------

void VIA6522::handleCB2Logic()
{
	const uint8_t mode = cb2_mode(pcr);
	const bool   is_output = (pcr & 0x80) != 0;

	// -------------------------------------------------------------
	// SR-driven CB2 detection:
	// - sr_shift_out = true for modes 100..111
	// - sr_bits_remaining > 0 while a transfer is active
	// - ((acr >> 2) & 0x04) != 0 means mode 100..111 (shift OUT)
	//
	// When this is true, CB2 is being driven by the shift register
	// (either PHI2 or T2 driven). In that case we MUST NOT override
	// cb2_out here with handshake / manual output logic, or we will
	// destroy the audio waveform.
	// -------------------------------------------------------------
	const uint8_t sr_mode_from_acr = (acr >> 2) & 0x07;

	// SR is considered "actively driving" CB2 if:
	//  - it is a shift-out mode (100..111), and
	//  - either PHI2 or T2 bit counters are non-zero.
	const bool sr_active_bits =
		(sr_bits_remaining > 0) || (sr_bits_left > 0);

	const bool sr_driving_cb2 =
		sr_shift_out &&
		sr_active_bits &&
		((sr_mode_from_acr & 0x04) != 0);   // any shift-out mode (4..7)

	bool prev_out = cb2_out;

	// -------------------------------------------------------------
	// 1) If SR is actively driving CB2, do NOT touch or log CB2 here.
	//    runShiftRegister()/runShiftRegister_T2()/driveCB2() have
	//    already updated cb2_out and logged edges.
	// -------------------------------------------------------------
	if (sr_driving_cb2)
	{
		return;
	}
	// -------------------------------------------------------------
	// 2) Regular CB2 input modes (bit7 = 0 in PCR)
	// -------------------------------------------------------------
	else if (!is_output)
	{
		// ---------------------------------------------------------------------
		// Input modes: mirror cb2_in to cb2_out and detect edges on the input.
		// cb2_mode values (0..3) when bit7 = 0:
		//   0: handshake, negative edge IRQ
		//   2: handshake, positive edge IRQ
		//   1: independent IRQ, negative edge
		//   3: independent IRQ, positive edge
		// ---------------------------------------------------------------------
		bool prev_level = cb2_out;
		cb2_out = cb2_in;

		bool rising = (!prev_level && cb2_out);
		bool falling = (prev_level && !cb2_out);

		switch (mode)
		{
		case 0: // 000: handshake input, negative edge IRQ
			if (falling)
				ifr |= 0x08; // IFR3
			break;

		case 2: // 010: handshake input, positive edge IRQ
			if (rising)
				ifr |= 0x08;
			break;

		case 1: // 001: independent IRQ input, negative edge
			if (falling)
				ifr |= 0x08;
			// Clearing of IFR3 is by explicit IFR write.
			break;

		case 3: // 011: independent IRQ input, positive edge
			if (rising)
				ifr |= 0x08;
			// Clearing of IFR3 is by explicit IFR write.
			break;

		default:
			// Should not happen for bit7=0, but keep cb2_out mirrored.
			break;
		}
	}
	// -------------------------------------------------------------
	// 3) Regular CB2 output modes (bit7 = 1 in PCR) when SR is *not*
	//    currently driving CB2.
	// -------------------------------------------------------------
	else
	{
		// Output modes (bit7 = 1, cb2_mode 4..7)
		switch (mode)
		{
		case 4: // handshake output (AUTO_HS)
		case 5: // pulse output (AUTO_HS)
			// In AUTO_HS modes, CB2 is driven directly by ORB writes and CB1 edges.
			// We emulate this via cb2_pulse_count and updateCB2State().
			updateCB2State(mode);
			logCB2Transition();
			break;

		case 6: // fixed low
			cb2_out = false;
			logCB2Transition();
			break;

		case 7: // fixed high
			cb2_out = true;
			logCB2Transition();
			break;

		default:
			// Should not happen for bit7=1, but do nothing.
			break;
		}
	}

	// -------------------------------------------------------------
	// CB2 edge logging (unchanged): any change in cb2_out from
	// either SR or PCR logic will be logged here.
	// -------------------------------------------------------------
	if (cb2_out != prev_out)
	{
		if (g_via_log_cb2 && (g_via_log_cb2_max > 0))
		{
			LOG_DEBUG(
				"VIA6522 CB2 EDGE: tick=%u mode=%u output=%d PCR=%02X IFR=%02X ORB=%02X",
				(unsigned)cb2_tick_counter,
				(unsigned)mode,
				cb2_out ? 1 : 0,
				(unsigned)pcr,
				(unsigned)ifr,
				(unsigned)orb
			);
			--g_via_log_cb2_max;
		}

		logCB2Edge();
	}
}

void VIA6522::handlePB7Output()
{
	if (acr & 0x80)
		t1_pb7_output = t1_pb7_toggle;
}

// -----------------------------------------------------------------------------
// T2 external pulses
// -----------------------------------------------------------------------------

void VIA6522::externalT2Pulse()
{
	if (!t2_enabled || !t2_counts_external)
		return;

	if (t2_counter == 0x0000)
	{
		ifr |= 0x20; // IFR5
		t2_enabled = false;
	}
	else
	{
		--t2_counter;
	}
}

// -----------------------------------------------------------------------------
// IFR update
// -----------------------------------------------------------------------------

void VIA6522::updateIFR()
{
	uint8_t flags = ifr & 0x7F;
	if (flags & ier)
		ifr |= 0x80;
	else
		ifr &= ~0x80;
}

// -----------------------------------------------------------------------------
// updatePBOutput
// Recompute the effective PB pin levels from the ORB latch, DDRB, and
// current external PB input (portB_in), and log the result.
//
// Notes:
// - orb       = output latch
// - ddrb      = data direction register for port B
// - portB_in  = external input level on PB pins
//
// For each bit:
//   if DDRB bit = 1 -> pin is driven by ORB bit
//   if DDRB bit = 0 -> pin level comes from portB_in (external wiring)
// -----------------------------------------------------------------------------
void VIA6522::updatePBOutput()
{
	uint8_t pins = (orb & ddrb) | (portB_in & ~ddrb);

	//LOG_DEBUG("VIA6522: PB pins=%02X (ORB=%02X DDRB=%02X IN=%02X)",	pins, orb, ddrb, portB_in);
}

void VIA6522::handleCA2WriteSideEffects()
{
	// CA2 mode is encoded in PCR bits 3..1
	uint8_t mode = (pcr >> 1) & 0x07;

	switch (mode)
	{
	case 0: // Input, negative edge, handshake
	case 1: // Input, positive edge, handshake
	case 6: // Input, negative edge, independent interrupt
	case 7: // Input, positive edge, independent interrupt
		// In all input modes, writing ORA has no effect on CA2.
		break;

	case 2: // Output, manual low
		ca2_out = false;
		break;

	case 3: // Output, manual high
		ca2_out = true;
		break;

	case 4: // Output, handshake (PET DAV mode)
		ca2_pulse_count = 1;
		LOG_DEBUG(
			"VIA6522: CA2 handshake pulse armed (PCR=%02X, mode=%u)",
			(unsigned)pcr,
			(unsigned)mode
		);
		break;

	case 5: // Output, pulse low then restore high
		ca2_pulse_count = 1;
		LOG_DEBUG(
			"VIA6522: CA2 manual pulse armed (PCR=%02X, mode=%u)",
			(unsigned)pcr,
			(unsigned)mode
		);
		break;
	}
}

// -----------------------------------------------------------------------------
// handleCB2WriteSideEffects
// Called on ORB writes (case 0 in writeReg).
//
// For CB2 output modes, ORB accesses can trigger handshake or pulse behavior.
//
// Output modes (bit7 = 1):
//   100 (4): handshake output  -> ORB write starts a LOW pulse
//   101 (5): pulse output      -> ORB write starts a LOW pulse (manual)
//
// We use a 2-tick LOW pulse as the PET-safe default you requested.
// Input modes (bit7 = 0) and fixed outputs (6/7) do not get pulses here.
// -----------------------------------------------------------------------------
void VIA6522::handleCB2WriteSideEffects()
{
	const uint8_t mode = cb2_mode(pcr);
	const bool is_output = (pcr & 0x80) != 0;

	if (!is_output)
		return; // input modes: no write-side CB2 behavior

	// MAME-style "AUTO_HS": handshake and pulse outputs both behave the same.
	if (cb2_auto_handshake(pcr))
	{
		// ORB write drives CB2 LOW if it is currently HIGH.
		if (cb2_out)
		{
			cb2_out = false;

			// Keep your debug logging and edge buffer in sync.
			logCB2Transition();
			logCB2Edge();
		}
		return;
	}

	// Fixed output modes (6/7) do not get pulses here, just like MAME.
	// If you want, you can keep any non-AUTO_HS pulse modes here,
	// but by 6522 spec and MAME's macros, there are none.
}

void VIA6522::logCB2Edge()
{
	// Only log when the level actually changes
	if (cb2_out == cb2_last_logged)
		return;

	if (cb2_edge_count < CB2_EDGE_MAX)
	{
		cb2_edges[cb2_edge_count].cycle = cb2_tick_counter;
		cb2_edges[cb2_edge_count].level = cb2_out ? 1 : 0;
		cb2_edge_count++;
	}

	cb2_last_logged = cb2_out;
}

// -----------------------------------------------------------------------------
// CB2 edge log public helpers
// -----------------------------------------------------------------------------
void VIA6522::cb2_reset_edge_log()
{
	cb2_edge_count = 0;
	cb2_tick_counter = 0;
	cb2_last_logged = cb2_out;
	cb2_level_frame_start = cb2_out;
}

uint32_t VIA6522::cb2_get_edge_count() const
{
	return cb2_edge_count;
}

const CB2Edge* VIA6522::cb2_get_edges() const
{
	return cb2_edges;
}

uint32_t VIA6522::cb2_get_tick_counter() const
{
	return cb2_tick_counter;
}

// -----------------------------------------------------------------------------
// updateCB2State
// Helper for CB2 output modes that use pulses (modes 4 and 5).
//
//   mode 4: handshake output, idle HIGH, pulse LOW while cb2_pulse_count > 0
//   mode 5: pulse output,     idle HIGH, pulse LOW while cb2_pulse_count > 0
//
// Fixed-output modes (6,7) are handled directly in handleCB2Logic.
// -----------------------------------------------------------------------------
void VIA6522::updateCB2State(uint8_t mode)
{
	if (cb2_pulse_count > 0)
	{
		// Active pulse: for both handshake and manual pulse, drive LOW.
		cb2_pulse_count--;
		cb2_out = false;
	}
	else
	{
		// Idle: both handshake and manual pulse idle HIGH.
		cb2_out = true;
	}
}

void VIA6522::updateCA2State(uint8_t /*mode*/)
{
	if (ca2_pulse_count > 0)
	{
		// Active pulse: drive CA2 LOW for the duration.
		ca2_pulse_count--;
		ca2_out = false;
	}
	else
	{
		// Idle state: CA2 HIGH.
		ca2_out = true;
	}
}