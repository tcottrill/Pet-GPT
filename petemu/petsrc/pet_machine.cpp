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

    cpuPtr = new cpu_6502(memory.ramData(), rdTbl.data(), wrTbl.data(), 0xFFFF, 0);

    // Configure PET timing: 1.000 MHz CPU, 60 Hz SYNC, ~30% low pulse
    memory.io().setCpuClockHz(1'000'000.0);
    memory.io().setRefreshHz(60.0);
    memory.io().setSyncDutyLow(5000.0 / 16667.0);
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
    std::ifstream f(path, std::ios::binary);
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
  
    return true;
}

void PetMachine::reset()
{
    // PetMem::reset() leaves RAM as-is (video screen prefilled with spaces on cold start).
    cpuPtr->reset6502();  // CPU reads reset vector from MEM[$FFFC/$FFFD]
    memory.reset();       // reset I/O + video
    irq_vector_ready_ = false;   // re-arm guardrail on reset
}

int PetMachine::runCycles(int cycles)
{
    int executed = 0;
    while (executed < cycles)
    {
        const int c = cpuPtr->step6502();

        // Advance devices for each consumed CPU cycle
        for (int i = 0; i < c; ++i) {
            stepCycle();
        }

        // Guardrail: don't deliver IRQs until RAM IRQ vector $0090/$0091 is initialized
        if (!irq_vector_ready_) {
            const uint8_t lo = memory.readByte(0x0090);
            const uint8_t hi = memory.readByte(0x0091);
            irq_vector_ready_ = ((lo | hi) != 0);
        }

        // Deliver level-held IRQs only after the vector is ready
        if (irq_vector_ready_ && memory.irqLevel()) {
            if (!logged) { LOG_INFO("First IRQ -> CPU"); logged = true; }
            cpuPtr->irq6502(); // core will ignore if I-flag is set
        }

        executed += c;
    }
    return executed;
}

void PetMachine::stepCycle()
{
    memory.stepCycle(); // Pet2001IO: PIA/VIA timers & CB2/handshakes in lockstep
}

