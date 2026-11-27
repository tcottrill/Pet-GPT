// -----------------------------------------------------------------------------
// mos6545.cpp - MOS 6545 register file and raster timing
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
    epoch_ = 0;
    programmed_ = false;
    resetRaster();
}

void Mos6545::resetRaster()
{
    column_ = row_ = raster_ = line_ = adjust_ = syncLines_ = 0;
    rowAddress_ = screenStart();
    vretrace_ = false;
}

bool Mos6545::tick()
{
    // Latch the programmed memory address at the top of a new frame.
    if (line_ == 0 && column_ == 0) {
        rowAddress_ = screenStart();
        if (reg_[7] == 0 && syncLines_ == 0)
            syncLines_ = syncWidth();
    }
    if (++column_ <= reg_[0])
        return false;

    column_ = 0;
    ++line_;
    if (syncLines_ > 0)
        --syncLines_;

    if (adjust_ != 0) {
        if (adjust_++ < reg_[5])
            return false;
    } else {
        if (++raster_ <= reg_[9])
            return false;
        raster_ = 0;
        if (++row_ <= reg_[4]) {
            rowAddress_ = (rowAddress_ + cols()) & 0x3fff;
            if (row_ == reg_[7])
                syncLines_ = syncWidth();
            return false;
        }
        if (reg_[5] != 0) {
            adjust_ = 1;
            return false;
        }
    }

    row_ = raster_ = line_ = adjust_ = 0;
    if (reg_[7] == 0 && syncLines_ == 0)
        syncLines_ = syncWidth();
    return true;
}

void Mos6545::writeAddr(uint8_t v)
{
    addr_ = (uint8_t)(v & 0x1F);   // 5-bit register select
}

// 6545 status register (read of base+0):
//   bit 5 = vertical blank active
//   bit 6 = light-pen register full   (not modeled -> 0)
//   bit 7 = update ready (6545-1 transparent addressing; not used on the PET)
uint8_t Mos6545::readStatus() const
{
    return vretrace_ ? 0x20 : 0x00;
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
    if (addr_ == 0) programmed_ = true;
}

uint8_t Mos6545::readData() const
{
    // 6545: only the cursor (R14/R15) and light pen (R16/R17) read back;
    // everything else is write-only and reads as 0.
    if (addr_ >= 14 && addr_ <= 17) return reg_[addr_];
    return 0x00;
}

void Mos6545::saveState(StateWriter& w) const
{
    w.bytes(reg_, sizeof reg_);
    w.u8(addr_);
    w.flag(vretrace_);
    w.u32(epoch_);
    w.flag(programmed_);
    w.i32(column_); w.i32(row_); w.i32(raster_); w.i32(line_); w.i32(adjust_); w.i32(syncLines_);
    w.u16(rowAddress_);
}

bool Mos6545::loadState(StateReader& r)
{
    Mos6545 t = *this;   // all or nothing
    r.bytes(t.reg_, sizeof t.reg_);
    for (int i = 0; i < 18; ++i) t.reg_[i] &= kRegMask[i];
    t.addr_ = r.u8();
    t.vretrace_ = r.flag();
    t.epoch_ = r.u32();
    t.programmed_ = r.flag();
    t.column_ = r.i32(); t.row_ = r.i32(); t.raster_ = r.i32(); t.line_ = r.i32(); t.adjust_ = r.i32(); t.syncLines_ = r.i32();
    t.rowAddress_ = r.u16();
    // The counters size the renderer's raster buffer, so a damaged file must not reach it.
    if (r.failed() || t.addr_ > 31 || t.column_ < 0 || t.column_ > 255 || t.row_ < 0 || t.row_ > 255 ||
        t.raster_ < 0 || t.raster_ > 32 || t.line_ < 0 || t.line_ > 8192 || t.adjust_ < 0 || t.adjust_ > 32 ||
        t.syncLines_ < 0 || t.syncLines_ > 16 || t.rowAddress_ > 0x3FFF) {
        r.fail();
        return false;
    }
    *this = t;
    return true;
}
