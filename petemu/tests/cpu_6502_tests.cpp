#include "cpu_6502.h"
#include "sys_log.h"
#include <cstdio>
#include <cstring>
namespace Log { void write(Level, const char*, const char*, int, const char*, ...) {} }
static unsigned char mem[65536];
static MemoryReadByte rd[] = {{UINT32(-1),0,nullptr,nullptr}};
static MemoryWriteByte wr[] = {{UINT32(-1),0,nullptr,nullptr}};
static int failures;
static void check(const char* name, unsigned got, unsigned expected) {
    printf("%s: got %04X expected %04X %s\n",name,got,expected,got==expected?"PASS":"FAIL");
    failures += got != expected;
}
// Optional: cpu_6502_tests.exe <NMOS functional ROM> <CMOS extended ROM>.
// ROMs remain external; normal regression runs do not require them.
static void klaus(const char* path, CpuModel model, unsigned success) {
    FILE* file = nullptr;
    if (fopen_s(&file,path,"rb") != 0) {
        printf("Cannot open Klaus ROM: %s\n",path); ++failures; return;
    }
    const auto bytes = fread(mem,1,sizeof(mem),file);
    fclose(file);
    if (bytes != sizeof(mem)) {
        printf("Klaus ROM must contain 65536 bytes\n"); ++failures; return;
    }
    cpu_6502 cpu(mem,rd,wr,0xffff,0,model);
    cpu.reset6502(); cpu.PC=0x0400;
    for (unsigned i=0; i<100000000; ++i) {
        const auto previous = cpu.PC;
        cpu.step6502();
        if (cpu.PC == previous) {
            check("Klaus terminal PC",cpu.PC,success); return;
        }
    }
    printf("Klaus instruction limit reached at %04X\n",cpu.PC); ++failures;
}
int main(int argc, char** argv) {
    cpu_6502 n(mem,rd,wr,0xffff,0);
    n.PC=0x200; n.P=0x29; n.A=0x79; mem[0x200]=0x69; mem[0x201]=0;
    n.step6502(); check("NMOS decimal ADC N/V",n.P&0xc0,0xc0);
    n.PC=0x200; n.P=0x24; n.X=1; mem[0x200]=0x1c; mem[0x201]=0xff; mem[0x202]=0x20;
    check("NMOS abs,X NOP page crossing cycles",n.step6502(),5);
    n.PC=0x200; n.P=0x20; mem[0x200]=0x02; mem[0xfffe]=0; mem[0xffff]=0x40;
    n.step6502(); n.irq6502(); n.step6502(); check("NMOS JAM persists across IRQ",n.PC,0x200);
    cpu_6502 c(mem,rd,wr,0xffff,0,CPU_CMOS_65C02);
    c.PC=0x200; c.P=0x24; mem[0x200]=0xda;
    check("CMOS PHX cycles",c.step6502(),3);
    c.PC=0x200; c.P=0x28; c.irq6502(); c.step6502(); check("CMOS IRQ clears D",c.P&8,0);
    cpu_6502 nes(mem,rd,wr,0xffff,0,CPU_NES_2A03);
    nes.PC=0x200; nes.P=0x2d; nes.A=0x10; mem[0x200]=0xeb; mem[0x201]=1;
    nes.step6502(); check("NES unofficial SBC ignores decimal",nes.A,0x0f);
    // NMOS decimal flags differ from both binary and final BCD result flags.
    const struct { unsigned a, m, carry, result, flags; } adc_cases[] = {
        {0x79,0x00,1,0x80,0xc0}, {0x50,0x50,0,0x00,0xc1},
        {0x99,0x01,0,0x00,0x81}, {0x00,0x00,0,0x00,0x02},
        {0x0f,0x0f,1,0x15,0x00}
    };
    for (const auto& t : adc_cases) {
        cpu_6502 cpu(mem,rd,wr,0xffff,0);
        cpu.PC=0x200; cpu.P=0x28|t.carry; cpu.A=t.a;
        mem[0x200]=0x69; mem[0x201]=t.m;
        cpu.step6502(); check("decimal ADC result",cpu.A,t.result);
        check("decimal ADC flags",cpu.P&0xc3,t.flags);
    }
    // RRA performs ADC after rotating memory, including decimal flags.
    cpu_6502 r(mem,rd,wr,0xffff,0);
    r.PC=0x200; r.P=0x28; r.A=0x79; mem[0x200]=0x67; mem[0x201]=0x10; mem[0x10]=1;
    r.step6502(); check("decimal RRA flags",r.P&0xc3,0xc0);
    for (unsigned op : {0x1c,0x3c,0x5c,0x7c,0xdc,0xfc}) {
        cpu_6502 cpu(mem,rd,wr,0xffff,0);
        cpu.PC=0x200; cpu.P=0x24; cpu.X=1;
        mem[0x200]=op; mem[0x201]=0xff; mem[0x202]=0x20;
        check("indexed NOP crossing",cpu.step6502(),5);
        cpu.PC=0x200; mem[0x201]=0xfe;
        check("indexed NOP noncrossing",cpu.step6502(),4);
    }
    for (unsigned op : {0x02,0x12,0x22,0x32,0x42,0x52,0x62,0x72,0x92,0xb2,0xd2,0xf2}) {
        cpu_6502 cpu(mem,rd,wr,0xffff,0);
        cpu.PC=0x200; cpu.P=0x20; mem[0x200]=op;
        cpu.step6502(); cpu.nmi6502(); check("JAM ignores NMI",cpu.PC,0x200);
        check("JAM leaves stack alone",cpu.S,0xff);
        mem[0xfffc]=0; mem[0xfffd]=3; mem[0x300]=0xea;
        cpu.reset6502(); cpu.step6502(); check("reset releases JAM",cpu.PC,0x301);
    }
    for (CpuModel model : {CPU_NMOS_6502,CPU_CMOS_65C02}) {
        cpu_6502 cpu(mem,rd,wr,0xffff,0,model);
        cpu.PC=0x200; cpu.P=0x28; cpu.nmi6502();
        check("NMI decimal mode",cpu.P&8,model==CPU_CMOS_65C02?0:8);
        check("NMI stacked decimal flag",mem[0x1fd]&8,8);
    }
    for (unsigned op : {0x5a,0xda,0x7a,0xfa}) {
        cpu_6502 cpu(mem,rd,wr,0xffff,0,CPU_CMOS_65C02);
        cpu.PC=0x200; cpu.P=0x24; mem[0x200]=op;
        check("CMOS push/pull timing",cpu.step6502(),(op==0x5a||op==0xda)?3:4);
    }
    if (argc == 3) {
        klaus(argv[1],CPU_NMOS_6502,0x3469);
        klaus(argv[2],CPU_CMOS_65C02,0x24f1);
    }
    printf("CPU regression failures: %d\n",failures);
    return failures ? 1 : 0;
}
