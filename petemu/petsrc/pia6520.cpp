#include "pia6520.h"
#include <string.h>
#include "sys_log.h"

/*
	PIA6520
	-------------------------------------------------------------------------
	Implementation of the MOS 6520 / 6821 PIA.
*/

namespace
{
	inline bool IRQ1_ENABLED(uint8_t c) { return (c & 0x01) != 0; }
	inline bool C1_LOW_TO_HIGH(uint8_t c) { return (c & 0x02) != 0; }
	inline bool C1_HIGH_TO_LOW(uint8_t c) { return (c & 0x02) == 0; }
	inline bool OUTPUT_SELECTED(uint8_t c) { return (c & 0x04) != 0; }

	inline bool IRQ2_ENABLED(uint8_t c) { return (c & 0x08) != 0; }
	inline bool STROBE_E_RESET(uint8_t c) { return (c & 0x08) != 0; }
	inline bool SET_C2(uint8_t c) { return (c & 0x08) != 0; }

	inline bool C2_LOW_TO_HIGH(uint8_t c) { return (c & 0x10) != 0; }
	inline bool C2_HIGH_TO_LOW(uint8_t c) { return (c & 0x10) == 0; }
	inline bool C2_SET_MODE(uint8_t c) { return (c & 0x10) != 0; }
	inline bool C2_STROBE_MODE(uint8_t c) { return (c & 0x10) == 0; }

	inline bool C2_OUTPUT(uint8_t c) { return (c & 0x20) != 0; }
	inline bool C2_INPUT(uint8_t c) { return (c & 0x20) == 0; }

	inline bool STROBE_C1_RESET(uint8_t c) { return (c & 0x08) == 0; }
}

PIA6520::PIA6520()
{
	reset();
}

void PIA6520::reset()
{
	pa_in = 0xFF;
	pa_out = 0x00;
	ddra = 0x00;
	cra = 0x00;

	pb_in = 0xFF;
	pb_out = 0x00;
	ddrb = 0x00;
	crb = 0x00;

	ca2_out = false;
	cb1 = 0;

	ca1_in = false;
	cb1_in = false;
	old_ca1 = false;
	old_cb1 = false;

	ca2_in = false;
	cb2_in = false;
	old_ca2 = false;
	old_cb2 = false;

	// FIX: Ensure outputs are cleared or set to defaults
	ca2_out = false;
	cb2_out = false;

	irq_a1 = false;
	irq_a2 = false;
	irq_b1 = false;
	irq_b2 = false;
}

void PIA6520::setCA1(bool level)
{
	ca1_in = level;
	handleCA1Edge();
}

void PIA6520::setCB1(bool level)
{
	cb1_in = level;
	handleCB1Edge();
}
void PIA6520::setCA2(bool level)
{
	ca2_in = level;
	handleCA2Edge();
}

void PIA6520::setCB2(bool level)
{
	cb2_in = level;
	handleCB2Edge();
}

void PIA6520::setSyncCB1(bool level)
{
	cb1_in = level;
	handleCB1Edge();
}

// -----------------------------------------------------------------------------
// READ
// -----------------------------------------------------------------------------
uint8_t PIA6520::read(uint16_t lo8)
{
	switch (lo8 & 0x03)
	{
	case 0x00: return readPIA_PA();
	case 0x01: return readPIA_CRA();
	case 0x02: return readPIA_PB();
	case 0x03: return readPIA_CRB();
	default:   return 0xFF;
	}
}

// -----------------------------------------------------------------------------
// WRITE
// -----------------------------------------------------------------------------
void PIA6520::write(uint16_t lo8, uint8_t data)
{
	switch (lo8 & 0x03)
	{
	case 0x00: writePIA_PA(data); return;
	case 0x01: writePIA_CRA(data); return;
	case 0x02: writePIA_PB(data); return;
	case 0x03: writePIA_CRB(data); return;
	}
}

// -----------------------------------------------------------------------------
// PORT A READ
// -----------------------------------------------------------------------------
uint8_t PIA6520::readPIA_PA()
{
	// Bit 2 in CRA determines Data (1) or DDR (0)
	if (!(cra & 0x04))
	{
		return ddra;
	}

	// Mix input pins with output latches based on DDR
	uint8_t val = static_cast<uint8_t>((pa_in & ~ddra) | (pa_out & ddra));

	// Reading DATA clears the interrupt flags
	irq_a1 = false;
	irq_a2 = false;

	// Strobe Logic: If CA2 is Output in Strobe Mode (100 or 101),
	// a read of PA triggers the strobe (High->Low transition).
	// (Note: Your original code had this logic, preserved here).
	if (C2_OUTPUT(cra) && C2_STROBE_MODE(cra))
	{
		ca2_out = false;
		// If reset by E (Read), restore High immediately (pulse).
		if (STROBE_E_RESET(cra))
		{
			ca2_out = true;
		}
	}

	return val;
}

uint8_t PIA6520::readPIA_CRA()
{
	uint8_t val = (uint8_t)(cra & 0x3F);
	if (irq_a1) val |= 0x80;
	if (irq_a2) val |= 0x40;
	return val;
}

// -----------------------------------------------------------------------------
// PORT B READ
// -----------------------------------------------------------------------------
uint8_t PIA6520::readPIA_PB()
{
	if (!(crb & 0x04))
	{
		return ddrb;
	}

	uint8_t val = static_cast<uint8_t>((pb_in & ~ddrb) | (pb_out & ddrb));

	//LOG_DEBUG("PIA1 PB READ: val=%02X pb_in=%02X pb_out=%02X DDRB=%02X CRB=%02X", val, pb_in, pb_out, ddrb, crb);

	// Reading DATA clears interrupt flags
	irq_b1 = false;
	irq_b2 = false;

	return val;
}

uint8_t PIA6520::readPIA_CRB()
{
	uint8_t val = (uint8_t)(crb & 0x3F);
	if (irq_b1) val |= 0x80;
	if (irq_b2) val |= 0x40;
	return val;
}

// -----------------------------------------------------------------------------
// PORT A WRITE
// -----------------------------------------------------------------------------
void PIA6520::writePIA_PA(uint8_t data)
{
	if (cra & 0x04)
	{
		pa_out = data;
		//LOG_DEBUG("PIA1 WRITE PA_out = %02X", data);
	}
	else
	{
		ddra = data;
		//LOG_DEBUG("PIA1 WRITE DDRA = %02X", data);
	}
}

void PIA6520::writePIA_CRA(uint8_t data)
{
	cra = static_cast<uint8_t>(data & 0x3F);

	// CA2 Output Control (Bits 5..3)
	// 111 (0x38) = Set High
	// 110 (0x30) = Set Low
	const bool ca2_mode_is_output = ((cra & 0x38) == 0x38);

	if (ca2_mode_is_output && !ca2_out)
	{
		ca2_out = true;
	}
	else if (((cra & 0x38) == 0x30) && ca2_out)
	{
		ca2_out = false;
	}
}

// -----------------------------------------------------------------------------
// PORT B WRITE
// -----------------------------------------------------------------------------
void PIA6520::writePIA_PB(uint8_t data)
{
	if (crb & 0x04)
	{
		pb_out = data;

		// CB2 Write Strobe Logic
		// If CB2 is Output Strobe Mode, Write to PB triggers it.
		if (C2_OUTPUT(crb) && C2_STROBE_MODE(crb))
		{
			cb2_out = false; // Go Low

			// Restore High if in Pulse Mode
			if (STROBE_E_RESET(crb))
			{
				cb2_out = true;
			}
		}
	}
	else
	{
		//LOG_DEBUG("PIA1 WRITE DDRB = %02X", data);
		ddrb = data;
	}
}

// -----------------------------------------------------------------------------
// CRB WRITE - FIX APPLIED HERE
// -----------------------------------------------------------------------------
void PIA6520::writePIA_CRB(uint8_t data)
{
	crb = static_cast<uint8_t>(data & 0x3F);

	// FIX: Manual CB2 Output Control (Bits 5..3)
	// Just like CA2, CB2 can be manually driven High (111) or Low (110).
	// The original code was missing this logic block.

	// Mask 0x38 = Bits 5,4,3
	uint8_t mode = crb & 0x38;

	if (mode == 0x38) // 111 = Manual Output High
	{
		if (!cb2_out) cb2_out = true;
	}
	else if (mode == 0x30) // 110 = Manual Output Low
	{
		if (cb2_out) cb2_out = false;
	}
}

// -----------------------------------------------------------------------------
// EDGES
// -----------------------------------------------------------------------------

void PIA6520::handleCA1Edge()
{
	bool prev = old_ca1;
	bool curr = ca1_in;

	if (prev != curr)
	{
		bool active = (curr && C1_LOW_TO_HIGH(cra)) || (!curr && C1_HIGH_TO_LOW(cra));

		if (active)
		{
			irq_a1 = true;

			// CA2 Strobe Restore on C1 Active Edge
			if (C2_OUTPUT(cra) && C2_STROBE_MODE(cra) && STROBE_C1_RESET(cra))
			{
				if (!ca2_out) ca2_out = true;
			}
		}
	}
	old_ca1 = curr;
}

void PIA6520::handleCB1Edge()
{
	bool prev = old_cb1;
	bool curr = cb1_in;

	if (prev != curr)
	{
		bool active = (curr && C1_LOW_TO_HIGH(crb)) || (!curr && C1_HIGH_TO_LOW(crb));

		if (active)
		{
			irq_b1 = true;

			// CB2 Strobe Restore on C1 Active Edge
			if (C2_OUTPUT(crb) && C2_STROBE_MODE(crb) && STROBE_C1_RESET(crb))
			{
				if (!cb2_out) cb2_out = true;
			}
		}
	}
	old_cb1 = curr;
}

void PIA6520::handleCA2Edge()
{
	bool prev = old_ca2;
	bool curr = ca2_in;

	if (prev != curr && C2_INPUT(cra))
	{
		bool active = (curr && C2_LOW_TO_HIGH(cra)) || (!curr && C2_HIGH_TO_LOW(cra));
		if (active) irq_a2 = true;
	}
	old_ca2 = curr;
}

void PIA6520::handleCB2Edge()
{
	bool prev = old_cb2;
	bool curr = cb2_in;

	if (prev != curr && C2_INPUT(crb))
	{
		bool active = (curr && C2_LOW_TO_HIGH(crb)) || (!curr && C2_HIGH_TO_LOW(crb));
		if (active) irq_b2 = true;
	}
	old_cb2 = curr;
}

void PIA6520::tick()
{
	handleCA1Edge();
	handleCB1Edge();
	handleCA2Edge();
	handleCB2Edge();
}

// -----------------------------------------------------------------------------
// IRQ
// -----------------------------------------------------------------------------

bool PIA6520::getIRQA() const
{
	bool active1 = irq_a1 && IRQ1_ENABLED(cra);
	bool active2 = irq_a2 && IRQ2_ENABLED(cra);
	return active1 || active2;
}

bool PIA6520::getIRQB() const
{
	bool active1 = irq_b1 && IRQ1_ENABLED(crb);
	bool active2 = irq_b2 && IRQ2_ENABLED(crb);
	return active1 || active2;
}

bool PIA6520::getIRQ() const
{
	return getIRQA() || getIRQB();
}