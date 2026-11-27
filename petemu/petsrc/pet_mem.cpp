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
{
	// Initialize the full 4 KB screen window, including the non-visible tail.
	// A later switch to 8032 (2 KB) or 8296 (4 KB) exposes the upper part;
	// leaving it zero-filled briefly displays '@' characters before the ROM
	// clears the screen.
	for (uint16_t a = VIDRAM_ADDR; a < VIDRAM_ADDR + 0x1000; ++a) {
		ram[a] = 0x20;
	}

	// configuredRamBytes = 64 * 1024; // informational
	irq_line = false;
}

void PetMem::reset()
{
	// Do NOT clear RAM on reset; PET KERNAL init decides cold/warm behavior.
	videoUnit.reset();
	// Screen SRAM survives reset. Restore the renderer's copy, including the
	// non-visible tail that a shifted CRTC display may expose.
	for (int offset = 0; offset <= screenMask_; ++offset)
		videoUnit.write(offset, ram[VIDRAM_ADDR + offset]);
	ieeeUnit.reset();
	ioUnit.reset();
	irq_line = false;
	expansionCtrl_ = 0;   // the $FFF0 register clears on reset
}

int PetMem::expansionOffset(uint16_t addr, bool& writeProtected) const
{
	// Bit 7 enables the expansion over $8000-$FFFF; bits 6 and 5 keep the I/O
	// page and the screen memory visible through it.
	if (!expansionPresent_ || !(expansionCtrl_ & 0x80) || addr < 0x8000) return -1;
	if ((expansionCtrl_ & 0x40) && addr >= 0xE800 && addr <= 0xEFFF) return -1;
	if ((expansionCtrl_ & 0x20) && addr <= 0x8FFF) return -1;
	const bool high = addr >= 0xC000;   // $C000-$FFFF: block 2/3, else block 0/1
	const int block = high ? ((expansionCtrl_ & 0x08) ? 3 : 2) : ((expansionCtrl_ & 0x04) ? 1 : 0);
	writeProtected = (expansionCtrl_ & (high ? 0x02 : 0x01)) != 0;
	return block * 0x4000 + (addr & 0x3FFF);
}

uint8_t PetMem::readByte(uint16_t addr)
{
	// Expansion RAM wins over I/O, ROM and screen decoding.
	bool protect = false;
	const int exp = expansionOffset(addr, protect);
	if (exp >= 0) return expRam_[exp];

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
	// CRTC boards decode screen SRAM only at $8000-$87FF. The 4032's
	// 1 KB SRAM mirrors once in that range, and the 8032 has a full 2 KB.
	// The 8296 has 4 KB, so nothing is open bus there.
	if ((addr & 0xF000) == 0x8000) {
		if (screenOpenBus(addr)) return 0xFF;
		return ram[0x8000 | (addr & screenMask_)];
	}

	// ---------------- Base RAM (obey configured size) --------------
	if (addr < configuredRamBytes) {
		return ram[addr];
	}

	// Unmapped RAM -> open bus behavior (PET typically reads 0xFF)
	return 0xFF;
}

uint8_t PetMem::peekByte(uint16_t addr) const
{
	bool protect = false;
	const int exp = expansionOffset(addr, protect);
	if (exp >= 0) return expRam_[exp];
	if (inRange(addr, PIA1_BASE, PIA1_END) ||
		inRange(addr, PIA2_BASE, PIA2_END) ||
		inRange(addr, VIA_BASE, VIA_END) ||
		addr == 0xE880 || addr == 0xE881)
		return 0xFF;   // device registers: reading them is not side-effect free
	if (romMask[addr]) return rom[addr];
	if ((addr & 0xF000) == 0x8000) {
		if (screenOpenBus(addr)) return 0xFF;
		return ram[0x8000 | (addr & screenMask_)];
	}
	return addr < configuredRamBytes ? ram[addr] : 0xFF;
}

void PetMem::writeByte(uint16_t addr, uint8_t val)
{
	// The control register loads first, so the byte itself is then routed by
	// the new mapping (it may land in expansion RAM, or be an ignored ROM write).
	if (addr == 0xFFF0 && expansionPresent_) expansionCtrl_ = val;

	// Expansion RAM wins over I/O, ROM and screen decoding.
	bool protect = false;
	const int exp = expansionOffset(addr, protect);
	if (exp >= 0) {
		if (!protect) expRam_[exp] = val;
		return;
	}

	// ---------------- Screen RAM window ($8000-$8FFF) ----------------
	if ((addr & 0xF000) == 0x8000) {
		if (screenOpenBus(addr)) return;
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
	else if (expansionPresent_ && addr == 0xFFF0) {
		// Banking register write: not a stray ROM write, so not logged.
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

void PetMem::saveState(StateWriter& w) const
{
	w.u32(static_cast<uint32_t>(configuredRamBytes));
	w.u16(screenMask_);
	w.flag(expansionPresent_);
	w.u8(expansionCtrl_);
	w.flag(irq_line);
	w.bytes(ram.data(), ram.size());
	if (expansionPresent_) w.bytes(expRam_.data(), expRam_.size());
}

bool PetMem::loadState(StateReader& r)
{
	// Small fields first, so the lengths can be checked before anything is written.
	const uint32_t configured = r.u32();
	const uint16_t mask = r.u16();
	const bool expansion = r.flag();
	const uint8_t ctrl = r.u8();
	const bool irq = r.flag();
	if (r.failed() || configured > 65536 || !(mask == 0x03FF || mask == 0x07FF || mask == 0x0FFF) ||
		r.remaining() < ram.size() + (expansion ? expRam_.size() : 0)) {
		r.fail();
		return false;
	}
	r.bytes(ram.data(), ram.size());   // the CPU's MEM pointer is this vector: fill it in place
	if (expansion) r.bytes(expRam_.data(), expRam_.size());
	configuredRamBytes = configured;
	screenMask_ = mask;
	expansionPresent_ = expansion;
	expansionCtrl_ = expansion ? ctrl : 0;
	irq_line = irq;
	return true;
}

// CRC over which addresses are ROM and what they hold, so a different ROM set (or the
// same files at other addresses) gives a different value.
uint32_t PetMem::romChecksum() const
{
	uint32_t crc = 0;
	for (size_t a = 0; a < rom.size(); ++a) {
		const uint8_t cell[2] = { romMask[a], romMask[a] ? rom[a] : uint8_t(0) };
		crc = stateCrc32(cell, 2, crc);
	}
	return crc;
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
