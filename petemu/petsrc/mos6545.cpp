// -----------------------------------------------------------------------------
// mos6545.cpp - functional MOS 6545 CRTC (see mos6545.h for the design notes)
// -----------------------------------------------------------------------------
#include "mos6545.h"

#include <cstring>

// Per-register implemented-bit masks (6545 datasheet; matches VICE's crtc).
// R16/R17 are the light-pen latch: read-only, writes ignored.
static const uint8_t kRegMask[18] = {
    0xFF, // R0  horizontal total
    0xFF, // R1  horizontal displayed (chars; the 8032 fetches 2 bytes/char clock)
    0xFF, // R2  hsync position
    0xFF, // R3  sync widths
    0x7F, // R4  vertical total
    0x1F, // R5  vertical total adjust
    0x7F, // R6  vertical displayed (char rows)
    0x7F, // R7  vsync position
    0xFF, // R8  mode control
    0x1F, // R9  scanlines per char row - 1
    0x7F, // R10 cursor start line + blink mode (bits 6:5)
    0x1F, // R11 cursor end line
    0x3F, // R12 display start address HIGH
    0xFF, // R13 display start address LOW
    0x3F, // R14 cursor address HIGH
    0xFF, // R15 cursor address LOW
    0x00, // R16 light pen HIGH (read-only)
    0x00, // R17 light pen LOW  (read-only)
};

void Mos6545::reset()
{
    // Real-chip contents at reset are undefined; the 8032 editor ROM programs
    // the full file during init. Zeroed here so unprogrammed state is obvious
    // (cols()==0 tells the glue "not initialized yet").
    std::memset(reg_, 0, sizeof(reg_));
    addr_ = 0;
    vretrace_ = false;
    epoch_ = 0;
}

void Mos6545::writeAddr(uint8_t v)
{
    addr_ = (uint8_t)(v & 0x1F);   // 5-bit register select
}

// 6545 status register (read of base+0):
//   bit 7 = vertical retrace active
//   bit 6 = light-pen register full   (not modeled -> 0)
//   bit 5 = update ready (6545-1 transparent addressing; not used on the PET)
uint8_t Mos6545::readStatus() const
{
    return vretrace_ ? 0x80 : 0x00;
}

void Mos6545::writeData(uint8_t v)
{
    if (addr_ >= 18) return;            // unimplemented select: ignored
    if (addr_ == 16 || addr_ == 17) return; // light pen: read-only

    const uint8_t nv = (uint8_t)(v & kRegMask[addr_]);
    const bool geometry = (addr_ == 1 || addr_ == 6 || addr_ == 9 ||
                           addr_ == 12 || addr_ == 13);
    if (geometry && reg_[addr_] != nv)
        ++epoch_;
    reg_[addr_] = nv;
}

uint8_t Mos6545::readData() const
{
    // 6545: only the cursor (R14/R15) and light pen (R16/R17) read back;
    // everything else is write-only and reads as 0.
    if (addr_ >= 14 && addr_ <= 17) return reg_[addr_];
    return 0x00;
}
