// SM83 interpreter: gb_cpu_step @ 0x710008e7f0 dispatches through a 256-entry handler table
// (@ 0x71001b2018, 245 distinct handlers); CB-prefixed opcodes go through one switch @ 0x7100092190.
// Each handler adds its fixed cost to cpu.ticks (4 ticks = 1 M-cycle).
#include "gb/Core.h"
#include <cstdio>

namespace gb {
namespace {

uint16_t Pair(uint8_t hi, uint8_t lo) { return static_cast<uint16_t>(hi << 8 | lo); }
uint16_t BC() { return Pair(cpu.b, cpu.c); }
uint16_t DE() { return Pair(cpu.d, cpu.e); }
uint16_t HL() { return Pair(cpu.h, cpu.l); }
void SetBC(uint16_t v) { cpu.b = static_cast<uint8_t>(v >> 8); cpu.c = static_cast<uint8_t>(v); }
void SetDE(uint16_t v) { cpu.d = static_cast<uint8_t>(v >> 8); cpu.e = static_cast<uint8_t>(v); }
void SetHL(uint16_t v) { cpu.h = static_cast<uint8_t>(v >> 8); cpu.l = static_cast<uint8_t>(v); }

bool FlagZ() { return (cpu.f & 0x80) != 0; }
bool FlagC() { return (cpu.f & 0x10) != 0; }

// operand index 0-7: B C D E H L (HL) A
uint8_t* RegRef(uint32_t index) {
    switch (index & 7) {
    case 0: return &cpu.b;
    case 1: return &cpu.c;
    case 2: return &cpu.d;
    case 3: return &cpu.e;
    case 4: return &cpu.h;
    case 5: return &cpu.l;
    case 7: return &cpu.a;
    default: return nullptr;
    }
}

uint8_t Fetch8() {
    const uint8_t v = Read8(cpu.pc);
    ++cpu.pc;
    return v;
}

uint16_t Fetch16() {
    const uint16_t v = Read16(cpu.pc);
    cpu.pc = static_cast<uint16_t>(cpu.pc + 2);
    return v;
}

void Push16(uint32_t value) {
    const uint32_t sp = cpu.sp;
    cpu.sp = static_cast<uint16_t>(sp - 2);
    Write16(sp - 2, value);
}

uint16_t Pop16() {
    const uint16_t v = Read16(cpu.sp);
    cpu.sp = static_cast<uint16_t>(cpu.sp + 2);
    return v;
}

void Inc8(uint8_t& r) {
    const uint8_t old = r;
    r = static_cast<uint8_t>(old + 1);
    uint8_t f = static_cast<uint8_t>((cpu.f & 0x1f) | (r == 0 ? 0x80 : 0));
    if ((old & 0xf) == 0xf) f |= 0x20;
    cpu.f = f;
}

void Dec8(uint8_t& r) {
    const uint8_t old = r;
    r = static_cast<uint8_t>(old - 1);
    cpu.f = static_cast<uint8_t>((cpu.f & 0x1f) | (r == 0 ? 0x80 : 0) | ((old & 0xf) != 0 ? 0x40 : 0x60));
}

void Add8(uint8_t value, uint32_t carry) {
    const uint32_t a = cpu.a;
    const uint32_t sum = a + value + carry;
    cpu.a = static_cast<uint8_t>(sum);
    cpu.f = static_cast<uint8_t>(((sum & 0xff) == 0 ? 0x80 : 0) | (((a ^ value ^ sum) >> 4 & 1) << 5) | (sum >> 4 & 0x10));
}

uint8_t SubFlags(uint32_t a, uint32_t value, uint32_t diff) {
    return static_cast<uint8_t>(((diff & 0xff) == 0 ? 0xc0 : 0x40) | ((a ^ value ^ diff) & 0x10) << 1 | (diff >> 4 & 0x10));
}

void Sub8(uint8_t value, uint32_t carry) {
    const uint32_t a = cpu.a;
    const uint32_t diff = a - value - carry;
    cpu.a = static_cast<uint8_t>(diff);
    cpu.f = SubFlags(a, value, diff);
}

void Cp8(uint8_t value) {
    const uint32_t a = cpu.a;
    cpu.f = SubFlags(a, value, a - value);
}

void And8(uint8_t value) {
    cpu.a &= value;
    cpu.f = cpu.a != 0 ? 0x20 : 0xa0;
}

void Xor8(uint8_t value) {
    cpu.a ^= value;
    cpu.f = cpu.a == 0 ? 0x80 : 0;
}

void Or8(uint8_t value) {
    cpu.a |= value;
    cpu.f = cpu.a == 0 ? 0x80 : 0;
}

void AddHl(uint16_t value) {
    const uint32_t hl = HL();
    const uint32_t sum = hl + value;
    SetHL(static_cast<uint16_t>(sum));
    cpu.f = static_cast<uint8_t>((cpu.f & 0x8f) | ((sum & 0x10000) ? 0x10 : 0) | (((sum ^ hl ^ value) & 0x1000) ? 0x20 : 0));
}

uint16_t SpPlusOffset() {  // ADD SP,e / LD HL,SP+e
    const int32_t e = static_cast<int8_t>(Fetch8());
    const uint32_t sp = cpu.sp;
    const uint32_t result = sp + static_cast<uint32_t>(e);
    const uint32_t carries = (sp ^ static_cast<uint32_t>(e) ^ result) >> 4;
    cpu.f = static_cast<uint8_t>((carries & 0x10) | (carries & 1) << 5);
    return static_cast<uint16_t>(result);
}

void JumpRelative(bool taken) {
    const int8_t e = static_cast<int8_t>(Read8(cpu.pc));
    if (taken) {
        cpu.pc = static_cast<uint16_t>(cpu.pc + e);
        cpu.ticks += 12;
    } else {
        cpu.ticks += 8;
    }
    ++cpu.pc;
}

void JumpAbsolute(bool taken) {
    const uint16_t target = Read16(cpu.pc);
    cpu.pc = taken ? target : static_cast<uint16_t>(cpu.pc + 2);
    cpu.ticks += taken ? 16 : 12;
}

void CallAbsolute(bool taken) {
    const uint16_t target = Read16(cpu.pc);
    const uint32_t ret = static_cast<uint32_t>(cpu.pc) + 2;
    cpu.pc = static_cast<uint16_t>(ret);
    if (taken) {
        Push16(ret);
        cpu.pc = target;
        cpu.ticks += 24;
    } else {
        cpu.ticks += 12;
    }
}

void ReturnIf(bool taken) {
    if (taken) {
        cpu.pc = Pop16();
        cpu.ticks += 20;
    } else {
        cpu.ticks += 8;
    }
}

void Restart(uint16_t vector) {
    Push16(cpu.pc);
    cpu.pc = vector;
    cpu.ticks += 16;
}

void Daa() {
    const uint32_t a = cpu.a;
    uint32_t v;
    if ((cpu.f >> 6 & 1) == 0) {
        v = a + 6;
        if ((a & 0xe) < 10 && (cpu.f & 0x20) == 0) v = a;
        if ((cpu.f >> 4 & 1) != 0 || v > 0x9f) {  // tests the already-adjusted value
            cpu.f |= 0x10;
            v += 0x60;
        }
    } else {
        v = (a - 6) & 0xff;
        if ((cpu.f & 0x20) == 0) v = a;
        if (cpu.f & 0x10) v -= 0x60;
    }
    cpu.f = static_cast<uint8_t>((cpu.f & 0x5f) | ((v & 0xff) == 0 ? 0x80 : 0));
    cpu.a = static_cast<uint8_t>(v);
}

void Stop() {  // @ 0x710008efb0: CGB speed switch only, no low-power mode
    if (core.isCgb != 0) {
        const uint8_t key1 = Read8(0xff4d);
        cpu.key1 = key1;
        if (key1 & 1) {
            const uint8_t next = cpu.doubleSpeed == 0 ? 0x80 : 0;
            cpu.doubleSpeed = cpu.doubleSpeed == 0;
            Write8(0xff4d, next);
        }
    }
    if (cpu.doubleSpeed != 0) return;  // in double speed: no padding byte skipped, no ticks
    ++cpu.pc;
    cpu.ticks += 4;
}

void Illegal() {  // @ 0x710008e870, shared by NOP and D3 DB DD E3 E4 EB EC ED F4 FC FD
    if (cpu.debugLog != 0) std::printf("not implemented");
    cpu.ticks += 4;
}

// @ 0x7100092190
void ExecuteCb() {
    const uint8_t op = Fetch8();
    const uint32_t target = op & 7;
    uint8_t* reg = RegRef(target);
    const uint16_t hl = HL();
    uint8_t v = reg ? *reg : Read8(hl);
    const uint32_t group = op >> 3;
    const uint32_t c = cpu.f >> 4 & 1;
    bool store = true;

    if (group < 8) {
        uint8_t result = 0;
        uint8_t carry = 0;
        switch (group) {
        case 0: result = static_cast<uint8_t>(v << 1 | v >> 7); carry = v >> 7; break;               // RLC
        case 1: result = static_cast<uint8_t>(v >> 1 | v << 7); carry = v & 1; break;                // RRC
        case 2: result = static_cast<uint8_t>(v << 1 | c); carry = v >> 7; break;                    // RL
        case 3: result = static_cast<uint8_t>(v >> 1 | c << 7); carry = v & 1; break;                // RR
        case 4: result = static_cast<uint8_t>(v << 1); carry = v >> 7; break;                        // SLA
        case 5: result = static_cast<uint8_t>(v >> 1 | (v & 0x80)); carry = v & 1; break;            // SRA
        case 6: result = static_cast<uint8_t>(v << 4 | v >> 4); carry = 0; break;                    // SWAP
        default: result = static_cast<uint8_t>(v >> 1); carry = v & 1; break;                        // SRL
        }
        const uint8_t keep = group == 4 ? (cpu.f & 0x0f) : 0;  // SLA leaves F's low nibble alone
        cpu.f = static_cast<uint8_t>(keep | (result == 0 ? 0x80 : 0) | (carry ? 0x10 : 0));
        v = result;
    } else if (group < 16) {  // BIT: keeps C and F's low nibble
        const uint32_t bit = group - 8;
        cpu.f = static_cast<uint8_t>((cpu.f & 0x1f) | 0x20 | ((v >> bit & 1) == 0 ? 0x80 : 0));
        store = false;
    } else if (group < 24) {  // RES
        v = static_cast<uint8_t>(v & ~(1u << (group - 16)));
    } else {  // SET
        v = static_cast<uint8_t>(v | 1u << (group - 24));
    }

    if (store) {
        if (reg) *reg = v;
        else Write8(hl, v);
    }
    if (reg) cpu.ticks += 8;
    else cpu.ticks += store ? 16 : 12;
}

void Execute(uint8_t op) {
    // LD r,r' / LD r,(HL) / LD (HL),r / HALT
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) {
            cpu.halted = 1;
            cpu.ticks += 4;
            return;
        }
        uint8_t* dst = RegRef(op >> 3);
        uint8_t* src = RegRef(op);
        if (dst && src) {
            *dst = *src;
            cpu.ticks += 4;
        } else if (dst) {
            *dst = Read8(HL());
            cpu.ticks += 8;
        } else {
            Write8(HL(), *src);
            cpu.ticks += 8;
        }
        return;
    }
    // 8-bit arithmetic on r / (HL)
    if (op >= 0x80 && op < 0xc0) {
        uint8_t* src = RegRef(op);
        const uint8_t value = src ? *src : Read8(HL());
        switch (op >> 3 & 7) {
        case 0: Add8(value, 0); break;
        case 1: Add8(value, cpu.f >> 4 & 1); break;
        case 2: Sub8(value, 0); break;
        case 3: Sub8(value, cpu.f >> 4 & 1); break;
        case 4: And8(value); break;
        case 5: Xor8(value); break;
        case 6: Or8(value); break;
        default: Cp8(value); break;
        }
        cpu.ticks += src ? 4 : 8;
        return;
    }

    switch (op) {
    case 0x00: Illegal(); return;
    case 0x01: SetBC(Fetch16()); cpu.ticks += 12; return;
    case 0x02: Write8(BC(), cpu.a); cpu.ticks += 8; return;
    case 0x03: SetBC(static_cast<uint16_t>(BC() + 1)); cpu.ticks += 8; return;
    case 0x04: Inc8(cpu.b); cpu.ticks += 4; return;
    case 0x05: Dec8(cpu.b); cpu.ticks += 4; return;
    case 0x06: cpu.b = Fetch8(); cpu.ticks += 8; return;
    case 0x07: cpu.f = static_cast<uint8_t>((cpu.a >> 7) << 4); cpu.a = static_cast<uint8_t>(cpu.a >> 7 | cpu.a << 1); cpu.ticks += 4; return;
    case 0x08: {
        const uint16_t addr = Read16(cpu.pc);
        Write16(addr, cpu.sp);
        cpu.pc = static_cast<uint16_t>(cpu.pc + 2);
        cpu.ticks += 20;
        return;
    }
    case 0x09: AddHl(BC()); cpu.ticks += 8; return;
    case 0x0a: cpu.a = Read8(BC()); cpu.ticks += 8; return;
    case 0x0b: SetBC(static_cast<uint16_t>(BC() - 1)); cpu.ticks += 8; return;
    case 0x0c: Inc8(cpu.c); cpu.ticks += 4; return;
    case 0x0d: Dec8(cpu.c); cpu.ticks += 4; return;
    case 0x0e: cpu.c = Fetch8(); cpu.ticks += 8; return;
    case 0x0f: cpu.f = static_cast<uint8_t>((cpu.a & 1) << 4); cpu.a = static_cast<uint8_t>(cpu.a >> 1 | cpu.a << 7); cpu.ticks += 4; return;

    case 0x10: Stop(); return;
    case 0x11: SetDE(Fetch16()); cpu.ticks += 12; return;
    case 0x12: Write8(DE(), cpu.a); cpu.ticks += 8; return;
    case 0x13: SetDE(static_cast<uint16_t>(DE() + 1)); cpu.ticks += 8; return;
    case 0x14: Inc8(cpu.d); cpu.ticks += 4; return;
    case 0x15: Dec8(cpu.d); cpu.ticks += 4; return;
    case 0x16: cpu.d = Fetch8(); cpu.ticks += 8; return;
    case 0x17: {
        const uint8_t carry = cpu.f >> 4 & 1;
        cpu.f = cpu.a >> 3 & 0x10;
        cpu.a = static_cast<uint8_t>(cpu.a << 1 | carry);
        cpu.ticks += 4;
        return;
    }
    case 0x18: {
        const int8_t e = static_cast<int8_t>(Read8(cpu.pc));
        cpu.pc = static_cast<uint16_t>(cpu.pc + e + 1);
        cpu.ticks += 12;
        return;
    }
    case 0x19: AddHl(DE()); cpu.ticks += 8; return;
    case 0x1a: cpu.a = Read8(DE()); cpu.ticks += 8; return;
    case 0x1b: SetDE(static_cast<uint16_t>(DE() - 1)); cpu.ticks += 8; return;
    case 0x1c: Inc8(cpu.e); cpu.ticks += 4; return;
    case 0x1d: Dec8(cpu.e); cpu.ticks += 4; return;
    case 0x1e: cpu.e = Fetch8(); cpu.ticks += 8; return;
    case 0x1f: {
        const uint8_t carry = cpu.f >> 4 & 1;
        cpu.f = static_cast<uint8_t>((cpu.a & 1) << 4);
        cpu.a = static_cast<uint8_t>(cpu.a >> 1 | carry << 7);
        cpu.ticks += 4;
        return;
    }

    case 0x20: JumpRelative(!FlagZ()); return;
    case 0x21: SetHL(Fetch16()); cpu.ticks += 12; return;
    case 0x22: { const uint16_t hl = HL(); SetHL(static_cast<uint16_t>(hl + 1)); Write8(hl, cpu.a); cpu.ticks += 8; return; }
    case 0x23: SetHL(static_cast<uint16_t>(HL() + 1)); cpu.ticks += 8; return;
    case 0x24: Inc8(cpu.h); cpu.ticks += 4; return;
    case 0x25: Dec8(cpu.h); cpu.ticks += 4; return;
    case 0x26: cpu.h = Fetch8(); cpu.ticks += 8; return;
    case 0x27: Daa(); cpu.ticks += 4; return;
    case 0x28: JumpRelative(FlagZ()); return;
    case 0x29: AddHl(HL()); cpu.ticks += 8; return;
    case 0x2a: { const uint16_t hl = HL(); SetHL(static_cast<uint16_t>(hl + 1)); cpu.a = Read8(hl); cpu.ticks += 8; return; }
    case 0x2b: SetHL(static_cast<uint16_t>(HL() - 1)); cpu.ticks += 8; return;
    case 0x2c: Inc8(cpu.l); cpu.ticks += 4; return;
    case 0x2d: Dec8(cpu.l); cpu.ticks += 4; return;
    case 0x2e: cpu.l = Fetch8(); cpu.ticks += 8; return;
    case 0x2f: cpu.a = static_cast<uint8_t>(~cpu.a); cpu.f |= 0x60; cpu.ticks += 4; return;

    case 0x30: JumpRelative(!FlagC()); return;
    case 0x31: cpu.sp = Fetch16(); cpu.ticks += 12; return;
    case 0x32: { const uint16_t hl = HL(); SetHL(static_cast<uint16_t>(hl - 1)); Write8(hl, cpu.a); cpu.ticks += 8; return; }
    case 0x33: ++cpu.sp; cpu.ticks += 8; return;
    case 0x34: { uint8_t v = Read8(HL()); Inc8(v); Write8(HL(), v); cpu.ticks += 12; return; }
    case 0x35: { uint8_t v = Read8(HL()); Dec8(v); Write8(HL(), v); cpu.ticks += 12; return; }
    case 0x36: { const uint8_t v = Read8(cpu.pc); Write8(HL(), v); ++cpu.pc; cpu.ticks += 12; return; }
    case 0x37: cpu.f = static_cast<uint8_t>((cpu.f & 0x9f) | 0x10); cpu.ticks += 4; return;
    case 0x38: JumpRelative(FlagC()); return;
    case 0x39: AddHl(cpu.sp); cpu.ticks += 8; return;
    case 0x3a: { const uint16_t hl = HL(); SetHL(static_cast<uint16_t>(hl - 1)); cpu.a = Read8(hl); cpu.ticks += 8; return; }
    case 0x3b: --cpu.sp; cpu.ticks += 8; return;
    case 0x3c: Inc8(cpu.a); cpu.ticks += 4; return;
    case 0x3d: Dec8(cpu.a); cpu.ticks += 4; return;
    case 0x3e: cpu.a = Fetch8(); cpu.ticks += 8; return;
    case 0x3f: cpu.f = static_cast<uint8_t>((cpu.f & 0x9f) ^ 0x10); cpu.ticks += 4; return;

    case 0xc0: ReturnIf(!FlagZ()); return;
    case 0xc1: SetBC(Pop16()); cpu.ticks += 12; return;
    case 0xc2: JumpAbsolute(!FlagZ()); return;
    case 0xc3: cpu.pc = Read16(cpu.pc); cpu.ticks += 16; return;
    case 0xc4: CallAbsolute(!FlagZ()); return;
    case 0xc5: Push16(BC()); cpu.ticks += 16; return;
    case 0xc6: Add8(Fetch8(), 0); cpu.ticks += 8; return;
    case 0xc7: Restart(0x00); return;
    case 0xc8: ReturnIf(FlagZ()); return;
    case 0xc9: cpu.pc = Pop16(); cpu.ticks += 16; return;
    case 0xca: JumpAbsolute(FlagZ()); return;
    case 0xcb: ExecuteCb(); return;
    case 0xcc: CallAbsolute(FlagZ()); return;
    case 0xcd: {
        const uint16_t target = Read16(cpu.pc);
        Push16(static_cast<uint32_t>(cpu.pc) + 2);
        cpu.pc = target;
        cpu.ticks += 24;
        return;
    }
    case 0xce: Add8(Fetch8(), cpu.f >> 4 & 1); cpu.ticks += 8; return;
    case 0xcf: Restart(0x08); return;

    case 0xd0: ReturnIf(!FlagC()); return;
    case 0xd1: SetDE(Pop16()); cpu.ticks += 12; return;
    case 0xd2: JumpAbsolute(!FlagC()); return;
    case 0xd4: CallAbsolute(!FlagC()); return;
    case 0xd5: Push16(DE()); cpu.ticks += 16; return;
    case 0xd6: Sub8(Fetch8(), 0); cpu.ticks += 8; return;
    case 0xd7: Restart(0x10); return;
    case 0xd8: ReturnIf(FlagC()); return;
    case 0xd9:  // RETI @ 0x7100085660: enables interrupts immediately
        core.ime = 1;
        core.imeRequest = 1;
        cpu.pc = Pop16();
        cpu.ticks += 16;
        return;
    case 0xda: JumpAbsolute(FlagC()); return;
    case 0xdc: CallAbsolute(FlagC()); return;
    case 0xde: Sub8(Fetch8(), cpu.f >> 4 & 1); cpu.ticks += 8; return;
    case 0xdf: Restart(0x18); return;

    case 0xe0: { const uint8_t n = Read8(cpu.pc); Write8(0xff00u | n, cpu.a); ++cpu.pc; cpu.ticks += 12; return; }
    case 0xe1: SetHL(Pop16()); cpu.ticks += 12; return;
    case 0xe2: Write8(0xff00u | cpu.c, cpu.a); cpu.ticks += 8; return;
    case 0xe5: Push16(HL()); cpu.ticks += 16; return;
    case 0xe6: And8(Fetch8()); cpu.ticks += 8; return;
    case 0xe7: Restart(0x20); return;
    case 0xe8: cpu.sp = SpPlusOffset(); cpu.ticks += 16; return;
    case 0xe9: cpu.pc = HL(); cpu.ticks += 4; return;
    case 0xea: { const uint16_t addr = Read16(cpu.pc); Write8(addr, cpu.a); cpu.pc = static_cast<uint16_t>(cpu.pc + 2); cpu.ticks += 16; return; }
    case 0xee: Xor8(Fetch8()); cpu.ticks += 8; return;
    case 0xef: Restart(0x28); return;

    case 0xf0: { const uint8_t n = Read8(cpu.pc); cpu.a = Read8(0xff00u | n); ++cpu.pc; cpu.ticks += 12; return; }
    case 0xf1: { const uint16_t v = Pop16(); cpu.f = static_cast<uint8_t>(v & 0xf0); cpu.a = static_cast<uint8_t>(v >> 8); cpu.ticks += 12; return; }
    case 0xf2: cpu.a = Read8(0xff00u | cpu.c); cpu.ticks += 8; return;
    case 0xf3: core.imeRequest = 0; core.eiDelay = 1; cpu.ticks += 4; return;
    case 0xf5: Push16(Pair(cpu.a, cpu.f)); cpu.ticks += 16; return;
    case 0xf6: Or8(Fetch8()); cpu.ticks += 8; return;
    case 0xf7: Restart(0x30); return;
    case 0xf8: SetHL(SpPlusOffset()); cpu.ticks += 12; return;
    case 0xf9: cpu.sp = HL(); cpu.ticks += 8; return;
    case 0xfa: { const uint16_t addr = Read16(cpu.pc); cpu.a = Read8(addr); cpu.pc = static_cast<uint16_t>(cpu.pc + 2); cpu.ticks += 16; return; }
    case 0xfb: core.imeRequest = 1; core.eiDelay = 1; cpu.ticks += 4; return;
    case 0xfe: Cp8(Fetch8()); cpu.ticks += 8; return;
    case 0xff: cpu.rst38Marker = 0; Restart(0x38); return;

    default: Illegal(); return;  // D3 DB DD E3 E4 EB EC ED F4 FC FD
    }
}

// shared tail of every taken interrupt
int32_t Vector(uint8_t clearMask, uint16_t vector, bool clearBeforePush) {
    if (clearBeforePush) cpu.intFlags &= clearMask;
    core.imeRequest = 0;
    core.ime = 0;
    Push16(cpu.pc);
    cpu.pc = vector;
    if (!clearBeforePush) cpu.intFlags &= clearMask;
    cpu.halted = 0;
    return 0x14;
}

}  // namespace

// @ 0x710008e7f0
int32_t CpuStep() {
    cpu.ticks = 0;
    const uint8_t op = Read8(cpu.pc);
    if (cpu.halted != 0) {
        cpu.ticks += 4;
        return cpu.ticks;
    }
    ++cpu.pc;
    Execute(op);
    return cpu.ticks;
}

// @ 0x71000853e0. Priority is VBlank, Joypad, STAT, Serial, Timer (hardware: VBlank, STAT, Timer, Serial,
// Joypad). A pending enabled interrupt clears HALT even with interrupts disabled.
int32_t ServiceInterrupts() {
    if (core.eiDelay == 0) core.ime = core.imeRequest;
    else core.eiDelay = 0;

    const uint8_t pending = cpu.intFlags & cpu.intEnable;
    if (pending & 0x01) {
        cpu.halted = 0;
        if (core.ime == 0) return 0;
        return Vector(0xfe, 0x40, true);
    }
    if (pending & 0x10) {
        if (core.ime == 0) { cpu.halted = 0; return 0; }
        return Vector(0xef, 0x60, true);
    }
    if (pending & 0x02) {
        if (core.ime == 0) { cpu.halted = 0; return 0; }
        return Vector(0xfd, 0x48, false);
    }
    if (pending & 0x08) {
        if (core.ime == 0) { cpu.halted = 0; return 0; }
        return Vector(0xf7, 0x58, false);
    }
    if (pending & 0x04) {
        if (core.ime == 0) { cpu.halted = 0; return 0; }
        return Vector(0xfb, 0x50, false);
    }
    return 0;
}

// @ 0x710008e780
void CpuReset() {
    cpu.a = 0x11;
    cpu.c = 0x00;
    cpu.f = 0x80;
    cpu.b = 0x00;
    cpu.e = 0x56;
    cpu.d = 0xff;
    cpu.l = 0x0d;
    cpu.h = 0x00;
    cpu.sp = 0xfffe;
    cpu.pc = 0x0100;
    Write8(0xff40, 0x91);
    Write8(0xff41, 0x81);
    Write8(0xff40, 0x91);
}

}  // namespace gb
