#include <filesystem>
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



#include "pet_machine.h"
#include <fstream>
#include <iterator>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include "sys_log.h"

// ---- Static handler callbacks ------------------------------------------------

static bool logged = false;

uint8_t PetMachine::s_read8(UINT32 ofs, MemoryReadByte* h)
{
    // ofs is relative; reconstruct absolute: lowAddr + ofs
    auto* pm = reinterpret_cast<PetMem*>(h->pUserArea);
    const uint16_t addr = static_cast<uint16_t>(h->lowAddr + ofs);
    return pm->readByte(addr);
}

void PetMachine::s_write8(UINT32 ofs, UINT8 data, MemoryWriteByte* h)
{
    auto* pm = reinterpret_cast<PetMem*>(h->pUserArea);
    const uint16_t addr = static_cast<uint16_t>(h->lowAddr + ofs);
    pm->writeByte(addr, data);
}

// ---- Construction ------------------------------------------------------------

PetMachine::PetMachine()
  //  : ramImage(65536, 0x00),
    : rdTbl(),
    wrTbl(),
    memory(), 
    irq_vector_ready_(false),
    last_irq_level_(false)
{
    // Mirror is initialized to zero; ROM loads will be mirrored explicitly.
   // std::fill(ramImage.begin(), ramImage.end(), 0x00);

    // Build flat handlers that forward all addresses [0x0000..0xFFFF]
    installFlatHandlers();

    // Create CPU: point MEM to our ramImage, and pass handler tables
    // NOTE: addr mask 0xFFFF, cpu number 0
   // cpuPtr = new cpu_6502(ramImage.data(), rdTbl.data(), wrTbl.data(), 0xFFFF, 0);
    // The core prefers handlers; direct MEM is used for vectors/stack.
    
    // IMPORTANT: mirror PetMem writes into the CPU MEM buffer
   // memory.setRamMirror(ramImage.data(), ramImage.size());

    cpuPtr = std::make_unique<cpu_6502>(memory.ramData(), rdTbl.data(), wrTbl.data(), 0xFFFF, 0);

    // The PET keeps ROM in a separate image served via the read handler (PetMem),
    // NOT in the flat MEM[] array. The new core defaults to fetching opcodes/operands
    // directly from MEM[] (readop), which would read zeros from ROM space here.
    // Route opcode/operand fetches through the memory handlers so ROM is visible.
    cpuPtr->opfetch_through_handlers(true);

    // Configure PET timing: 1.000 MHz CPU, 60 Hz SYNC, ~30% low pulse
    //memory.io().setCpuClockHz(1'000'000.0);
    //memory.io().setRefreshHz(60.0);
    //memory.io().setSyncDutyLow(3840.0 / 16667.0);
}

// Install a single catch-all read/write handler pair
void PetMachine::installFlatHandlers()
{
    rdTbl.clear();
    wrTbl.clear();

    MemoryReadByte rAll{};
    rAll.lowAddr = 0x0000;
    rAll.highAddr = 0xFFFF;
    rAll.memoryCall = &PetMachine::s_read8;
    rAll.pUserArea = &memory;
    rdTbl.push_back(rAll);

    MemoryReadByte rEnd{ static_cast<UINT32>(-1), 0, nullptr, nullptr };
    rdTbl.push_back(rEnd);

    MemoryWriteByte wAll{};
    wAll.lowAddr = 0x0000;
    wAll.highAddr = 0xFFFF;
    wAll.memoryCall = &PetMachine::s_write8;
    wAll.pUserArea = &memory;
    wrTbl.push_back(wAll);

    MemoryWriteByte wEnd{ static_cast<UINT32>(-1), 0, nullptr, nullptr };
    wrTbl.push_back(wEnd);
}

// ---- Public API --------------------------------------------------------------

void PetMachine::setVideoCharsets(const uint8_t* cs1, const uint8_t* cs2)
{
    memory.setVideoCharsets(cs1, cs2);
}

bool PetMachine::loadRomFromFile(const std::string& path, uint16_t base)
{
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    if (buf.empty()) return false;
    return loadRom(buf.data(), buf.size(), base);
}

bool PetMachine::loadRom(const uint8_t* data, size_t size, uint16_t base)
{
    if (!data || size == 0) return false;
    memory.installROM(base, data, size);          // write to bus overlay
    updateBasic1DiskTraps();
    return true;
}

void PetMachine::reset()
{
    // PetMem::reset() leaves RAM as-is (video screen prefilled with spaces on cold start).
    cpuPtr->reset6502();  // CPU reads reset vector from MEM[$FFFC/$FFFD]
    memory.reset();       // reset I/O + video
    irq_vector_ready_ = false;   // re-arm guardrail on reset
    last_irq_level_ = false;
    basic1LoadPending_ = false;
    updateBasic1DiskTraps();
}

// ---- BASIC 1 disk traps ------------------------------------------------------
//
// Why: the original PET 2001 ROM (BASIC 1, 901447-01..07) shipped with disk
// LOAD/SAVE code that never worked on the real machine. UNLISTEN/UNTALK
// ($F17A/$F17E) store $3F/$5F into the device number at $F1, so the following
// TALK goes out as $7F; LOAD never sends a secondary address and leaves ATN
// low; and the load loop at $F37E only ends on ST bit 7, while end of file is
// bit 6, so it waits for ever. The end-of-program pointer is never set either.
// (From a disassembly of these ROM files, cross-checked against the commented
// listing at zimmers.net.) LOAD"X",8 therefore hangs at LOADING and SAVE"X",8
// writes nothing.
//
// What: as an emulator convenience, LOAD, VERIFY and SAVE on device 8 are done
// here by watching the PC and calling the drive's host-side helpers, leaving
// the ROM to print its own messages and finish its own bookkeeping. Any other
// device number runs the ROM untouched, and data files (OPEN/PRINT#/INPUT#,
// channel 15) still go over the emulated bus. The traps only fire when the ROM
// bytes at every trapped place match this exact ROM.
//
// ROM addresses and variables (BASIC 1 KERNAL):
namespace {
constexpr uint16_t B1_LOAD_SENDNAME = 0xF375; // JSR $F462 (name on bus), JSR $F0B6 (TALK)
constexpr uint16_t B1_LOAD_MSG      = 0xF37B; // JSR $F422: print LOADING / VERIFYING
constexpr uint16_t B1_LOAD_LOOP     = 0xF37E; // byte loop that never ends
constexpr uint16_t B1_LOAD_DONE     = 0xF3CC; // after the loop: messages, end pointer, relink
constexpr uint16_t B1_NOT_FOUND     = 0xF579; // LDY #$24: ?FILE NOT FOUND ERROR
constexpr uint16_t B1_SAVE_BUS      = 0xF6C0; // JSR $F71C, name on bus, LISTEN, send bytes
constexpr uint16_t B1_SAVE_RTS      = 0xF735; // plain RTS at the same stack depth
constexpr uint16_t ZP_FNLEN = 0xEE, ZP_FA = 0xF1;   // name length, device
constexpr uint16_t ZP_FNADR = 0xF9;           // $F9/$FA: pointer to the name
constexpr uint16_t ZP_LADDR = 0xF7;           // $F7/$F8: start address ($0400)
constexpr uint16_t ZP_EADDR = 0xE5;           // $E5/$E6: end address (exclusive)
constexpr uint16_t VERIFY_FLAG = 0x020B;      // 0 = LOAD, else VERIFY
constexpr uint16_t STATUS_ST = 0x020C;

// The ROM bytes the traps depend on, checked against the real 901447-06 file.
struct Sig { uint16_t addr; uint8_t len; uint8_t bytes[15]; };
const Sig kBasic1Signature[] = {
    { B1_LOAD_SENDNAME, 15, { 0x20,0x62,0xF4, 0x20,0xB6,0xF0, 0x20,0x22,0xF4, 0x20,0x39,0xF3, 0x20,0x87,0xF1 } },
    { B1_LOAD_DONE,      3, { 0xAD,0x0B,0x02 } },                    // LDA $020B
    { B1_NOT_FOUND,      2, { 0xA0,0x24 } },                         // LDY #$24
    { B1_SAVE_BUS,       9, { 0x20,0x1C,0xF7, 0x20,0x62,0xF4, 0x20,0xBA,0xF0 } },
    { B1_SAVE_RTS,       1, { 0x60 } },                              // RTS
};
} // namespace

void PetMachine::setBasic1DiskTraps(bool enabled)
{
    basic1TrapsEnabled_ = enabled;
    updateBasic1DiskTraps();
}

void PetMachine::updateBasic1DiskTraps()
{
    bool match = basic1TrapsEnabled_;
    for (const Sig& s : kBasic1Signature)
        for (int i = 0; match && i < s.len; ++i)
            if (memory.readByte(uint16_t(s.addr + i)) != s.bytes[i]) match = false;
    basic1TrapsActive_ = match;
    if (!match) basic1LoadPending_ = false;
}

void PetMachine::basic1DiskTrap()
{
    const uint16_t pc = cpuPtr->get_pc();
    if (pc != B1_LOAD_SENDNAME && pc != B1_LOAD_LOOP && pc != B1_SAVE_BUS) return;

    auto word = [this](uint16_t a) {
        return uint16_t(memory.readByte(a) | (memory.readByte(uint16_t(a + 1)) << 8));
    };
    auto fileName = [&]() {
        std::string name;
        const uint16_t p = word(ZP_FNADR);
        for (int i = 0; i < memory.readByte(ZP_FNLEN); ++i)
            name += char(memory.readByte(uint16_t(p + i)));
        return name;
    };

    if (pc == B1_LOAD_SENDNAME) {
        // LOAD/VERIFY stage 1: skip the bus OPEN and TALK; the ROM still prints
        // LOADING or VERIFYING before reaching the loop.
        if (memory.readByte(ZP_FA) != 8) return;
        basic1LoadPending_ = true;
        cpuPtr->set_pc(B1_LOAD_MSG);
        return;
    }

    if (pc == B1_LOAD_LOOP) {
        // Stage 2: do the whole transfer, then hand back to the ROM after its loop.
        if (!basic1LoadPending_) return;
        basic1LoadPending_ = false;
        std::vector<uint8_t> payload;
        uint16_t addr = 0;
        if (!memory.ieee().hostLoadFile(fileName(), payload, addr)) {
            cpuPtr->set_pc(B1_NOT_FOUND);
            return;
        }
        uint8_t st = 0x40;                       // end of file
        const bool verify = memory.readByte(VERIFY_FLAG) != 0;
        if (!verify) {
            // Into RAM only: never at or above $8000 (screen, ROM, I/O).
            const size_t limit = std::min<size_t>(memory.ramConfiguredBytes(), 0x8000);
            if (size_t(addr) + payload.size() > limit) {
                st |= 0x10;                      // ?LOAD ERROR, nothing written
            } else {
                for (size_t i = 0; i < payload.size(); ++i)
                    memory.writeByte(uint16_t(addr + i), payload[i]);
                const uint16_t end = uint16_t(addr + payload.size());
                memory.writeByte(ZP_EADDR, uint8_t(end & 0xFF));
                memory.writeByte(ZP_EADDR + 1, uint8_t(end >> 8));
            }
        } else {
            for (size_t i = 0; i < payload.size(); ++i)
                if (addr + i > 0xFFFF || memory.readByte(uint16_t(addr + i)) != payload[i]) {
                    st |= 0x10;                  // ?VERIFY ERROR
                    break;
                }
        }
        memory.writeByte(STATUS_ST, st);
        cpuPtr->set_pc(B1_LOAD_DONE);
        return;
    }

    // SAVE: write the program file and return from the ROM's SAVE routine. Any
    // drive error (63 FILE EXISTS, 34, 26...) is left in the channel-15 status,
    // as on later ROMs, where SAVE itself reports nothing either.
    if (memory.readByte(ZP_FA) != 8) return;
    uint16_t start = word(ZP_LADDR);
    const uint16_t end = word(ZP_EADDR);
    // BASIC 1 saves from $0400, whose byte is the 0 before the first line. A
    // standard PET program file starts at $0401, which every model can load.
    if (start == 0x0400 && memory.readByte(0x0400) == 0) start = 0x0401;
    std::vector<uint8_t> prg{ uint8_t(start & 0xFF), uint8_t(start >> 8) };
    for (uint32_t a = start; a < end; ++a) prg.push_back(memory.readByte(uint16_t(a)));
    memory.ieee().hostSaveFile(fileName(), prg);
    memory.writeByte(STATUS_ST, 0);
    cpuPtr->set_pc(B1_SAVE_RTS);
}

int PetMachine::runCycles(int cycles)
{
    int executed = 0;

    while (executed < cycles)
    {
        // get_pc() is the address of the next instruction to run.
        if (basic1TrapsActive_) basic1DiskTrap();

               // Step CPU one instruction or interrupt entry.
        const int c = cpuPtr->step6502();

        // Advance devices for each CPU cycle executed.
        for (int i = 0; i < c; ++i) {
            stepCycle();
        }

        // IRQ line is physically wired at all times (level-sensitive).
        if (memory.irqLevel()) {
            cpuPtr->irq6502(IRQ_HOLD); // hold low
        }
        else {
            cpuPtr->m6502clearpendingint();  // release high
        }

        executed += c;
    }

    return executed;
}



void PetMachine::stepCycle()
{
    memory.stepCycle(); // Pet2001IO: PIA/VIA timers & CB2/handshakes in lockstep
    
}

