#pragma once
// ieee_helpers.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include "pet_machine.h"  // for g_pet access or pass a PetMachine&

static bool read_all(const std::string& path, std::vector<uint8_t>& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return !out.empty();
}

// Push a PRG into the IEEE "device" so a normal LOAD will fetch it.
static bool LoadPrgIntoIEEE(PetMachine& m, const std::string& prgPath, uint16_t fallbackAddr = 0x0801)
{
	std::vector<uint8_t> file;
	if (!read_all(prgPath, file)) {
		std::cerr << "LoadPrgIntoIEEE: can't read " << prgPath << "\n";
		return false;
	}

	uint16_t addr = fallbackAddr;
	size_t payload_off = 0;
	if (file.size() >= 2) {
		addr = static_cast<uint16_t>(file[0]) | (static_cast<uint16_t>(file[1]) << 8);
		payload_off = 2;
	}

	std::vector<uint8_t> payload(file.begin() + payload_off, file.end());

	// This lands in Pet2001IO -> PetIEEE::ieeeLoadData(...)
	m.bus().io().ieeeLoadData(addr, payload);

	std::cout << "[IEEE] Primed " << payload.size() << " bytes at $" << std::hex << addr << std::dec
		<< " — now type:  LOAD\"\",8  then  RUN\n";
	return true;
}
