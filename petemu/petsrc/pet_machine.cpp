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

uint8_t PetMachine::s_read8(UINT32 ofs, MemoryReadByte* h)
{
    // ofs is relative; reconstruct absolute: lowAddr + ofs
    auto* machine = static_cast<PetMachine*>(h->pUserArea);
    const uint16_t addr = static_cast<uint16_t>(h->lowAddr + ofs);
    if (machine->timeCrtcIo_ && (addr == 0xe840 || addr == 0xe844 ||
        addr == 0xe845 || addr == 0xe880)) {
        // The CPU samples the bus before devices advance at the end of the
        // data-read clock. Run the preceding clocks before supplying the byte.
        const int clock = machine->cpuPtr->absoluteDataReadCycle() - 1;
        while (machine->deviceCycles_ < clock) {
            machine->stepCycle();
            ++machine->deviceCycles_;
        }
    }
    return machine->memory.readByte(addr);
}

void PetMachine::s_write8(UINT32 ofs, UINT8 data, MemoryWriteByte* h)
{
    auto* machine = static_cast<PetMachine*>(h->pUserArea);
    const uint16_t addr = static_cast<uint16_t>(h->lowAddr + ofs);
    if (machine->timeVideoWrites_ && ((addr & 0xf000) == 0x8000 ||
        addr == 0xe84c || addr == 0xe880 || addr == 0xe881 ||
        (machine->timeCrtcIo_ && addr >= 0xe844 && addr <= 0xe847))) {
        // This core emits a single final data write for stores and RMW
        // instructions. Screen SRAM, the CRTC, and the VIA timer used to
        // synchronize to the raster see that write on the final clock.
        machine->videoWrite_ = {addr, data};
        machine->videoWritePending_ = true;
        return;
    }
    machine->memory.writeByte(addr, data);
}

// ---- Construction ------------------------------------------------------------

PetMachine::PetMachine()
  //  : ramImage(65536, 0x00),
    // irq_vector_ready_ and last_irq_level_ start false (member initialisers).
    : rdTbl(),
    wrTbl(),
    memory()
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

    cpuPtr = std::make_unique<cpu_6502>(memory.ramData(), rdTbl.data(), wrTbl.data(), uint16_t(0xFFFF), 0);

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
    rAll.pUserArea = this;
    rdTbl.push_back(rAll);

    MemoryReadByte rEnd{ static_cast<UINT32>(-1), 0, nullptr, nullptr };
    rdTbl.push_back(rEnd);

    MemoryWriteByte wAll{};
    wAll.lowAddr = 0x0000;
    wAll.highAddr = 0xFFFF;
    wAll.memoryCall = &PetMachine::s_write8;
    wAll.pUserArea = this;
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
    timeVideoWrites_ = memory.video().hasRaster();
    timeCrtcIo_ = memory.io().hasCrtc();

    while (executed < cycles)
    {
        // get_pc() is the address of the next instruction to run.
        if (basic1TrapsActive_) basic1DiskTrap();

        // Step CPU one instruction or interrupt entry.
        deviceCycles_ = 0;
        const int c = cpuPtr->step6502();

        // Advance devices for each CPU cycle executed.
        for (int i = deviceCycles_; i < c; ++i) {
            stepCycle();
        }
        // The video fetch precedes the 6502 write on the same clock.
        if (videoWritePending_) {
            memory.writeByte(videoWrite_.address, videoWrite_.value);
            videoWritePending_ = false;
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

    timeVideoWrites_ = false;
    timeCrtcIo_ = false;
    return executed;
}



void PetMachine::stepCycle()
{
    memory.stepCycle(); // Pet2001IO: PIA/VIA timers & CB2/handshakes in lockstep

}

// ---- Save states -------------------------------------------------------------

namespace {
struct TapeChunk {
    bool mounted = false, playing = false;
    std::string path;
    uint32_t pulses = 0, remaining = 0;
    uint64_t position = 0;
};
struct DiskChunk {
    std::string path;
    bool busy = false;
};

bool readTapeChunk(StateReader r, TapeChunk& out)
{
    out.mounted = r.flag();
    out.path = r.str();
    out.pulses = r.u32();
    out.position = r.u64();
    out.remaining = r.u32();
    out.playing = r.flag();
    return !r.failed() && (!out.mounted || out.position <= out.pulses);
}

bool readDiskChunk(StateReader r, DiskChunk& out)
{
    out.path = r.str();
    out.busy = r.flag();
    return !r.failed();
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) return false;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::string baseName(const std::string& path)
{
    return std::filesystem::u8path(path).filename().u8string();
}

void addNote(std::string* warning, const std::string& text)
{
    if (!warning) return;
    if (!warning->empty()) *warning += "; ";
    *warning += text;
}
} // namespace

void PetMachine::saveState(StateWriter& w, std::string* warning)
{
    // The CPU chunk is the core's own portable blob (see cpu_6502.h).
    uint8_t cpuBlob[cpu_6502::kStateMaxSize];
    const size_t cpuBytes = cpuPtr->save_state(cpuBlob, sizeof cpuBlob);
    w.beginChunk("CPU "); w.bytes(cpuBlob, cpuBytes); w.endChunk();
    // Machine-level latch that is not CPU state.
    w.beginChunk("MCH "); w.flag(basic1LoadPending_); w.endChunk();

    w.beginChunk("MEM "); memory.saveState(w); w.endChunk();
    memory.io().saveState(w);   // PIA1, PIA2, VIA , CRTC, IO
    w.beginChunk("VID "); memory.video().saveState(w); w.endChunk();

    const PetTape& tape = memory.io().tape();
    w.beginChunk("TAPE");
    w.flag(tape.mounted());
    w.str(tape.sourcePath());
    w.u32(uint32_t(tape.pulseCount()));
    w.u64(uint64_t(tape.position()));
    w.u32(tape.remaining());
    w.flag(tape.playing());
    w.endChunk();

    const bool busy = memory.ieee().isBusy();
    w.beginChunk("DISK");
    w.str(memory.ieee().getD64Image());
    w.flag(busy);
    w.endChunk();
    if (busy && warning) *warning = "saved while the disk drive was busy; a load in progress will not resume";
}

bool PetMachine::validateState(const StateFile& file, std::string& error) const
{
    PetMachine scratch;
    return scratch.applyState(file, error, nullptr, false);
}

bool PetMachine::loadState(const StateFile& file, std::string& error, std::string* warning)
{
    if (!validateState(file, error)) return false;
    return applyState(file, error, warning, true);
}

bool PetMachine::applyState(const StateFile& file, std::string& error, std::string* warning, bool media)
{
    // The small chunks first: everything that can be refused is checked before
    // the first byte of this machine changes (the device chunks are covered by
    // loadState() having run this on a scratch machine already).
    TapeChunk tape; DiskChunk disk;
    // load_state() leaves the CPU alone when it refuses, so the register
    // file is the first thing applied and the first thing that can fail.
    const uint8_t* cpuBlob = nullptr; size_t cpuBytes = 0;
    if (!file.chunkData("CPU ", cpuBlob, cpuBytes) || !cpuPtr->load_state(cpuBlob, cpuBytes)) { error = "the CPU chunk is missing or damaged"; return false; }
    bool basic1Pending = false;
    if (file.has("MCH ")) { StateReader mch = file.find("MCH "); basic1Pending = mch.flag(); }
    if (!readTapeChunk(file.find("TAPE"), tape)) { error = "the tape chunk is missing or damaged"; return false; }
    if (!readDiskChunk(file.find("DISK"), disk)) { error = "the disk chunk is missing or damaged"; return false; }

    StateReader mem = file.find("MEM ");
    if (!memory.loadState(mem)) { error = "the memory chunk is missing or damaged"; return false; }

    // The drive's image first: loading the I/O chunks restarts the drive, and its
    // power-on message depends on the mounted image type.
    if (media) {
        Pet2001IO& io = memory.io();
        if (disk.path.empty()) {
            if (io.ieeeIsD64Mounted()) io.setIeeeD64Image("");
        } else if (!(io.ieeeIsD64Mounted() && memory.ieee().getD64Image() == disk.path)) {
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::u8path(disk.path), ec) || !io.setIeeeD64Image(disk.path)) {
                if (io.ieeeIsD64Mounted()) io.setIeeeD64Image("");
                addNote(warning, "disk image " + baseName(disk.path) + " was not found or cannot be read: drive 8 is the files folder");
            }
        }
    }

    if (!memory.io().loadState(file)) { error = "a device chunk (PIA1, PIA2, VIA, CRTC or IO) is missing or damaged"; return false; }
    StateReader vid = file.find("VID ");
    if (!memory.video().loadState(vid)) { error = "the video chunk is missing or damaged"; return false; }

    if (media) {
        PetTape& transport = memory.io().tape();
        if (!tape.mounted) {
            transport.eject();
        } else {
            std::vector<uint8_t> image;
            PetTape staged;
            std::string reason;
            if (tape.path.empty()) {
                transport.eject();
                addNote(warning, "the saved tape was not attached from a file, so no tape is attached");
            } else if (!readFileBytes(tape.path, image)) {
                transport.eject();
                addNote(warning, "tape file " + baseName(tape.path) + " was not found: no tape attached");
            } else if (!staged.attach(image, reason)) {
                transport.eject();
                addNote(warning, "tape file " + baseName(tape.path) + " cannot be attached (" + reason + "): no tape attached");
            } else {
                staged.setSourcePath(tape.path);
                if (staged.pulseCount() != tape.pulses || !staged.restore(size_t(tape.position), tape.remaining, tape.playing))
                    addNote(warning, "tape file " + baseName(tape.path) + " has changed since the save: the tape was rewound");
                transport = std::move(staged);
            }
        }
    }

    videoWritePending_ = false;
    deviceCycles_ = 0;
    last_irq_level_ = false;
    irq_vector_ready_ = false;
    updateBasic1DiskTraps();                       // the ROM set may differ from the one before
    basic1LoadPending_ = basic1TrapsActive_ && basic1Pending;
    return true;
}

