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
	// ---------------- I/O windows ----------------
	if (inRange(addr, PIA1_BASE, PIA1_END) ||
		inRange(addr, PIA2_BASE, PIA2_END) ||
		inRange(addr, VIA_BASE, VIA_END))
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

	// ---------------- RAM (includes video RAM reads) ----------------
	if (inRange(addr, VIDRAM_ADDR, VIDRAM_END)) {
		return ram[addr];
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
	// ---------------- Screen RAM mirror ----------------
	if (inRange(addr, VIDRAM_ADDR, VIDRAM_END)) {
		ram[addr] = val;
		if (ramMirror_ && addr < ramMirrorSize_) ramMirror_[addr] = val;

		const int vOff = static_cast<int>(addr - VIDRAM_ADDR);
		videoUnit.write(vOff, val);
		return;
	}

	// ---------------- I/O windows ----------------
	if (inRange(addr, PIA1_BASE, PIA1_END) ||
		inRange(addr, PIA2_BASE, PIA2_END) ||
		inRange(addr, VIA_BASE, VIA_END)) {
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
		// writes to ROM are ignored
		LOG_DEBUG("[ROM W ignored] %04X <- %02X", addr, val);
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