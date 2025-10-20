#ifndef PET_MACHINE_H
#define PET_MACHINE_H

#include <cstdint>
#include <vector>
#include <string>
#include "deftypes.h"
#include "cpu_6502.h"
#include "pet_mem.h"

// -----------------------------------------------------------------------------
// PetMachine
// One-stop “backplane” that owns the 6502, memory bus, and devices.
// - Exposes init/reset/run helpers
// - Installs MAME-style memory handlers that forward to PetMem
// - Ensures cpu.MEM points to the same 64K RAM image used by PetMem
// -----------------------------------------------------------------------------
class PetMachine {
public:
    PetMachine();
    ~PetMachine() = default;

    // Provide PET character ROMs to the text renderer (1024 bytes each).
    void setVideoCharsets(const uint8_t* charset1, const uint8_t* charset2);

    // Load PET ROM blobs at the desired base addresses (returns false on error).
    bool loadRom(const uint8_t* data, size_t size, uint16_t base);
    bool loadRomFromFile(const std::string& path, uint16_t base);

    // Reset machine + CPU.
    void reset();

    // Execute N CPU cycles; returns cycles actually executed.
    int runCycles(int cycles);

    // Tick devices once per CPU cycle (already done by runCycles via stepCycle()).
    // Expose in case you drive CPU yourself.
    void stepCycle();

    // deliver IRQs only after KERNAL sets $0090/$0091
    bool irq_vector_ready_ = false;

    // Expose subsystems if needed
    cpu_6502& cpu() { return *cpuPtr; }
    PetMem& bus() { return memory; }
    Pet2001Video& video() { return memory.video(); }
    // Optional: direct access to consolidated I/O core
    Pet2001IO& io() { return memory.io(); }

private:
    // Backing 64K RAM image used as cpu.MEM (same bytes as PetMem::ram)
    //std::vector<uint8_t> ramImage;

    // Handler tables (MAME-style) with sentinel terminators
    std::vector<MemoryReadByte>  rdTbl;
    std::vector<MemoryWriteByte> wrTbl;

    // Bus devices
    PetMem memory;

    // CPU
    cpu_6502* cpuPtr = nullptr;

    // --- handlers ---
    static uint8_t  s_read8(UINT32 ofs, MemoryReadByte* h);
    static void     s_write8(UINT32 ofs, UINT8  data, MemoryWriteByte* h);

    // Build a single catch-all handler that forwards every address to PetMem
    void installFlatHandlers();

    // Keep cpu.MEM (ramImage) in sync with PetMem’s RAM + ROM overlay for cases
    // when CPU reads/writes MEM directly (reset vector, stack, etc.).
   // void syncRomIntoRamImage(uint16_t base, size_t size, const uint8_t* data);
};

#endif // PET_MACHINE_H
