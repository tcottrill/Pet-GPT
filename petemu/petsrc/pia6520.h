#ifndef PIA6520__H
#define PIA6520__H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------------
// PIA6520
// -----------------------------------------------------------------------------
// This is a PET-specific "" 6520 PIA wrapper. It is not a full generic
// 6520 core. It mirrors the legacy PET PIA1/PIA2 wiring and register behavior
// closely enough for the existing code, while exposing a clean API to
// Pet2001IO.
//
// Phase 1 changes:
// - Introduce boolean IRQ flags for A1/A2/B1/B2.
// - CRA/CRB CPU reads synthesize bits 7 and 6 from the boolean flags.
// - getPIAIRQ() uses "flag AND enable" logic instead of magic bit masks.
// -----------------------------------------------------------------------------
class PIA6520
{
public:
    PIA6520();


    // -------------------------------------------------------------------------
    // reset
    // -------------------------------------------------------------------------
    // Reset all PIA internal state to power-on defaults that match the old
    // Pet2001IO inlined PIA logic.
    // -------------------------------------------------------------------------
    void reset();

    // -------------------------------------------------------------------------
    // read / write
    // -------------------------------------------------------------------------
    // Full decoding of the low 8 address bits used by Pet2001IO.
    //
    //  0x10..0x13 : PIA1  (PA, CRA, PB, CRB)
    //  0x20..0x23 : PIA2  (PA, CRA, PB, CRB)
    //
    // These are the CPU-visible entry points.
    // -------------------------------------------------------------------------
    uint8_t read(uint16_t lo8);
    void    write(uint16_t lo8, uint8_t data);

    // -------------------------------------------------------------------------
    // Per-register helpers used by read()/write() and by glue code when
    // emulating things like PIA2 bus behavior.
    // -------------------------------------------------------------------------
    uint8_t readPIA_PA();
    uint8_t readPIA_CRA();
    uint8_t readPIA_PB();
    uint8_t readPIA_CRB();

    void writePIA_PA(uint8_t data);
    void writePIA_CRA(uint8_t data);
    void writePIA_PB(uint8_t data);
    void writePIA_CRB(uint8_t data);

    // -------------------------------------------------------------------------
    // PET wiring helpers (called by Pet2001IO)
    // -------------------------------------------------------------------------

    // Forward 6502 SYNC into PIA1 CB1 edge logic.
    void setSyncCB1(bool level);

    void setCA1(bool level);
    void setCB1(bool level);
    void setCA2(bool level);
    void setCB2(bool level);

    // Let Pet2001IO drive PIA1/PIA2 PB input pins (keyboard rows, IEEE, etc.).
    void setPIA_PB_in(uint8_t v) { pb_in = v; }

    // -------------------------------------------------------------------------
    // Lightweight accessors (used by Pet2001IO)
    // -------------------------------------------------------------------------

    // Port A
    uint8_t getPIA_PA_in() const { return pa_in; }
    uint8_t getPIA_PA_out() const { return pa_out; }
    uint8_t getPIA_DDRA() const { return ddra; }

    void    setPIA_PA_in(uint8_t v) { pa_in = v; }
    void    setPIA_DDRA(uint8_t v) { ddra = v; };

    // Port B
    uint8_t getPIA_PB_in() const { return pb_in; }
    uint8_t getPIA_PB_out() const { return pb_out; }
    uint8_t getPIA_DDRB() const { return ddrb; }

    // Raw CRA/CRB contents (used by PET glue to check mode bits, not IRQ flags).
    uint8_t getPIA_CRA() const { return cra; }
    uint8_t getPIA_CRB() const { return crb; }

    // CA2 output latch (used for screen blank / EOIout).
    bool    getPIA_CA2_out() const { return ca2_out != 0; }

    // Combined IRQ output line for this PIA.
    // True if any enabled IRQ (A or B side) is pending.
    bool getIRQ() const;

    // Optional per-side queries if you ever need them.
    bool getIRQA() const;
    bool getIRQB() const;

    void handleCA1Edge();
    void handleCB1Edge();

    void handleCA2Edge();
    void handleCB2Edge();

    void tick();
  

private:
   
    // Port A state
    uint8_t pa_in;
    uint8_t pa_out;
    uint8_t ddra;
    uint8_t cra;

    // Port B state
    uint8_t pb_in;
    uint8_t pb_out;
    uint8_t ddrb;
    uint8_t crb;

    // Misc control pins
    uint8_t cb1;      // CB1 input latch as 0/1

    bool ca1_in;
    bool cb1_in;
    bool old_ca1;
    bool old_cb1;

    bool ca2_in;
    bool cb2_in;
    bool old_ca2;
    bool old_cb2;

    bool ca2_out;
    bool cb2_out;

    // Boolean IRQ flags (mirrors of CRA/CRB bits 7 and 6)
    bool irq_a1;  // CRA bit7
    bool irq_a2;  // CRA bit6
    bool irq_b1;  // CRB bit7
    bool irq_b2;  // CRB bit6
};

#endif // PIA6520__H
