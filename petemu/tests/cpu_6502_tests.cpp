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
    if (!(file = fopen(path,"rb"))) {
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
    {
        // Portable save state: a blob loaded into a second core continues exactly like the first.
        static unsigned char memA[65536], memB[65536];
        const unsigned char program[] = {0xa9,1, 0x69,3, 0x9d,0,3, 0xe8, 0x4c,2,2};  // LDA #1: ADC #3, STA $0300,X, INX, JMP $0202
        memcpy(memA+0x200,program,sizeof program);
        memA[0x280]=0xe6; memA[0x281]=0x10; memA[0x282]=0x40;                        // IRQ: INC $10, RTI
        memA[0xfffe]=0x80; memA[0xffff]=0x02;
        cpu_6502 a(memA,rd,wr,0xffff,0), b(memB,rd,wr,0xffff,0);
        a.PC=0x200; a.P=0x20; a.S=0xff;
        for (int i=0;i<5;++i) a.step6502();
        a.irq6502(IRQ_HOLD);
        for (int i=0;i<4;++i) a.step6502();                                          // inside the IRQ handler
        uint8_t blob[cpu_6502::kStateMaxSize];
        const size_t n = a.save_state(blob,sizeof blob);
        check("state size",unsigned(n),34);
        check("state magic",blob[0]=='6'&&blob[1]=='5'&&blob[2]=='0'&&blob[3]=='2',1);
        check("state version",blob[4],1);
        check("state variant NMOS",blob[5],0);
        check("state length",blob[6]|(blob[7]<<8),26);
        check("state PC",blob[8]|(blob[9]<<8),a.PC);
        check("state IRQ line",blob[15],1);
        memcpy(memB,memA,sizeof memA);
        check("state loads",b.load_state(blob,n),1);
        check("state regs",b.PC==a.PC&&b.A==a.A&&b.X==a.X&&b.Y==a.Y&&b.S==a.S&&b.P==a.P,1);
        check("state cycle counter",b.get6502ticks(0),a.get6502ticks(0));
        bool same = true;
        for (int i=0;i<300;++i) {
            if (i==60) { a.m6502clearpendingint(); b.m6502clearpendingint(); }
            const int ca=a.step6502(), cb=b.step6502();
            same = same && ca==cb && b.PC==a.PC && b.A==a.A && b.X==a.X && b.Y==a.Y && b.S==a.S && b.P==a.P;
        }
        check("state continues identically",same,1);
        check("state memory identical",memcmp(memA,memB,sizeof memA)==0,1);
        check("state cycles identical",b.get6502ticks(0),a.get6502ticks(0));

        // A foreign emulator writes only the portable section (or a zero-length private one).
        for (int variant=0; variant<2; ++variant) {
            unsigned char foreign[26] = {'6','5','0','2',1,0, (unsigned char)(variant?18:16),0, 0x00,0x02, 0x10,0x02,0x00,0xf0,0x20, 0,
                                         0,0,0,0,0,0,0,0, 0,0};
            memset(memA+0x300,0,16);
            cpu_6502 c(memA,rd,wr,0xffff,0);
            check("portable-only blob loads",c.load_state(foreign,variant?26:24),1);
            check("portable-only PC",c.PC,0x200);
            check("portable-only SP",c.S,0xf0);
            c.step6502(); c.step6502();
            check("portable-only runs",c.A,4);
            c.step6502();
            check("portable-only store",memA[0x302],4);
            check("portable-only PC after three steps",c.PC,0x207);
        }

        // Rejected blobs leave the core untouched.
        cpu_6502 c(memA,rd,wr,0xffff,0);
        c.PC=0x1234; c.A=1; c.X=2; c.Y=3; c.S=0x44; c.P=0x25;
        auto untouched = [&]() { return c.PC==0x1234&&c.A==1&&c.X==2&&c.Y==3&&c.S==0x44&&c.P==0x25; };
        uint8_t bad[cpu_6502::kStateMaxSize];
        memcpy(bad,blob,n); bad[0]='7';
        check("wrong magic rejected",c.load_state(bad,n),0); check("wrong magic unchanged",untouched(),1);
        memcpy(bad,blob,n); bad[4]=2;
        check("unknown version rejected",c.load_state(bad,n),0); check("unknown version unchanged",untouched(),1);
        memcpy(bad,blob,n); bad[4]=0;
        check("version 0 rejected",c.load_state(bad,n),0);
        check("truncated rejected",c.load_state(blob,n-1),0); check("truncated unchanged",untouched(),1);
        check("header only rejected",c.load_state(blob,8),0);
        check("empty rejected",c.load_state(blob,0),0);
        memcpy(bad,blob,n); bad[24]=200;
        check("private length beyond blob rejected",c.load_state(bad,n),0); check("private length unchanged",untouched(),1);
        check("small output buffer",unsigned(a.save_state(bad,n-1)),0);
        check("null output buffer",unsigned(a.save_state(nullptr,64)),0);
    }
    if (argc == 3) {
        klaus(argv[1],CPU_NMOS_6502,0x3469);
        klaus(argv[2],CPU_CMOS_65C02,0x24f1);
    }
    printf("CPU regression failures: %d\n",failures);
    return failures ? 1 : 0;
}
