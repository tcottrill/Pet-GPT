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


#include "pet_mem.h"
#include <functional>
#include "sys_log.h"

PetMem::PetMem()
	: ram(65536, 0x00),
	romMask(65536, 0x00),
	rom(65536, 0xFF),
	ieeeUnit(),
	videoUnit(),
	// keysUnit(),
	ioUnit(ieeeUnit, videoUnit, [this](bool level) { this->onIrqLine(level); })
{    // Fill screen RAM with spaces on cold start 
	for (uint16_t a = VIDRAM_ADDR; a <= VIDRAM_END; ++a) {
		ram[a] = 0x20;
	}

	// configuredRamBytes = 64 * 1024; // informational
	irq_line = false;
}

void PetMem::reset()
{
	// Do NOT clear RAM on reset; PET KERNAL init decides cold/warm behavior.
	videoUnit.reset();
	ieeeUnit.reset();
	ioUnit.reset();
	irq_line = false;
}

uint8_t PetMem::readByte(uint16_t addr)
{
	if (addr == 0x0277)
	{
		LOG_DEBUG("READ KERNAL KEYBUF $0277 = %02X", ram[addr]);
	}

	if (addr == 0xE810 || addr == 0xE811)
	{
		const uint8_t lo8 = static_cast<uint8_t>(addr & 0xFF);
		const uint8_t v = ioUnit.read(lo8);
		//LOG_DEBUG("READ PIA1 port %c = %02X", (addr == 0xE810 ? 'A' : 'B'), v);
		return v; // IMPORTANT: return here so we do not read again below
	}

	// ---------------- I/O windows ----------------
	if (inRange(addr, PIA1_BASE, PIA1_END) ||
		inRange(addr, PIA2_BASE, PIA2_END) ||
		inRange(addr, VIA_BASE, VIA_END) ||
		addr == 0xE880 || addr == 0xE881)   // 6545 CRTC (8032)
	{
		// Pet2001IO expects the LOW 8 bits of the register address
		const uint8_t lo8 = static_cast<uint8_t>(addr & 0xFF);
		const uint8_t v = ioUnit.read(lo8);
		return v;
	}

	// ---------------- ROM over RAM ----------------
	if (romMask[addr]) {
		return rom[addr];
	}

	// ---------------- Screen RAM window ($8000-$8FFF) ----------------
	// The physical screen SRAM is mirrored across the 4 KB window: 1 KB
	// (mask $03FF) on 40-column PETs, 2 KB (mask $07FF) on the 8032
	// (setScreenWindow). Software stashes data at $83E8+ and probes mirrors.
	if ((addr & 0xF000) == 0x8000) {
		return ram[0x8000 | (addr & screenMask_)];
	}

	// ---------------- Base RAM (obey configured size) --------------
	if (addr < configuredRamBytes) {
		return ram[addr];
	}

	// Unmapped RAM -> open bus behavior (PET typically reads 0xFF)
	return 0xFF;
}

void PetMem::writeByte(uint16_t addr, uint8_t val)
{
	// ---------------- Screen RAM window ($8000-$8FFF) ----------------
	// The mirrors alias the same SRAM: 1 KB (mask $03FF) on 40-column PETs,
	// 2 KB (mask $07FF) on the 8032. The renderer ignores offsets past the
	// visible cells (24-byte tail at 40 cols, 48 bytes at 80).
	if ((addr & 0xF000) == 0x8000) {
		const uint16_t eff = (uint16_t)(0x8000 | (addr & screenMask_));
		ram[eff] = val;
		if (ramMirror_ && eff < ramMirrorSize_) ramMirror_[eff] = val;

		const int vOff = static_cast<int>(eff - VIDRAM_ADDR);
		videoUnit.write(vOff, val);   // out-of-range offsets ignored by video
		return;
	}

	// ---------------- I/O windows ----------------
	if (inRange(addr, PIA1_BASE, PIA1_END) ||
		inRange(addr, PIA2_BASE, PIA2_END) ||
		inRange(addr, VIA_BASE, VIA_END) ||
		addr == 0xE880 || addr == 0xE881) {   // 6545 CRTC (8032)
		const uint8_t lo8 = static_cast<uint8_t>(addr & 0xFF);
		ioUnit.write(lo8, val);
		return;
	}

	// ---------------- Normal RAM / ROM ----------------
	if (!romMask[addr]) {
		// Only honor writes inside configured base RAM
		if (addr < configuredRamBytes) {
			ram[addr] = val;
			if (ramMirror_ && addr < ramMirrorSize_) ramMirror_[addr] = val;
		}
		else {
			// ignore writes to non-existent RAM
		}
	}
	else {
		// Writes to ROM are ignored (open bus), same as real hardware.
		// Rate-limited: software legitimately hammers ROM space - e.g. the
		// Infocom interpreters toggle $FFF0 (the 8096/8296 banking register,
		// absent on this machine) thousands of times per second.
		static int romWriteLogs = 24;
		if (romWriteLogs > 0) {
			LOG_DEBUG("[ROM W ignored] %04X <- %02X", addr, val);
			if (--romWriteLogs == 0)
				LOG_DEBUG("[ROM W ignored] (further ROM-write messages suppressed)");
		}
	}
}

void PetMem::stepCycle()
{
	// Advance I/O devices (timers, IRQs, IEEE handshakes)
	ioUnit.cycle();
}

void PetMem::setRamSize(size_t bytes)
{
	configuredRamBytes = bytes;
	if (bytes > 65536) bytes = 65536;
	configuredRamBytes = bytes;
	LOG_INFO("Configured PET RAM: %zu bytes", configuredRamBytes);
}

void PetMem::installROM(uint16_t base, const uint8_t* data, size_t size)
{
	if (!data || size == 0) return;
	uint32_t end = static_cast<uint32_t>(base) + static_cast<uint32_t>(size);
	if (end > 65536u) end = 65536u;

	for (uint32_t a = base, i = 0; a < end; ++a, ++i) {
		rom[a] = data[i];
		romMask[a] = 1;
	}
	LOG_INFO("ROM installed @ $%04X [%zu bytes]", base, size);
}

void PetMem::clearROM(uint16_t base, size_t size)
{
	uint32_t end = static_cast<uint32_t>(base) + static_cast<uint32_t>(size);
	if (end > 65536u) end = 65536u;

	for (uint32_t a = base; a < end; ++a) {
		romMask[a] = 0;
	}
	LOG_INFO("ROM unmapped @ $%04X [%zu bytes]", base, size);
}