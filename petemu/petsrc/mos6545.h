#ifndef MOS6545_H
#define MOS6545_H

#include <cstdint>

// -----------------------------------------------------------------------------
// Mos6545 - functional (geometry-driven) MOS 6545 CRTC for the PET 8032.
//
// Design (locked 2026-06-07, built 2026-07-10):
//   - Full R0-R17 register file with real 6545 R/W semantics: address register
//     at base+0 (write) / STATUS at base+0 (read), data at base+1. Only
//     R14-R17 read back; R16/R17 (light pen) are read-only.
//   - The renderer is driven by the DECODED GEOMETRY (R1 columns, R6 rows,
//     R9 scanlines/char, R12/R13 screen start) - there is no per-scanline
//     raster model, and frame timing stays with the synthesized retrace in
//     Pet2001IO (the 8032 drives its IRQ from the same signal).
//   - PET-8032 quirk for the glue, NOT this class: the CRTC character clock
//     counts 2-BYTE units, so the editor ROM programs R1=40 for an 80-column
//     screen. The renderer applies the x2; cols() reports the raw R1.
//   - The PET screen editor uses a SOFTWARE cursor (reverse video), so the
//     hardware cursor (R10/R11/R14/R15) is stored/readable but not rendered.
//
// Mapped by Pet2001IO at $E880 (addr/status) / $E881 (data), 8032 model only.
// -----------------------------------------------------------------------------
class Mos6545 {
public:
    Mos6545() { reset(); }

    void reset();

    // ---- Bus interface ------------------------------------------------------
    void    writeAddr(uint8_t v);      // base+0 write: select register (5 bits)
    uint8_t readStatus() const;        // base+0 read : 6545 status register
    void    writeData(uint8_t v);      // base+1 write: store to selected reg
    uint8_t readData() const;          // base+1 read : R14-R17 only, else 0

    // Host hook: feed the vertical-blank state from the synthesized retrace
    // timing so software polling the status register (bit 7) sees real phase.
    void setVerticalRetrace(bool inVBlank) { vretrace_ = inVBlank; }

    // ---- Decoded geometry (for the renderer / glue) -------------------------
    int cols() const { return reg_[1]; }                          // R1, raw (8032 glue doubles it)
    int rows() const { return reg_[6] & 0x7F; }                   // R6 char rows displayed
    int scanlinesPerChar() const { return (reg_[9] & 0x1F) + 1; } // R9 + 1
    uint16_t screenStart() const { return (uint16_t)(((reg_[12] << 8) | reg_[13]) & 0x3FFF); }
    uint16_t cursorAddr()  const { return (uint16_t)(((reg_[14] << 8) | reg_[15]) & 0x3FFF); }

    // ---- Frame timing derived from the register file ------------------------
    // Units are character clocks, which on the 8032 equal CPU cycles (1 MHz
    // char clock fetching 2 bytes per clock). Callers sanity-check the result:
    // an unprogrammed register file yields nonsense (frameCycles() == 1).
    //   line   = R0+1 clocks
    //   frame  = (R4+1)*(R9+1) + R5 scanlines
    //   active = R6*(R9+1) scanlines (rest of the frame is vertical blank)
    int frameCycles() const {
        return (reg_[0] + 1) * ((reg_[4] + 1) * (reg_[9] + 1) + reg_[5]);
    }
    int vblankCycles() const {
        const int total  = (reg_[4] + 1) * (reg_[9] + 1) + reg_[5];
        const int active = reg_[6] * (reg_[9] + 1);
        return (total > active) ? (total - active) * (reg_[0] + 1) : 0;
    }

    // Raw register access for diagnostics/tests.
    uint8_t reg(int i) const { return (i >= 0 && i < 18) ? reg_[i] : 0; }
    uint8_t selectedReg() const { return addr_; }

    // Bumped whenever a DISPLAY-GEOMETRY register (R1/R6/R9/R12/R13) changes
    // value; the renderer rebuilds only when this moves. Cursor registers are
    // deliberately excluded (they change constantly, and the PET cursor is
    // software-rendered anyway).
    uint32_t geometryEpoch() const { return epoch_; }

private:
    uint8_t  reg_[18];
    uint8_t  addr_;
    bool     vretrace_;
    uint32_t epoch_;
};

#endif // MOS6545_H
