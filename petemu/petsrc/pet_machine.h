#ifndef PET_MACHINE_H
#define PET_MACHINE_H

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include "deftypes.h"
#include "cpu_6502.h"
#include "pet_mem.h"
#include "state_io.h"

// -----------------------------------------------------------------------------
// PetMachine
// One-stop"backplane" that owns the 6502, memory bus, and devices.
// - Exposes init/reset/run helpers
// - Installs MAME-style memory handlers that forward to PetMem
// - Ensures cpu.MEM points to the same 64K RAM image used by PetMem
// -----------------------------------------------------------------------------
class PetMachine {
public:
    PetMachine();
    ~PetMachine() = default;
    PetMachine(const PetMachine&) = delete;
    PetMachine& operator=(const PetMachine&) = delete;
    PetMachine(PetMachine&&) = delete;
    PetMachine& operator=(PetMachine&&) = delete;

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

    // Disk LOAD / VERIFY / SAVE on the original PET 2001 (BASIC 1). That ROM's
    // disk code was never finished, so the machine core does these three for
    // device 8 itself (see pet_machine.cpp). On by default; only active while the
    // installed ROM matches the BASIC 1 KERNAL byte for byte at the trapped places.
    void setBasic1DiskTraps(bool enabled);
    bool basic1DiskTrapsActive() const { return basic1TrapsActive_; }

    // Save states. Call between runCycles() calls, never from inside one.
    // saveState() appends the chunks CPU , MCH , MEM , PIA1, PIA2, VIA , CRTC, IO  ,
    // VID , TAPE and DISK; the caller writes the file header and the MACH chunk,
    // which describe things only the host knows (model, RAM size, ROM set).
    // *warning, when given, is set if something was saved that a load cannot
    // bring back exactly (the disk drive in the middle of a transfer).
    void saveState(StateWriter& w, std::string* warning = nullptr);

    // Reads every chunk of a file on a scratch machine, so a damaged file is
    // refused without touching this one. The machine must already be set up like
    // the saved one (ROM set, video geometry, RAM size); ROM bytes are not in the file.
    bool validateState(const StateFile& file, std::string& error) const;

    // validateState() and then the real load. On success *warning receives notes
    // about media that could not be attached again (tape, disk image). The tape
    // and disk image are re-attached from their saved paths, and the disk drive
    // restarts in its power-on state: open channels and transfers are lost.
    bool loadState(const StateFile& file, std::string& error, std::string* warning = nullptr);

private:
    // media false = scratch run: parse and range-check everything, touch no files.
    bool applyState(const StateFile& file, std::string& error, std::string* warning, bool media);

    // Backing 64K RAM image used as cpu.MEM (same bytes as PetMem::ram)
    //std::vector<uint8_t> ramImage;

    // Handler tables (MAME-style) with sentinel terminators
    std::vector<MemoryReadByte>  rdTbl;
    std::vector<MemoryWriteByte> wrTbl;

    // Bus devices
    PetMem memory;

    // CPU
    std::unique_ptr<cpu_6502> cpuPtr;

    // --- handlers ---
    static uint8_t  s_read8(UINT32 ofs, MemoryReadByte* h);
    static void     s_write8(UINT32 ofs, UINT8  data, MemoryWriteByte* h);

    struct VideoWrite {
        uint16_t address;
        uint8_t value;
    };
    VideoWrite videoWrite_{};
    bool videoWritePending_ = false;
    bool timeVideoWrites_ = false;
    bool timeCrtcIo_ = false;
    int deviceCycles_ = 0;

    // Build a single catch-all handler that forwards every address to PetMem
    void installFlatHandlers();

    // New: remember last IRQ level so we can detect rising edges
    bool last_irq_level_ = false;

    // BASIC 1 disk traps: enabled by the caller, active when also the ROM
    // signature matches (cached, so the per-instruction cost is one bool test).
    bool basic1TrapsEnabled_ = true;
    bool basic1TrapsActive_ = false;
    bool basic1LoadPending_ = false;   // LOAD/VERIFY on device 8 passed $F375
    void updateBasic1DiskTraps();      // recompute basic1TrapsActive_
    void basic1DiskTrap();             // check the PC before each instruction

    // Keep cpu.MEM (ramImage) in sync with PetMems RAM + ROM overlay for cases
    // when CPU reads/writes MEM directly (reset vector, stack, etc.).
   // void syncRomIntoRamImage(uint16_t base, size_t size, const uint8_t* data);
};

#endif // PET_MACHINE_H
