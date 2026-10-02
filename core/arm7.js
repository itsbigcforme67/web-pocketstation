// Web PocketStation - Copyright (C) 2026 the Web PocketStation authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// ARM7TDMI interpreter (ARMv4T: ARM + Thumb), written for the PocketStation emulator.
// Portable plain JS (ES module) so it runs in browsers and in Node for tests.
//
// Bus interface expected:
//   read8(a) read16(a) read32(a)   -> unsigned values; addresses are pre-aligned by the CPU
//   write8(a,v) write16(a,v) write32(a,v)
//   waitstates(a, width) optional  -> extra cycles for an access (not used by default)

export const MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
  MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F;

function bankIndex(mode) {
  switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0; // USR / SYS (and invalid modes)
  }
}

export class ARM7 {
  constructor(bus) {
    this.bus = bus;
    this.r = new Int32Array(16);
    this.bankR13 = new Int32Array(6);
    this.bankR14 = new Int32Array(6);
    this.spsrBank = new Int32Array(6);
    this.usrR8 = new Int32Array(5);
    this.fiqR8 = new Int32Array(5);
    this.n = 0; this.z = 0; this.c = 0; this.v = 0;
    this.iflag = 1; this.fflag = 1; this.t = 0; this.mode = MODE_SVC;
    this.pc = 0;          // address of the next instruction to execute
    this.cycles = 0;      // cycles consumed (accumulates; caller may reset)
    this.irqLine = false; // driven by the interrupt controller
    this.fiqLine = false;
    this.sc = 0;          // shifter carry-out scratch
    this.onUndefined = null; // optional hook(cpu, instr) -> true if handled
    this.onSWI = null;       // optional HLE hook(cpu, comment) -> true if handled
    this.reset();
  }

  reset() {
    this.r.fill(0);
    this.bankR13.fill(0); this.bankR14.fill(0); this.spsrBank.fill(0);
    this.usrR8.fill(0); this.fiqR8.fill(0);
    this.n = this.z = this.c = this.v = 0;
    this.mode = MODE_SVC; this.iflag = 1; this.fflag = 1; this.t = 0;
    this.pc = 0;
  }

  // ---------------- CPSR / modes ----------------
  getCPSR() {
    return ((this.n << 31) | (this.z << 30) | (this.c << 29) | (this.v << 28) |
      (this.iflag << 7) | (this.fflag << 6) | (this.t << 5) | this.mode) | 0;
  }
  setCPSR(val) {
    this.n = (val >>> 31) & 1; this.z = (val >>> 30) & 1;
    this.c = (val >>> 29) & 1; this.v = (val >>> 28) & 1;
    this.iflag = (val >>> 7) & 1; this.fflag = (val >>> 6) & 1;
    this.t = (val >>> 5) & 1;
    this.switchMode(val & 0x1F);
  }
  switchMode(newMode) {
    const old = this.mode;
    if (old === newMode) return;
    const ob = bankIndex(old), nb = bankIndex(newMode);
    const r = this.r;
    if (ob !== nb) {
      this.bankR13[ob] = r[13]; this.bankR14[ob] = r[14];
      if ((old === MODE_FIQ) !== (newMode === MODE_FIQ)) {
        const save = old === MODE_FIQ ? this.fiqR8 : this.usrR8;
        const load = newMode === MODE_FIQ ? this.fiqR8 : this.usrR8;
        for (let i = 0; i < 5; i++) { save[i] = r[8 + i]; r[8 + i] = load[i]; }
      }
      r[13] = this.bankR13[nb]; r[14] = this.bankR14[nb];
    }
    this.mode = newMode;
  }
  getSPSR() { const b = bankIndex(this.mode); return b ? this.spsrBank[b] : this.getCPSR(); }
  setSPSR(v) { const b = bankIndex(this.mode); if (b) this.spsrBank[b] = v; }

  // Read a user-bank register (for LDM/STM with S bit)
  getUserReg(i) {
    if (i < 8 || i === 15) return this.r[i];
    if (this.mode === MODE_FIQ && i < 13) return this.usrR8[i - 8];
    if (i < 13) return this.r[i];
    if (bankIndex(this.mode) === 0) return this.r[i];
    return i === 13 ? this.bankR13[0] : this.bankR14[0];
  }
  setUserReg(i, v) {
    if (i < 8 || i === 15) { this.r[i] = v; return; }
    if (this.mode === MODE_FIQ && i < 13) { this.usrR8[i - 8] = v; return; }
    if (i < 13 || bankIndex(this.mode) === 0) { this.r[i] = v; return; }
    if (i === 13) this.bankR13[0] = v; else this.bankR14[0] = v;
  }

  exception(vector, newMode, lr, disableFiq) {
    const cpsr = this.getCPSR();
    this.switchMode(newMode);
    this.spsrBank[bankIndex(newMode)] = cpsr;
    this.r[14] = lr;
    this.t = 0;
    this.iflag = 1;
    if (disableFiq) this.fflag = 1;
    this.pc = vector;
    this.cycles += 3;
  }

  // ---------------- execution ----------------
  // Executes until at least `budget` cycles have been consumed. Returns cycles used.
  run(budget) {
    const start = this.cycles;
    const end = start + budget;
    while (this.cycles < end) {
      if (this.fiqLine && !this.fflag) {
        this.exception(0x1C, MODE_FIQ, (this.pc + 4) | 0, true);
      } else if (this.irqLine && !this.iflag) {
        this.exception(0x18, MODE_IRQ, (this.pc + 4) | 0, false);
      }
      this.step();
      if (this.stopRequested) { this.stopRequested = false; break; }
    }
    return this.cycles - start;
  }

  step() {
    const pc = this.pc;
    if (this.t) {
      const op = this.bus.read16(pc & ~1);
      this.pc = (pc + 2) | 0;
      this.r[15] = (pc + 4) | 0;
      this.cycles += 1;
      THUMB[op >>> 6](this, op);
    } else {
      const op = this.bus.read32(pc & ~3) | 0;
      this.pc = (pc + 4) | 0;
      this.r[15] = (pc + 8) | 0;
      this.cycles += 1;
      if (!this.condPassed(op >>> 28)) return;
      ARM[((op >>> 16) & 0xFF0) | ((op >>> 4) & 0xF)](this, op);
    }
  }

  condPassed(cond) {
    switch (cond) {
      case 0xE: return true;
      case 0x0: return this.z === 1;
      case 0x1: return this.z === 0;
      case 0x2: return this.c === 1;
      case 0x3: return this.c === 0;
      case 0x4: return this.n === 1;
      case 0x5: return this.n === 0;
      case 0x6: return this.v === 1;
      case 0x7: return this.v === 0;
      case 0x8: return this.c === 1 && this.z === 0;
      case 0x9: return this.c === 0 || this.z === 1;
      case 0xA: return this.n === this.v;
      case 0xB: return this.n !== this.v;
      case 0xC: return this.z === 0 && this.n === this.v;
      case 0xD: return this.z === 1 || this.n !== this.v;
      default: return false; // 0xF: never (ARMv4)
    }
  }

  // Write a register; writing r15 branches.
  setReg(rd, val) {
    if (rd === 15) this.branchTo(val);
    else this.r[rd] = val;
  }
  branchTo(addr) {
    this.pc = (this.t ? addr & ~1 : addr & ~3) | 0;
    this.cycles += 2;
  }
  // BX-style branch with state switch
  branchExchange(addr) {
    this.t = addr & 1;
    this.pc = (addr & ~1) | 0;
    if (!this.t) this.pc &= ~3;
    this.cycles += 2;
  }

  // --------------- memory helpers ---------------
  ldr32(addr) { // with ARM7 rotation for misaligned
    const v = this.bus.read32(addr & ~3);
    const rot = (addr & 3) << 3;
    return rot ? ((v >>> rot) | (v << (32 - rot))) | 0 : v | 0;
  }
  ldr16(addr) { // LDRH: misaligned rotates
    const v = this.bus.read16(addr & ~1) & 0xFFFF;
    return (addr & 1) ? ((v >>> 8) | (v << 24)) | 0 : v;
  }
  ldrs16(addr) {
    if (addr & 1) { const b = this.bus.read8(addr) & 0xFF; return (b << 24) >> 24; }
    return (this.bus.read16(addr) << 16) >> 16;
  }
  ldrs8(addr) { return (this.bus.read8(addr) << 24) >> 24; }

  // --------------- ALU helpers ---------------
  setNZ(res) { this.n = (res >>> 31) & 1; this.z = res === 0 ? 1 : 0; }
  add(a, b, s) {
    const res = (a + b) | 0;
    if (s) {
      this.n = (res >>> 31) & 1; this.z = res === 0 ? 1 : 0;
      this.c = ((a >>> 0) + (b >>> 0)) > 0xFFFFFFFF ? 1 : 0;
      this.v = (((a ^ res) & (b ^ res)) >>> 31) & 1;
    }
    return res;
  }
  adc(a, b, s) {
    const cin = this.c;
    const res = (a + b + cin) | 0;
    if (s) {
      this.n = (res >>> 31) & 1; this.z = res === 0 ? 1 : 0;
      this.c = ((a >>> 0) + (b >>> 0) + cin) > 0xFFFFFFFF ? 1 : 0;
      this.v = (((a ^ res) & (b ^ res)) >>> 31) & 1;
    }
    return res;
  }
  sub(a, b, s) {
    const res = (a - b) | 0;
    if (s) {
      this.n = (res >>> 31) & 1; this.z = res === 0 ? 1 : 0;
      this.c = (a >>> 0) >= (b >>> 0) ? 1 : 0;
      this.v = (((a ^ b) & (a ^ res)) >>> 31) & 1;
    }
    return res;
  }
  sbc(a, b, s) {
    const borrow = 1 - this.c;
    const res = (a - b - borrow) | 0;
    if (s) {
      this.n = (res >>> 31) & 1; this.z = res === 0 ? 1 : 0;
      this.c = (a >>> 0) >= ((b >>> 0) + borrow) ? 1 : 0;
      this.v = (((a ^ b) & (a ^ res)) >>> 31) & 1;
    }
    return res;
  }

  // Barrel shifter. type: 0 LSL, 1 LSR, 2 ASR, 3 ROR. Sets this.sc.
  shiftImm(type, val, amt) {
    switch (type) {
      case 0:
        if (amt === 0) { this.sc = this.c; return val; }
        this.sc = (val >>> (32 - amt)) & 1; return (val << amt) | 0;
      case 1:
        if (amt === 0) { this.sc = (val >>> 31) & 1; return 0; }
        this.sc = (val >>> (amt - 1)) & 1; return (val >>> amt) | 0;
      case 2:
        if (amt === 0) { this.sc = (val >>> 31) & 1; return val >> 31; }
        this.sc = (val >>> (amt - 1)) & 1; return val >> amt;
      default:
        if (amt === 0) { // RRX
          this.sc = val & 1; return ((this.c << 31) | (val >>> 1)) | 0;
        }
        this.sc = (val >>> (amt - 1)) & 1; return ((val >>> amt) | (val << (32 - amt))) | 0;
    }
  }
  shiftReg(type, val, amt) {
    amt &= 0xFF;
    if (amt === 0) { this.sc = this.c; return val; }
    switch (type) {
      case 0:
        if (amt < 32) { this.sc = (val >>> (32 - amt)) & 1; return (val << amt) | 0; }
        this.sc = amt === 32 ? val & 1 : 0; return 0;
      case 1:
        if (amt < 32) { this.sc = (val >>> (amt - 1)) & 1; return (val >>> amt) | 0; }
        this.sc = amt === 32 ? (val >>> 31) & 1 : 0; return 0;
      case 2:
        if (amt < 32) { this.sc = (val >>> (amt - 1)) & 1; return val >> amt; }
        this.sc = (val >>> 31) & 1; return val >> 31;
      default: {
        const a = amt & 31;
        if (a === 0) { this.sc = (val >>> 31) & 1; return val; }
        this.sc = (val >>> (a - 1)) & 1; return ((val >>> a) | (val << (32 - a))) | 0;
      }
    }
  }
}

// =====================================================================
// ARM instruction handlers
// =====================================================================

function armUndefined(cpu, op) {
  if (cpu.onUndefined && cpu.onUndefined(cpu, op)) return;
  const ret = cpu.pc; // address of next instruction
  cpu.exception(0x04, MODE_UND, ret, false);
}

function armSWI(cpu, op) {
  if (cpu.onSWI && cpu.onSWI(cpu, op & 0xFFFFFF)) return;
  cpu.exception(0x08, MODE_SVC, cpu.pc, false);
}

function armB(cpu, op) {
  const off = (op << 8) >> 6;
  if (op & 0x01000000) cpu.r[14] = cpu.pc; // BL: address of next instruction
  cpu.branchTo((cpu.r[15] + off) | 0);
}

function armBX(cpu, op) {
  cpu.branchExchange(cpu.r[op & 15]);
}

function armMRS(cpu, op) {
  const rd = (op >>> 12) & 15;
  cpu.r[rd] = (op & 0x00400000) ? cpu.getSPSR() : cpu.getCPSR();
}

function armMSR(cpu, op) {
  let val;
  if (op & 0x02000000) {
    const rot = ((op >>> 8) & 15) << 1;
    const imm = op & 0xFF;
    val = rot ? ((imm >>> rot) | (imm << (32 - rot))) | 0 : imm;
  } else {
    val = cpu.r[op & 15];
  }
  let mask = 0;
  if (op & 0x00010000) mask |= 0x000000FF;
  if (op & 0x00020000) mask |= 0x0000FF00;
  if (op & 0x00040000) mask |= 0x00FF0000;
  if (op & 0x00080000) mask |= 0xFF000000;
  if (op & 0x00400000) {
    if (cpu.mode === MODE_USR || cpu.mode === MODE_SYS) return;
    const old = cpu.getSPSR();
    cpu.setSPSR((old & ~mask) | (val & mask));
  } else {
    if (cpu.mode === MODE_USR) mask &= 0xFF000000;
    const old = cpu.getCPSR();
    let nv = (old & ~mask) | (val & mask);
    // T bit is not changeable through MSR on real hardware (keep current)
    nv = (nv & ~0x20) | (old & 0x20);
    cpu.setCPSR(nv);
  }
}

// Data processing. Operand2 computed by the variant; then common op.
function dataProc(cpu, op, opnd2, sc) {
  const opcode = (op >>> 21) & 15;
  const s = (op >>> 20) & 1;
  const rn = (op >>> 16) & 15;
  const rd = (op >>> 12) & 15;
  const a = cpu.r[rn];
  let res;
  let logical = false;
  switch (opcode) {
    case 0x0: res = a & opnd2; logical = true; break;            // AND
    case 0x1: res = a ^ opnd2; logical = true; break;            // EOR
    case 0x2: res = cpu.sub(a, opnd2, s && rd !== 15); break;    // SUB
    case 0x3: res = cpu.sub(opnd2, a, s && rd !== 15); break;    // RSB
    case 0x4: res = cpu.add(a, opnd2, s && rd !== 15); break;    // ADD
    case 0x5: res = cpu.adc(a, opnd2, s && rd !== 15); break;    // ADC
    case 0x6: res = cpu.sbc(a, opnd2, s && rd !== 15); break;    // SBC
    case 0x7: res = cpu.sbc(opnd2, a, s && rd !== 15); break;    // RSC
    case 0x8: res = a & opnd2; cpu.setNZ(res); cpu.c = sc; return;   // TST
    case 0x9: res = a ^ opnd2; cpu.setNZ(res); cpu.c = sc; return;   // TEQ
    case 0xA: cpu.sub(a, opnd2, true); return;                       // CMP
    case 0xB: cpu.add(a, opnd2, true); return;                       // CMN
    case 0xC: res = a | opnd2; logical = true; break;            // ORR
    case 0xD: res = opnd2; logical = true; break;                // MOV
    case 0xE: res = a & ~opnd2; logical = true; break;           // BIC
    default: res = ~opnd2; logical = true; break;                // MVN
  }
  if (rd === 15) {
    if (s) {
      // restore CPSR from SPSR (exception return)
      const spsr = cpu.getSPSR();
      cpu.setCPSR(spsr);
      cpu.pc = (cpu.t ? res & ~1 : res & ~3) | 0;
      cpu.cycles += 2;
    } else {
      cpu.branchTo(res);
    }
    return;
  }
  cpu.r[rd] = res;
  if (s && logical) { cpu.setNZ(res); cpu.c = sc; }
}

function armDPImm(cpu, op) {
  const rot = ((op >>> 8) & 15) << 1;
  const imm = op & 0xFF;
  let val, sc;
  if (rot) { val = ((imm >>> rot) | (imm << (32 - rot))) | 0; sc = (val >>> 31) & 1; }
  else { val = imm; sc = cpu.c; }
  dataProc(cpu, op, val, sc);
}

function armDPRegImmShift(cpu, op) {
  const val = cpu.shiftImm((op >>> 5) & 3, cpu.r[op & 15], (op >>> 7) & 31);
  dataProc(cpu, op, val, cpu.sc);
}

function armDPRegRegShift(cpu, op) {
  // PC as operand reads +12 when a register-specified shift is used
  cpu.r[15] = (cpu.r[15] + 4) | 0;
  const val = cpu.shiftReg((op >>> 5) & 3, cpu.r[op & 15], cpu.r[(op >>> 8) & 15]);
  cpu.cycles += 1;
  dataProc(cpu, op, val, cpu.sc);
  // (r15 is refreshed per instruction, no restore needed)
}

function mulCycles(cpu, rs) {
  const v = rs >>> 0;
  if ((v & 0xFFFFFF00) === 0 || (v & 0xFFFFFF00) === 0xFFFFFF00) cpu.cycles += 1;
  else if ((v & 0xFFFF0000) === 0 || (v & 0xFFFF0000) === 0xFFFF0000) cpu.cycles += 2;
  else if ((v & 0xFF000000) === 0 || (v & 0xFF000000) === 0xFF000000) cpu.cycles += 3;
  else cpu.cycles += 4;
}

function armMUL(cpu, op) {
  const rd = (op >>> 16) & 15, rn = (op >>> 12) & 15, rs = (op >>> 8) & 15, rm = op & 15;
  let res = Math.imul(cpu.r[rm], cpu.r[rs]);
  if (op & 0x00200000) { res = (res + cpu.r[rn]) | 0; cpu.cycles += 1; }
  mulCycles(cpu, cpu.r[rs]);
  cpu.r[rd] = res;
  if (op & 0x00100000) cpu.setNZ(res);
}

function armMULL(cpu, op) {
  const rdHi = (op >>> 16) & 15, rdLo = (op >>> 12) & 15, rs = (op >>> 8) & 15, rm = op & 15;
  const signed = op & 0x00400000;
  const acc = op & 0x00200000;
  let a, b;
  if (signed) { a = BigInt(cpu.r[rm]); b = BigInt(cpu.r[rs]); }
  else { a = BigInt(cpu.r[rm] >>> 0); b = BigInt(cpu.r[rs] >>> 0); }
  let res = a * b;
  if (acc) {
    res += (BigInt(cpu.r[rdHi] >>> 0) << 32n) | BigInt(cpu.r[rdLo] >>> 0);
    cpu.cycles += 1;
  }
  res = BigInt.asUintN(64, res);
  const lo = Number(res & 0xFFFFFFFFn) | 0;
  const hi = Number(res >> 32n) | 0;
  cpu.r[rdLo] = lo; cpu.r[rdHi] = hi;
  mulCycles(cpu, cpu.r[rs]); cpu.cycles += 1;
  if (op & 0x00100000) { cpu.n = (hi >>> 31) & 1; cpu.z = (hi === 0 && lo === 0) ? 1 : 0; }
}

function armSWP(cpu, op) {
  const rn = (op >>> 16) & 15, rd = (op >>> 12) & 15, rm = op & 15;
  const addr = cpu.r[rn];
  const src = cpu.r[rm];
  if (op & 0x00400000) {
    const v = cpu.bus.read8(addr) & 0xFF;
    cpu.bus.write8(addr, src & 0xFF);
    cpu.r[rd] = v;
  } else {
    const v = cpu.ldr32(addr);
    cpu.bus.write32(addr & ~3, src);
    cpu.r[rd] = v;
  }
  cpu.cycles += 3;
}

// Halfword / signed transfers
function armHalf(cpu, op) {
  const p = (op >>> 24) & 1, u = (op >>> 23) & 1, i = (op >>> 22) & 1,
    w = (op >>> 21) & 1, l = (op >>> 20) & 1;
  const rn = (op >>> 16) & 15, rd = (op >>> 12) & 15;
  const sh = (op >>> 5) & 3;
  const off = i ? (((op >>> 4) & 0xF0) | (op & 15)) : cpu.r[op & 15];
  const base = cpu.r[rn];
  const offAddr = (u ? base + off : base - off) | 0;
  const addr = p ? offAddr : base;
  if (l) {
    let v;
    if (sh === 1) v = cpu.ldr16(addr);
    else if (sh === 2) v = cpu.ldrs8(addr);
    else v = cpu.ldrs16(addr);
    if (!p || w) cpu.r[rn] = offAddr;
    cpu.setReg(rd, v);
    cpu.cycles += 2;
  } else {
    if (sh === 1) {
      let v = cpu.r[rd];
      if (rd === 15) v = (v + 4) | 0;
      cpu.bus.write16(addr & ~1, v & 0xFFFF);
    }
    // sh 2/3 store forms are LDRD/STRD on ARMv5; ignore on v4
    if (!p || w) cpu.r[rn] = offAddr;
    cpu.cycles += 1;
  }
}

function armSDT(cpu, op) {
  const i = (op >>> 25) & 1, p = (op >>> 24) & 1, u = (op >>> 23) & 1,
    b = (op >>> 22) & 1, w = (op >>> 21) & 1, l = (op >>> 20) & 1;
  const rn = (op >>> 16) & 15, rd = (op >>> 12) & 15;
  let off;
  if (i) off = cpu.shiftImm((op >>> 5) & 3, cpu.r[op & 15], (op >>> 7) & 31);
  else off = op & 0xFFF;
  const base = cpu.r[rn];
  const offAddr = (u ? base + off : base - off) | 0;
  const addr = p ? offAddr : base;
  if (l) {
    const v = b ? (cpu.bus.read8(addr) & 0xFF) : cpu.ldr32(addr);
    if (!p || w) cpu.r[rn] = offAddr;
    cpu.setReg(rd, v);
    cpu.cycles += 2;
  } else {
    let v = cpu.r[rd];
    if (rd === 15) v = (v + 4) | 0;
    if (b) cpu.bus.write8(addr, v & 0xFF);
    else cpu.bus.write32(addr & ~3, v);
    if (!p || w) cpu.r[rn] = offAddr;
    cpu.cycles += 1;
  }
}

function armBlock(cpu, op) {
  const p = (op >>> 24) & 1, u = (op >>> 23) & 1, s = (op >>> 22) & 1,
    w = (op >>> 21) & 1, l = (op >>> 20) & 1;
  const rn = (op >>> 16) & 15;
  let list = op & 0xFFFF;
  const base = cpu.r[rn];
  let count = 0;
  for (let x = list; x; x &= x - 1) count++;
  let emptyList = false;
  if (list === 0) { list = 0x8000; count = 16; emptyList = true; }
  // compute lowest address
  let addr;
  if (u) addr = p ? base + 4 : base;
  else addr = p ? base - count * 4 : base - count * 4 + 4;
  addr |= 0;
  const newBase = (u ? base + count * 4 : base - count * 4) | 0;
  const userBank = s && (!l || !(list & 0x8000));
  cpu.cycles += count + (l ? 1 : 0);
  if (l) {
    if (w) cpu.r[rn] = newBase;
    for (let i = 0; i < 16; i++) {
      if (!(list & (1 << i))) continue;
      const v = cpu.bus.read32(addr & ~3) | 0;
      addr = (addr + 4) | 0;
      if (emptyList && i === 15) { cpu.branchTo(v); continue; }
      if (userBank) cpu.setUserReg(i, v);
      else if (i === 15) {
        if (s) { cpu.setCPSR(cpu.getSPSR()); cpu.pc = (cpu.t ? v & ~1 : v & ~3) | 0; cpu.cycles += 2; }
        else cpu.branchTo(v);
      } else cpu.r[i] = v;
    }
  } else {
    let first = true;
    for (let i = 0; i < 16; i++) {
      if (!(list & (1 << i))) continue;
      let v;
      if (i === rn) v = first ? base : newBase;
      else if (userBank) v = cpu.getUserReg(i);
      else v = cpu.r[i];
      if (i === 15) v = (cpu.r[15] + 4) | 0;
      cpu.bus.write32(addr & ~3, v);
      addr = (addr + 4) | 0;
      first = false;
    }
    if (w) cpu.r[rn] = newBase;
  }
}

// Build ARM decode table indexed by bits[27:20]<<4 | bits[7:4]
const ARM = new Array(4096);
(function buildARM() {
  for (let key = 0; key < 4096; key++) {
    const hi = key >>> 4;       // bits 27-20
    const lo = key & 15;        // bits 7-4
    const top3 = hi >>> 5;      // bits 27-25
    let h = armUndefined;
    switch (top3) {
      case 0: {
        if (lo === 9) {
          if ((hi & 0xFC) === 0x00) h = armMUL;
          else if ((hi & 0xF8) === 0x08) h = armMULL;
          else if ((hi & 0xFB) === 0x10) h = armSWP;
          else h = armUndefined;
        } else if ((lo & 9) === 9) {
          h = armHalf; // LDRH/STRH/LDRSB/LDRSH
        } else if ((hi & 0x19) === 0x10) {
          // opcodes 8-11 with S=0: misc instructions
          if ((hi === 0x12) && lo === 1) h = armBX;
          else if ((hi & 0xFB) === 0x10 && lo === 0) h = armMRS;
          else if ((hi & 0xFB) === 0x12 && lo === 0) h = armMSR;
          else h = armUndefined;
        } else {
          h = (lo & 1) ? armDPRegRegShift : armDPRegImmShift;
        }
        break;
      }
      case 1: {
        if ((hi & 0x19) === 0x10) {
          h = ((hi & 0xFB) === 0x32) ? armMSR : armUndefined;
        } else h = armDPImm;
        break;
      }
      case 2: h = armSDT; break;
      case 3: h = (lo & 1) ? armUndefined : armSDT; break;
      case 4: h = armBlock; break;
      case 5: h = armB; break;
      case 6: h = armUndefined; break; // coprocessor transfer
      case 7: h = (hi & 0x10) ? armSWI : armUndefined; break;
    }
    ARM[key] = h;
  }
})();

// =====================================================================
// THUMB instruction handlers (indexed by op >>> 6, 1024 entries)
// =====================================================================

function tShiftImm(cpu, op) {
  const type = (op >>> 11) & 3, amt = (op >>> 6) & 31, rs = (op >>> 3) & 7, rd = op & 7;
  const res = cpu.shiftImm(type, cpu.r[rs], amt);
  cpu.r[rd] = res; cpu.setNZ(res); cpu.c = cpu.sc;
}
function tAddSub(cpu, op) {
  const imm = (op >>> 10) & 1, sub = (op >>> 9) & 1, rn = (op >>> 6) & 7, rs = (op >>> 3) & 7, rd = op & 7;
  const b = imm ? rn : cpu.r[rn];
  cpu.r[rd] = sub ? cpu.sub(cpu.r[rs], b, true) : cpu.add(cpu.r[rs], b, true);
}
function tImm(cpu, op) {
  const o = (op >>> 11) & 3, rd = (op >>> 8) & 7, imm = op & 0xFF;
  switch (o) {
    case 0: cpu.r[rd] = imm; cpu.setNZ(imm); break;
    case 1: cpu.sub(cpu.r[rd], imm, true); break;
    case 2: cpu.r[rd] = cpu.add(cpu.r[rd], imm, true); break;
    default: cpu.r[rd] = cpu.sub(cpu.r[rd], imm, true); break;
  }
}
function tALU(cpu, op) {
  const o = (op >>> 6) & 15, rs = (op >>> 3) & 7, rd = op & 7;
  const a = cpu.r[rd], b = cpu.r[rs];
  let res;
  switch (o) {
    case 0x0: res = a & b; cpu.r[rd] = res; cpu.setNZ(res); break;
    case 0x1: res = a ^ b; cpu.r[rd] = res; cpu.setNZ(res); break;
    case 0x2: res = cpu.shiftReg(0, a, b); cpu.r[rd] = res; cpu.setNZ(res); cpu.c = cpu.sc; cpu.cycles++; break;
    case 0x3: res = cpu.shiftReg(1, a, b); cpu.r[rd] = res; cpu.setNZ(res); cpu.c = cpu.sc; cpu.cycles++; break;
    case 0x4: res = cpu.shiftReg(2, a, b); cpu.r[rd] = res; cpu.setNZ(res); cpu.c = cpu.sc; cpu.cycles++; break;
    case 0x5: cpu.r[rd] = cpu.adc(a, b, true); break;
    case 0x6: cpu.r[rd] = cpu.sbc(a, b, true); break;
    case 0x7: res = cpu.shiftReg(3, a, b); cpu.r[rd] = res; cpu.setNZ(res); cpu.c = cpu.sc; cpu.cycles++; break;
    case 0x8: cpu.setNZ(a & b); break;
    case 0x9: cpu.r[rd] = cpu.sub(0, b, true); break;
    case 0xA: cpu.sub(a, b, true); break;
    case 0xB: cpu.add(a, b, true); break;
    case 0xC: res = a | b; cpu.r[rd] = res; cpu.setNZ(res); break;
    case 0xD: res = Math.imul(a, b); mulCycles(cpu, a); cpu.r[rd] = res; cpu.setNZ(res); break;
    case 0xE: res = a & ~b; cpu.r[rd] = res; cpu.setNZ(res); break;
    default: res = ~b; cpu.r[rd] = res; cpu.setNZ(res); break;
  }
}
function tHiReg(cpu, op) {
  const o = (op >>> 8) & 3;
  const rs = (op >>> 3) & 15;
  const rd = (op & 7) | ((op >>> 4) & 8);
  const b = cpu.r[rs];
  switch (o) {
    case 0: cpu.setReg(rd, (cpu.r[rd] + b) | 0); break;
    case 1: cpu.sub(cpu.r[rd], b, true); break;
    case 2: cpu.setReg(rd, b); break;
    default: cpu.branchExchange(b); break; // BX (BLX not on v4)
  }
}
function tLdrPC(cpu, op) {
  const rd = (op >>> 8) & 7;
  const addr = ((cpu.r[15] & ~3) + ((op & 0xFF) << 2)) | 0;
  cpu.r[rd] = cpu.bus.read32(addr) | 0;
  cpu.cycles += 2;
}
function tLdStReg(cpu, op) {
  const o = (op >>> 9) & 7, ro = (op >>> 6) & 7, rb = (op >>> 3) & 7, rd = op & 7;
  const addr = (cpu.r[rb] + cpu.r[ro]) | 0;
  switch (o) {
    case 0: cpu.bus.write32(addr & ~3, cpu.r[rd]); cpu.cycles += 1; break;          // STR
    case 1: cpu.bus.write16(addr & ~1, cpu.r[rd] & 0xFFFF); cpu.cycles += 1; break; // STRH
    case 2: cpu.bus.write8(addr, cpu.r[rd] & 0xFF); cpu.cycles += 1; break;         // STRB
    case 3: cpu.r[rd] = cpu.ldrs8(addr); cpu.cycles += 2; break;                    // LDSB
    case 4: cpu.r[rd] = cpu.ldr32(addr); cpu.cycles += 2; break;                    // LDR
    case 5: cpu.r[rd] = cpu.ldr16(addr); cpu.cycles += 2; break;                    // LDRH
    case 6: cpu.r[rd] = cpu.bus.read8(addr) & 0xFF; cpu.cycles += 2; break;         // LDRB
    default: cpu.r[rd] = cpu.ldrs16(addr); cpu.cycles += 2; break;                  // LDSH
  }
}
function tLdStImm(cpu, op) {
  const b = (op >>> 12) & 1, l = (op >>> 11) & 1, off = (op >>> 6) & 31, rb = (op >>> 3) & 7, rd = op & 7;
  if (b) {
    const addr = (cpu.r[rb] + off) | 0;
    if (l) { cpu.r[rd] = cpu.bus.read8(addr) & 0xFF; cpu.cycles += 2; }
    else { cpu.bus.write8(addr, cpu.r[rd] & 0xFF); cpu.cycles += 1; }
  } else {
    const addr = (cpu.r[rb] + (off << 2)) | 0;
    if (l) { cpu.r[rd] = cpu.ldr32(addr); cpu.cycles += 2; }
    else { cpu.bus.write32(addr & ~3, cpu.r[rd]); cpu.cycles += 1; }
  }
}
function tLdStH(cpu, op) {
  const l = (op >>> 11) & 1, off = ((op >>> 6) & 31) << 1, rb = (op >>> 3) & 7, rd = op & 7;
  const addr = (cpu.r[rb] + off) | 0;
  if (l) { cpu.r[rd] = cpu.ldr16(addr); cpu.cycles += 2; }
  else { cpu.bus.write16(addr & ~1, cpu.r[rd] & 0xFFFF); cpu.cycles += 1; }
}
function tLdStSP(cpu, op) {
  const l = (op >>> 11) & 1, rd = (op >>> 8) & 7;
  const addr = (cpu.r[13] + ((op & 0xFF) << 2)) | 0;
  if (l) { cpu.r[rd] = cpu.ldr32(addr); cpu.cycles += 2; }
  else { cpu.bus.write32(addr & ~3, cpu.r[rd]); cpu.cycles += 1; }
}
function tLoadAddr(cpu, op) {
  const sp = (op >>> 11) & 1, rd = (op >>> 8) & 7;
  const base = sp ? cpu.r[13] : (cpu.r[15] & ~3);
  cpu.r[rd] = (base + ((op & 0xFF) << 2)) | 0;
}
function tAddSP(cpu, op) {
  const off = (op & 0x7F) << 2;
  cpu.r[13] = (op & 0x80) ? (cpu.r[13] - off) | 0 : (cpu.r[13] + off) | 0;
}
function tPushPop(cpu, op) {
  const l = (op >>> 11) & 1, rbit = (op >>> 8) & 1, list = op & 0xFF;
  if (l) {
    let addr = cpu.r[13];
    for (let i = 0; i < 8; i++) {
      if (list & (1 << i)) { cpu.r[i] = cpu.bus.read32(addr & ~3) | 0; addr = (addr + 4) | 0; cpu.cycles++; }
    }
    if (rbit) {
      const v = cpu.bus.read32(addr & ~3) | 0; addr = (addr + 4) | 0;
      cpu.r[13] = addr;
      cpu.branchTo(v); // v4T: POP {pc} stays in Thumb
      cpu.cycles++;
    } else cpu.r[13] = addr;
    cpu.cycles += 1;
  } else {
    let count = rbit;
    for (let x = list; x; x &= x - 1) count++;
    let addr = (cpu.r[13] - count * 4) | 0;
    cpu.r[13] = addr;
    for (let i = 0; i < 8; i++) {
      if (list & (1 << i)) { cpu.bus.write32(addr & ~3, cpu.r[i]); addr = (addr + 4) | 0; cpu.cycles++; }
    }
    if (rbit) { cpu.bus.write32(addr & ~3, cpu.r[14]); cpu.cycles++; }
  }
}
function tLdStM(cpu, op) {
  const l = (op >>> 11) & 1, rb = (op >>> 8) & 7, list = op & 0xFF;
  let addr = cpu.r[rb];
  if (list === 0) { // ARM7 quirk: transfers r15, base += 0x40
    if (l) cpu.branchTo(cpu.bus.read32(addr & ~3));
    else cpu.bus.write32(addr & ~3, (cpu.r[15] + 2) | 0);
    cpu.r[rb] = (addr + 0x40) | 0;
    return;
  }
  let count = 0;
  for (let x = list; x; x &= x - 1) count++;
  const newBase = (addr + count * 4) | 0;
  if (l) {
    for (let i = 0; i < 8; i++) {
      if (list & (1 << i)) { cpu.r[i] = cpu.bus.read32(addr & ~3) | 0; addr = (addr + 4) | 0; cpu.cycles++; }
    }
    if (!(list & (1 << rb))) cpu.r[rb] = newBase;
    cpu.cycles += 1;
  } else {
    let first = true;
    for (let i = 0; i < 8; i++) {
      if (list & (1 << i)) {
        const v = (i === rb && !first) ? newBase : cpu.r[i];
        cpu.bus.write32(addr & ~3, v); addr = (addr + 4) | 0; cpu.cycles++;
        first = false;
      }
    }
    cpu.r[rb] = newBase;
  }
}
function tBcond(cpu, op) {
  const cond = (op >>> 8) & 15;
  if (cond === 0xF) { // SWI
    if (cpu.onSWI && cpu.onSWI(cpu, op & 0xFF)) return;
    cpu.exception(0x08, MODE_SVC, cpu.pc, false);
    return;
  }
  if (cond === 0xE) { thumbUndefined(cpu, op); return; }
  if (!cpu.condPassed(cond)) return;
  const off = ((op & 0xFF) << 24) >> 23;
  cpu.branchTo((cpu.r[15] + off) | 0);
}
function tB(cpu, op) {
  const off = ((op & 0x7FF) << 21) >> 20;
  cpu.branchTo((cpu.r[15] + off) | 0);
}
function tBL(cpu, op) {
  if (op & 0x0800) { // second half
    const target = (cpu.r[14] + ((op & 0x7FF) << 1)) | 0;
    cpu.r[14] = cpu.pc | 1;
    cpu.branchTo(target);
  } else { // first half
    const off = ((op & 0x7FF) << 21) >> 9;
    cpu.r[14] = (cpu.r[15] + off) | 0;
  }
}
function thumbUndefined(cpu, op) {
  if (cpu.onUndefined && cpu.onUndefined(cpu, op)) return;
  cpu.exception(0x04, MODE_UND, cpu.pc, false);
}

const THUMB = new Array(1024);
(function buildThumb() {
  for (let k = 0; k < 1024; k++) {
    const op = k << 6;
    let h = thumbUndefined;
    if ((op & 0xF800) === 0x1800) h = tAddSub;
    else if ((op & 0xE000) === 0x0000) h = tShiftImm;
    else if ((op & 0xE000) === 0x2000) h = tImm;
    else if ((op & 0xFC00) === 0x4000) h = tALU;
    else if ((op & 0xFC00) === 0x4400) h = tHiReg;
    else if ((op & 0xF800) === 0x4800) h = tLdrPC;
    else if ((op & 0xF000) === 0x5000) h = tLdStReg;
    else if ((op & 0xE000) === 0x6000) h = tLdStImm;
    else if ((op & 0xF000) === 0x8000) h = tLdStH;
    else if ((op & 0xF000) === 0x9000) h = tLdStSP;
    else if ((op & 0xF000) === 0xA000) h = tLoadAddr;
    else if ((op & 0xFF00) === 0xB000) h = tAddSP;
    else if ((op & 0xF600) === 0xB400) h = tPushPop;
    else if ((op & 0xF000) === 0xC000) h = tLdStM;
    else if ((op & 0xF000) === 0xD000) h = tBcond;
    else if ((op & 0xF800) === 0xE000) h = tB;
    else if ((op & 0xF000) === 0xF000) h = tBL;
    THUMB[k] = h;
  }
})();
