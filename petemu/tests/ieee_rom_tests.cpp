// Drive behaviour as BASIC sees it: boots a real ROM set, types commands and
// reads the screen. Needs ROMs, so CTest only runs it when PET_TEST_ROM_DIR
// is set. Usage: ieee_rom_tests <rom root, holding pet2001n/ and pet4000-9/>
#include "pet_machine.h"
#include "pet_roms.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
namespace Log {
bool open(const std::string&) { return true; }
void close() {}
void write(Level, const char*, const char*, int, const char*, ...) {}
void setLevel(Level) {}
void setConsoleOutputEnabled(bool) {}
}
static int checks=0, failures=0;
static void screen(PetMachine& m) {
    for(int y=0;y<25;++y) { for(int x=0;x<40;++x) {
        int c=m.bus().readByte(0x8000+y*40+x)&127;
        putchar(c<32 ? c+64 : c);
    } putchar('\n'); }
}
static bool shows(PetMachine& m, const char* text) {
    for(int a=0;a<1000-int(strlen(text));++a) {
        bool match=true;
        for(size_t n=0;n<strlen(text);++n)
            if((m.bus().readByte(uint16_t(0x8000+a+n))&127)!=(uint8_t(text[n])&63)) match=false;
        if(match) return true;
    }
    return false;
}
#define CHECK_SHOWS(m,text) do { ++checks; if (!shows(m,text)) { ++failures; printf("FAIL %d: screen lacks \"%s\"\n",__LINE__,text); screen(m); } } while(0)
#define CHECK_LACKS(m,text) do { ++checks; if (shows(m,text)) { ++failures; printf("FAIL %d: screen has \"%s\"\n",__LINE__,text); screen(m); } } while(0)
// Types through the KERNAL keyboard buffer, then lets the command finish.
static void command(PetMachine& m, const char* text) {
    while (*text) {
        for (int n=0;n<100 && m.bus().readByte(0x9e);++n) m.runCycles(20000);
        m.bus().writeByte(0x26f,uint8_t(*text++)); m.bus().writeByte(0x9e,1);
        m.runCycles(30000);
    }
    m.runCycles(4000000);
}
static void run_model(const std::string& romRoot, bool basic4, const std::filesystem::path& files) {
    printf("-- %s\n", basic4 ? "BASIC 4" : "BASIC 2");
    PetMachine m;
    const bool loaded = basic4 ? load_pet4_romset(m, romRoot + "/pet4000-9/", false)
                               : load_pet2_romset(m, romRoot + "/pet2001n/", true);
    ++checks; if (!loaded) { ++failures; printf("FAIL: ROM set missing under %s\n", romRoot.c_str()); return; }
    m.bus().setRamSize(32768);
    m.bus().io().setIeeeHostRoot(files.u8string());
    m.reset(); m.runCycles(3000000);

    // "$0" is the dual-drive form of the directory; it listed no files.
    command(m, "\x93LOAD\"$0\",8\r"); command(m, "LIST\r");
    CHECK_SHOWS(m, "HELLO");

    // A missing file must be an error, not a silent one-byte "program".
    command(m, "\x93LOAD\"NOSUCH\",8\r");
    if (basic4) CHECK_SHOWS(m, "FILE NOT FOUND");
    else { // BASIC 2's disk LOAD retries a silent drive until STOP is pressed
        for (int n=0;n<2000;++n) { m.bus().writeByte(0x9b, 0xef); m.runCycles(1000); }
        CHECK_SHOWS(m, "BREAK");
    }

    // A real load still works afterwards.
    command(m, "\x93LOAD\"HELLO\",8\r"); command(m, "LIST\r");
    CHECK_SHOWS(m, "PRINT\"HI\"");

    // Saving and loading a program back works (the listener path).
    command(m, "\x93NEW\r"); command(m, "10 PRINT\"ROUND\"\r");
    command(m, "SAVE\"T1\",8\r"); command(m, "NEW\r");
    command(m, "\x93LOAD\"T1\",8\r"); command(m, "LIST\r");
    CHECK_SHOWS(m, "PRINT\"ROUND\"");

    // Nothing answers for device 4 (the drive is device 8): the PET sees both
    // handshake lines released and sets ST bit 7, and an OPEN with a secondary
    // address and name fails with DEVICE NOT PRESENT.
    command(m, "\x93OPEN4,4:PRINT#4,\"X\":PRINT\"ST=\";ST\r");
    CHECK_SHOWS(m, "ST=-128");
    command(m, "\x93OPEN5,4,7,\"X\"\r");
    CHECK_SHOWS(m, "DEVICE NOT PRESENT");
    command(m, "CLOSE4:CLOSE5\r");
    // ...and the drive is not left stuck serving the previous file.
    command(m, "\x93LOAD\"$\",8\r"); command(m, "LIST\r");
    CHECK_SHOWS(m, "BLOCKS FREE");

    // Closing channel 15 must leave the status readable (it returned the
    // bytes of the last program, or timed out).
    command(m, "\x93NEW\r");
    command(m, "1 OPEN15,8,15:CLOSE15:OPEN15,8,15\r");
    command(m, "2 INPUT#15,A,B$:PRINT\"ST=\"A;B$:CLOSE15\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "ST= 0 OK");
    CHECK_LACKS(m, "ERROR");

    // Data files on the folder drive: write two values, close, reopen, read
    // them back. This used to write nothing and still report 00,OK.
    std::filesystem::remove(files / "SCORES.seq");
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"SCORES,S,W\":PRINT#2,123\",\"456:CLOSE2\r");
    command(m, "20 OPEN2,8,2,\"SCORES,S,R\":INPUT#2,A,B:CLOSE2\r");
    command(m, "30 PRINT\"GOT\";A;B\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "GOT 123  456");
    ++checks; if (!std::filesystem::exists(files / "SCORES.seq")) { ++failures; printf("FAIL: SCORES.seq not written\n"); }
    // The folder listing shows it as a SEQ file.
    command(m, "\x93LOAD\"$\",8\r"); command(m, "LIST\r");
    CHECK_SHOWS(m, "\"SCORES\"");
    CHECK_SHOWS(m, "SEQ");
    // Without "@" a second write is refused (63) and the old data survives.
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"SCORES,S,W\":PRINT#2,7:CLOSE2:OPEN15,8,15:INPUT#15,E,E$:CLOSE15\r");
    command(m, "20 PRINT\"E=\";E;E$\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "E= 63 FILE EXISTS");
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"SCORES,S,R\":INPUT#2,A,B:CLOSE2:PRINT\"STILL\";A;B\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "STILL 123  456");

    // PRINT# line endings are the ROM's own: BASIC 2 always sends CR LF, BASIC 4
    // sends CR alone for file numbers below 128. The drive stores what it is sent.
    std::filesystem::remove(files / "ENDING.seq");
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"ENDING,S,W\":PRINT#2,7:CLOSE2\r");
    command(m, "RUN\r");
    {
        std::ifstream in(files / "ENDING.seq", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ++checks; if (bytes != (basic4 ? " 7 \r" : " 7 \r\n")) { ++failures; printf("FAIL %d: PRINT# wrote %zu bytes\n", __LINE__, bytes.size()); }
    }

    // One record per INPUT#: the first INPUT# hands the bus back with the second
    // record unread. BASIC 2 hung here (and STOP did not break it). CHR$(13);
    // keeps BASIC 2's line feed out of the file. Kept last: a hang would wreck
    // every check after it.
    std::filesystem::remove(files / "RECS.seq");
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"RECS,S,W\":PRINT#2,123;CHR$(13);:PRINT#2,456;CHR$(13);:CLOSE2\r");
    command(m, "20 OPEN2,8,2,\"RECS,S,R\":INPUT#2,A:INPUT#2,B:CLOSE2\r");
    command(m, "30 PRINT\"TWO\";A;B\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "TWO 123  456");
    // Byte at a time, as a GET# loop reads: every byte arrives, none is lost.
    command(m, "\x93NEW\r");
    command(m, "10 OPEN2,8,2,\"RECS,S,R\":FORI=1TO12:GET#2,A$:S=S+ASC(A$+CHR$(0)):NEXT\r");
    command(m, "20 PRINT\"SUM\";S;ST:CLOSE2\r");
    command(m, "RUN\r");
    CHECK_SHOWS(m, "SUM 463  64");   // " 123 " CR " 456 " CR; ST 64 = end of file
}

// The original PET 2001 (BASIC 1). Its own disk LOAD/SAVE never worked, so the
// machine core traps them for device 8 (see PetMachine). BASIC 1 keeps the
// keyboard buffer at $020F and its count at $020D.
static void command1(PetMachine& m, const char* text) {
    while (*text) {
        for (int n=0;n<100 && m.bus().readByte(0x20d);++n) m.runCycles(20000);
        m.bus().writeByte(0x20f,uint8_t(*text++)); m.bus().writeByte(0x20d,1);
        m.runCycles(30000);
    }
    m.runCycles(4000000);
}
static bool boot_basic1(PetMachine& m, const std::string& dir, const std::filesystem::path& files) {
    if (!load_pet1_romset(m, dir)) return false;
    m.bus().setRamSize(8192);   // as the application configures this model
    m.bus().io().setIeeeHostRoot(files.u8string());
    m.reset(); m.runCycles(3000000);
    return true;
}
static std::string file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void run_basic1(const std::string& romRoot, const std::filesystem::path& files) {
    const std::string dir = romRoot + "/pet2001/";
    if (!pet_romset_present(1, dir)) { printf("-- BASIC 1: ROM set absent under %s, skipped\n", dir.c_str()); return; }
    printf("-- BASIC 1\n");
    {
        PetMachine m;
        ++checks; if (!boot_basic1(m, dir, files)) { ++failures; printf("FAIL: BASIC 1 ROM set did not load\n"); return; }
        // The directory loads and LOAD ends by itself (it used to hang at LOADING).
        command1(m, "\x93LOAD\"$\",8\r");
        CHECK_SHOWS(m, "LOADING");
        CHECK_SHOWS(m, "READY.");
        command1(m, "LIST\r");
        CHECK_SHOWS(m, "HELLO");
        // A missing file is the ROM's own error.
        command1(m, "\x93LOAD\"NOSUCH\",8\r");
        CHECK_SHOWS(m, "FILE NOT FOUND");
        // A real program loads, lists and runs.
        command1(m, "\x93LOAD\"HELLO\",8\r"); command1(m, "LIST\r");
        CHECK_SHOWS(m, "PRINT\"HI\"");
        command1(m, "\x93RUN\r");
        CHECK_SHOWS(m, "HI    ");   // the output line ("SEARCHING" also holds "HI")
        // The end-of-program pointer follows the loaded bytes: a new variable
        // goes after the program, not over it.
        command1(m, "\x93X=7:PRINT X+1\r");
        CHECK_SHOWS(m, " 8");
        command1(m, "\x93LIST\r");
        CHECK_SHOWS(m, "PRINT\"HI\"");
        // A file that would run past the end of RAM (8 KB here) is refused
        // without writing anything.
        {
            std::string big("\xf0\x1f", 2); big += std::string(32, '\xaa');
            std::ofstream out(files / "big.prg", std::ios::binary); out << big;
        }
        command1(m, "\x93LOAD\"BIG\",8\r");
        CHECK_SHOWS(m, "LOAD ERROR");
        ++checks; if (m.bus().readByte(0x1ff0) == 0xaa) { ++failures; printf("FAIL %d: refused LOAD wrote to RAM\n", __LINE__); }
        command1(m, "\x93LIST\r");
        CHECK_SHOWS(m, "PRINT\"HI\"");
        std::filesystem::remove(files / "big.prg");
    }
    {
        PetMachine m;
        ++checks; if (!boot_basic1(m, dir, files)) { ++failures; printf("FAIL: BASIC 1 ROM set did not load\n"); return; }
        std::filesystem::remove(files / "T1.prg");
        command1(m, "\x93NEW\r"); command1(m, "10 PRINT\"ROUND\"\r");
        command1(m, "SAVE\"T1\",8\r");
        // Load address $0401, then the line: link $040E, line 10, PRINT token,
        // "ROUND" in quotes, end of line, and the 00 00 end of program. BASIC 1
        // puts its start of variables ($7C) one byte past that ($0411, where
        // later ROMs have $0410), and SAVE writes up to $7C, so the file ends
        // with that one spare byte, as a BASIC 1 tape SAVE does.
        std::string want("\x01\x04" "\x0e\x04\x0a\x00" "\x99\"ROUND\"\x00" "\x00\x00", 17);
        ++checks; if (m.bus().readByte(0x7c) != 0x11 || m.bus().readByte(0x7d) != 0x04) { ++failures; printf("FAIL %d: BASIC 1 end of program is $%02X%02X\n", __LINE__, m.bus().readByte(0x7d), m.bus().readByte(0x7c)); }
        want += char(m.bus().readByte(0x0410));
        const std::string got = file_bytes(files / "T1.prg");
        ++checks; if (got != want) {
            ++failures; printf("FAIL %d: T1.prg has %zu bytes:", __LINE__, got.size());
            for (unsigned char c : got) printf(" %02X", c);
            printf("\n");
        }
        command1(m, "\x93NEW\r");
        command1(m, "\x93LOAD\"T1\",8\r"); command1(m, "LIST\r");
        CHECK_SHOWS(m, "PRINT\"ROUND\"");
        command1(m, "\x93VERIFY\"T1\",8\r");
        CHECK_SHOWS(m, "OK");
        CHECK_LACKS(m, "ERROR");
        command1(m, "20 END\r");
        command1(m, "\x93VERIFY\"T1\",8\r");
        CHECK_SHOWS(m, "VERIFY ERROR");
        // Without "@" a second SAVE is refused (63) and the file is unchanged;
        // the drive status says so, as on later ROMs.
        command1(m, "\x93SAVE\"T1\",8\r");
        ++checks; if (file_bytes(files / "T1.prg") != want) { ++failures; printf("FAIL %d: T1.prg changed\n", __LINE__); }
        command1(m, "\x93NEW\r");
        command1(m, "10 OPEN15,8,15:INPUT#15,E,E$:CLOSE15:PRINT\"E=\";E;E$\r");
        command1(m, "RUN\r");
        CHECK_SHOWS(m, "E= 63 FILE EXISTS");
        // Data files keep working through the ROM's own bus code. A string, as
        // BASIC 1 prints a number with a cursor-right after it (" 5" $1D), which
        // its own INPUT# then rejects with ?BAD DATA, on the real machine too.
        std::filesystem::remove(files / "D.seq");
        command1(m, "\x93NEW\r");
        command1(m, "10 OPEN2,8,2,\"D,S,W\":PRINT#2,\"FIVE\":CLOSE2\r");
        command1(m, "20 OPEN2,8,2,\"D,S,R\":INPUT#2,A$:CLOSE2:PRINT\"BACK \";A$\r");
        command1(m, "RUN\r");
        CHECK_SHOWS(m, "BACK FIVE");
    }
}
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: ieee_rom_tests <rom root>\n"); return 2; }
    namespace fs = std::filesystem;
    const fs::path files = fs::temp_directory_path() / "pet-ieee-rom-test";
    fs::remove_all(files); fs::create_directories(files);
    {
        // 10 PRINT"HI"
        const unsigned char prg[] = {0x01,0x04, 0x0b,0x04, 0x0a,0x00, 0x99,0x22,'H','I',0x22,0x00, 0x00,0x00};
        std::ofstream out(files / "hello.prg", std::ios::binary);
        out.write(reinterpret_cast<const char*>(prg), sizeof(prg));
    }
    std::string root = argv[1];
    run_model(root, false, files);
    run_model(root, true, files);
    run_basic1(root, files);
    fs::remove_all(files);
    printf("IEEE ROM tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
