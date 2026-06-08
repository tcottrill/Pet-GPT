#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include <cstring>

#include "sys_log.h"
#include "pet2001io.h"      // Pet2001IO (PIA1 + PIA2 + VIA in one core)
#include "pet2001ieee.h"    // IEEE-488 handshake device
#include "pet2001video.h"   // PET 40-column text video
//#include "petkeys.h"        // PET keyboard matrix + RawInput bridge

// ---------------- Address map (PET 2001N style) ----------------
static constexpr uint16_t PIA1_BASE = 0xE810;  // 4 bytes: E810..E813
static constexpr uint16_t PIA1_END = 0xE813;

static constexpr uint16_t PIA2_BASE = 0xE820;  // 4 bytes: E820..E823
static constexpr uint16_t PIA2_END = 0xE823;

static constexpr uint16_t VIA_BASE = 0xE840;  // 16 bytes: E840..E84F
static constexpr uint16_t VIA_END = 0xE84F;

static constexpr uint16_t VIDRAM_ADDR = 0x8000;  // 1 KB screen RAM (40x25)
static constexpr uint16_t VIDRAM_END = 0x83E7;  // 1000 bytes (0..999)

// ---------------- Bus / memory + device glue ----------------
class PetMem {
public:
    PetMem();
    ~PetMem() = default;

    // CPU-visible memory API
    uint8_t readByte(uint16_t addr);
    void    writeByte(uint16_t addr, uint8_t val);

    void onIrqLine(bool level) { irq_line = level; }

    void    reset();

    // Per CPU CYCLE (advance devices like timers/IRQs)
    void    stepCycle();

    // Held IRQ line from I/O core
    bool    irqLevel() const { return irq_line; }

    // Install helpers used by loader
    void    setRamSize(size_t bytes);  // informational
    void    installROM(uint16_t base, const uint8_t* data, size_t size);
    void    clearROM(uint16_t base, size_t size);

    // Device accessors (if needed by higher layers)
    Pet2001IO& io() { return ioUnit; }
    Pet2001Video& video() { return videoUnit; }
    PetIEEE& ieee() { return ieeeUnit; }
   // PetKeys& keys() { return keysUnit; }

    // Optional: install the two 1KB character sets
    void setVideoCharsets(const uint8_t* page0, const uint8_t* page1) {
        videoUnit.setCharsets(page0, page1);
    }

  //  inline void setRamMirror(uint8_t* mirror, size_t size) noexcept {
   //     ramMirror_ = mirror;
     //   ramMirrorSize_ = size;
   // }
    // Expose RAM image for direct access (e.g. CPU MEM)
    inline uint8_t* ramData() noexcept { return ram.data(); }
    inline size_t   ramSize() const noexcept { return ram.size(); }
    // Configured base-RAM size (writes at/above this are dropped by writeByte()).
    inline size_t   ramConfiguredBytes() const noexcept { return configuredRamBytes; }


private:
    // Memory images
    std::vector<uint8_t> ram;      // 64KB
    std::vector<uint8_t> romMask;  // 64KB (1=ROM visible)
    std::vector<uint8_t> rom;      // 64KB (ROM bytes)

    // Devices
    PetIEEE       ieeeUnit;
    Pet2001Video  videoUnit;
  //  PetKeys       keysUnit;
    Pet2001IO     ioUnit;

    // IRQ latch from I/O (level-sensitive)
    bool   irq_line = false;

    // Bookkeeping
    size_t configuredRamBytes = 0;

    inline bool inRange(uint16_t a, uint16_t lo, uint16_t hi) const {
        return (a >= lo) && (a <= hi);
    }

    uint8_t* ramMirror_ = nullptr;
    size_t    ramMirrorSize_ = 0;

};
