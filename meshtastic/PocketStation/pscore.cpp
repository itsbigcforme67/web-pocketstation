// PocketStation emulator core - see pscore.h
#include "pscore.h"
#include <string.h>

#if defined(ESP_PLATFORM) && !defined(PS_NO_IRAM)
#include "esp_attr.h"
#define HOT IRAM_ATTR
#else
#define HOT
#endif

namespace ps {

enum : uint32_t { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13, MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };
static const uint32_t INT_COM = 1u << 6, INT_T0 = 1u << 7, INT_T1 = 1u << 8, INT_RTC = 1u << 9, INT_T2 = 1u << 13;
static const uint32_t FIQ_MASK = INT_COM | INT_T2;
static const uint32_t LEVEL_MASK = 0x1F | (1u << 10) | (1u << 11);
static const uint32_t TIMER_DIV[4] = {2, 32, 512, 2};
static const uint32_t RTC_PAUSED_PERIOD = MAX_CLOCK / 4096;  // 1952 exactly

static inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
static inline uint32_t mixWord(uint32_t a, uint32_t v) {
  uint32_t x = v ^ (a * 0x9E3779B1u);
  x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
  return x;
}
static inline int toBCD(int n) { return ((n / 10) << 4) | (n % 10); }
static inline int bankIndex(uint32_t m) {
  switch (m) { case MODE_FIQ: return 1; case MODE_IRQ: return 2; case MODE_SVC: return 3; case MODE_ABT: return 4; case MODE_UND: return 5; default: return 0; }
}
static inline int popcount16(uint32_t x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }

// =====================================================================================
// CPU
// =====================================================================================
typedef void (*ArmFn)(Cpu&, uint32_t);
#if defined(PS_SMALL_TABLES)
// One byte per entry (an index into HANDLER) instead of a 4-byte pointer: 5 KiB, not 20 KiB.
static uint8_t* ARM = nullptr;
static uint8_t* THUMB = nullptr;
static ArmFn HANDLER[160];
static int handlerCount = 0;
static uint8_t handlerIndex(ArmFn f) {
  for (int i = 0; i < handlerCount; i++) if (HANDLER[i] == f) return (uint8_t)i;
  if (handlerCount == (int)(sizeof HANDLER / sizeof HANDLER[0])) return 0;  // HANDLER[0] is armUndefined
  HANDLER[handlerCount] = f;
  return (uint8_t)handlerCount++;
}
#define TABLE_FN(f) handlerIndex(f)
#define CALL_ARM(k) HANDLER[ARM[k]]
#define CALL_THUMB(k) HANDLER[THUMB[k]]
#else
#if defined(PS_HEAP_TABLES)
static ArmFn* ARM = nullptr;
static ArmFn* THUMB = nullptr;
#else
static ArmFn ARM[4096];
static ArmFn THUMB[1024];
#endif
#define TABLE_FN(f) (f)
#define CALL_ARM(k) ARM[k]
#define CALL_THUMB(k) THUMB[k]
#endif
static bool tablesBuilt = false;
static void buildTables();

void Cpu::reset() {
  memset(r, 0, sizeof r); memset(bankR13, 0, sizeof bankR13); memset(bankR14, 0, sizeof bankR14);
  memset(spsrBank, 0, sizeof spsrBank); memset(usrR8, 0, sizeof usrR8); memset(fiqR8, 0, sizeof fiqR8);
  n = z = c = v = 0; mode = MODE_SVC; iflag = 1; fflag = 1; t = 0; pc = 0;
  cycles = 0; irqLine = fiqLine = stopRequested = false; sc = 0;
  idle = false; idlePc = 0xFFFFFFFF; idleSkips = 0; lastIdleAt = 0;
  for (auto& sl : idleSlots) { sl.pc = 0xFFFFFFFF; sl.writes = sl.h1 = sl.h2 = 0; }
}
uint32_t Cpu::getCPSR() const {
  return (n << 31) | (z << 30) | (c << 29) | (v << 28) | (iflag << 7) | (fflag << 6) | (t << 5) | mode;
}
void Cpu::setCPSR(uint32_t val) {
  n = (val >> 31) & 1; z = (val >> 30) & 1; c = (val >> 29) & 1; v = (val >> 28) & 1;
  iflag = (val >> 7) & 1; fflag = (val >> 6) & 1; t = (val >> 5) & 1;
  switchMode(val & 0x1F);
}
void Cpu::switchMode(uint32_t nm) {
  uint32_t old = mode;
  if (old == nm) return;
  int ob = bankIndex(old), nb = bankIndex(nm);
  if (ob != nb) {
    bankR13[ob] = r[13]; bankR14[ob] = r[14];
    if ((old == MODE_FIQ) != (nm == MODE_FIQ)) {
      int32_t* save = old == MODE_FIQ ? fiqR8 : usrR8;
      int32_t* load = nm == MODE_FIQ ? fiqR8 : usrR8;
      for (int i = 0; i < 5; i++) { save[i] = r[8 + i]; r[8 + i] = load[i]; }
    }
    r[13] = bankR13[nb]; r[14] = bankR14[nb];
  }
  mode = nm;
}
uint32_t Cpu::getSPSR() const { int b = bankIndex(mode); return b ? (uint32_t)spsrBank[b] : getCPSR(); }
void Cpu::setSPSR(uint32_t val) { int b = bankIndex(mode); if (b) spsrBank[b] = val; }
int32_t Cpu::getUserReg(int i) const {
  if (i < 8 || i == 15) return r[i];
  if (mode == MODE_FIQ && i < 13) return usrR8[i - 8];
  if (i < 13) return r[i];
  if (bankIndex(mode) == 0) return r[i];
  return i == 13 ? bankR13[0] : bankR14[0];
}
void Cpu::setUserReg(int i, int32_t val) {
  if (i < 8 || i == 15) { r[i] = val; return; }
  if (mode == MODE_FIQ && i < 13) { usrR8[i - 8] = val; return; }
  if (i < 13 || bankIndex(mode) == 0) { r[i] = val; return; }
  if (i == 13) bankR13[0] = val; else bankR14[0] = val;
}
void Cpu::exception(uint32_t vector, uint32_t nm, uint32_t lr, bool disableFiq) {
  uint32_t cpsr = getCPSR();
  switchMode(nm);
  spsrBank[bankIndex(nm)] = cpsr;
  r[14] = lr; t = 0; iflag = 1;
  if (disableFiq) fflag = 1;
  pc = vector; cycles += 3;
  for (auto& sl : idleSlots) sl.pc = 0xFFFFFFFF;
}
HOT uint32_t Cpu::run(uint32_t budget) {
  uint32_t start = cycles, end = start + budget;
  while ((int32_t)(cycles - end) < 0) {
    if (fiqLine && !fflag) exception(0x1C, MODE_FIQ, pc + 4, true);
    else if (irqLine && !iflag) exception(0x18, MODE_IRQ, pc + 4, false);
    step();
    if (stopRequested) { stopRequested = false; break; }
  }
  return cycles - start;
}
HOT void Cpu::step() {
  uint32_t p = pc;
  if (t) {
    uint32_t op = bus->read16(p & ~1u);
    pc = p + 2; r[15] = p + 4; cycles += 1;
    CALL_THUMB(op >> 6)(*this, op);
  } else {
    uint32_t op = bus->read32(p & ~3u);
    pc = p + 4; r[15] = p + 8; cycles += 1;
    if (!condPassed(op >> 28)) return;
    CALL_ARM(((op >> 16) & 0xFF0) | ((op >> 4) & 0xF))(*this, op);
  }
}
void Cpu::checkIdle(uint32_t at) {
  uint32_t h = 0, h2 = 0x9E3779B9u;
  for (int i = 0; i < 15; i++) { h ^= (uint32_t)r[i]; h = (h << 5) | (h >> 27); h2 += (uint32_t)r[i] * (2 * i + 1); }
  uint32_t cpsr = getCPSR();
  h ^= cpsr; h2 ^= cpsr ^ bus->ramHash;
  IdleSlot& sl = idleSlots[(at >> 1) & 7];
  if (sl.pc == at && sl.writes == bus->writeCount && sl.h1 == h && sl.h2 == h2) {
    if (bus->idleSkip) { idle = true; stopRequested = true; idleSkips++; lastIdleAt = at; }
    return;
  }
  sl.pc = at; sl.writes = bus->writeCount; sl.h1 = h; sl.h2 = h2;
}

bool Cpu::condPassed(uint32_t cond) const {
  switch (cond) {
    case 0xE: return true;
    case 0x0: return z; case 0x1: return !z;
    case 0x2: return c; case 0x3: return !c;
    case 0x4: return n; case 0x5: return !n;
    case 0x6: return v; case 0x7: return !v;
    case 0x8: return c && !z; case 0x9: return !c || z;
    case 0xA: return n == v; case 0xB: return n != v;
    case 0xC: return !z && n == v; case 0xD: return z || n != v;
    default: return false;
  }
}
int32_t Cpu::ldr32(uint32_t a) {
  uint32_t val = bus->read32(a & ~3u);
  uint32_t rot = (a & 3) << 3;
  return rot ? (int32_t)((val >> rot) | (val << (32 - rot))) : (int32_t)val;
}
int32_t Cpu::ldr16(uint32_t a) {
  uint32_t val = bus->read16(a & ~1u) & 0xFFFF;
  return (a & 1) ? (int32_t)((val >> 8) | (val << 24)) : (int32_t)val;
}
int32_t Cpu::ldrs16(uint32_t a) {
  if (a & 1) return (int32_t)(int8_t)bus->read8(a);
  return (int32_t)(int16_t)bus->read16(a);
}
int32_t Cpu::ldrs8(uint32_t a) { return (int32_t)(int8_t)bus->read8(a); }

int32_t Cpu::add(int32_t a, int32_t b, bool s) {
  uint32_t res = (uint32_t)a + (uint32_t)b;
  if (s) { n = res >> 31; z = res == 0; c = res < (uint32_t)a; v = (((a ^ res) & (b ^ res)) >> 31) & 1; }
  return (int32_t)res;
}
int32_t Cpu::adc(int32_t a, int32_t b, bool s) {
  uint64_t wide = (uint64_t)(uint32_t)a + (uint32_t)b + c;
  uint32_t res = (uint32_t)wide;
  if (s) { n = res >> 31; z = res == 0; c = (uint32_t)(wide >> 32); v = (((a ^ res) & (b ^ res)) >> 31) & 1; }
  return (int32_t)res;
}
int32_t Cpu::sub(int32_t a, int32_t b, bool s) {
  uint32_t res = (uint32_t)a - (uint32_t)b;
  if (s) { n = res >> 31; z = res == 0; c = (uint32_t)a >= (uint32_t)b; v = (((a ^ b) & (a ^ res)) >> 31) & 1; }
  return (int32_t)res;
}
int32_t Cpu::sbc(int32_t a, int32_t b, bool s) {
  uint32_t borrow = 1 - c;
  uint32_t res = (uint32_t)a - (uint32_t)b - borrow;
  if (s) { n = res >> 31; z = res == 0; c = (uint64_t)(uint32_t)a >= (uint64_t)(uint32_t)b + borrow; v = (((a ^ b) & (a ^ res)) >> 31) & 1; }
  return (int32_t)res;
}
int32_t Cpu::shiftImm(uint32_t type, int32_t val, uint32_t amt) {
  uint32_t u = (uint32_t)val;
  switch (type) {
    case 0: if (!amt) { sc = c; return val; } sc = (u >> (32 - amt)) & 1; return (int32_t)(u << amt);
    case 1: if (!amt) { sc = u >> 31; return 0; } sc = (u >> (amt - 1)) & 1; return (int32_t)(u >> amt);
    case 2: if (!amt) { sc = u >> 31; return val >> 31; } sc = (u >> (amt - 1)) & 1; return val >> amt;
    default:
      if (!amt) { sc = u & 1; return (int32_t)((c << 31) | (u >> 1)); }
      sc = (u >> (amt - 1)) & 1; return (int32_t)((u >> amt) | (u << (32 - amt)));
  }
}
int32_t Cpu::shiftReg(uint32_t type, int32_t val, uint32_t amt) {
  amt &= 0xFF;
  uint32_t u = (uint32_t)val;
  if (!amt) { sc = c; return val; }
  switch (type) {
    case 0: if (amt < 32) { sc = (u >> (32 - amt)) & 1; return (int32_t)(u << amt); } sc = amt == 32 ? (u & 1) : 0; return 0;
    case 1: if (amt < 32) { sc = (u >> (amt - 1)) & 1; return (int32_t)(u >> amt); } sc = amt == 32 ? (u >> 31) : 0; return 0;
    case 2: if (amt < 32) { sc = (u >> (amt - 1)) & 1; return val >> amt; } sc = u >> 31; return val >> 31;
    default: {
      uint32_t a = amt & 31;
      if (!a) { sc = u >> 31; return val; }
      sc = (u >> (a - 1)) & 1; return (int32_t)((u >> a) | (u << (32 - a)));
    }
  }
}

// ---------------- ARM handlers ----------------
static void armUndefined(Cpu& c, uint32_t op) {
  if (op == 0xE6000010u) return;  // debug TTY trap: ignore
  c.exception(0x04, MODE_UND, c.pc, false);
}
static void armSWI(Cpu& c, uint32_t) { c.exception(0x08, MODE_SVC, c.pc, false); }
static void armB(Cpu& c, uint32_t op) {
  int32_t off = ((int32_t)(op << 8)) >> 6;
  if (op & 0x01000000) c.r[14] = c.pc;
  else if (off < 0) c.checkIdle(c.pc - 4);
  c.branchTo(c.r[15] + off);
}
static void armBX(Cpu& c, uint32_t op) { c.branchExchange(c.r[op & 15]); }
static void armMRS(Cpu& c, uint32_t op) { c.r[(op >> 12) & 15] = (op & 0x00400000) ? c.getSPSR() : c.getCPSR(); }
static void armMSR(Cpu& c, uint32_t op) {
  uint32_t val;
  if (op & 0x02000000) { uint32_t rot = ((op >> 8) & 15) << 1, imm = op & 0xFF; val = rot ? (imm >> rot) | (imm << (32 - rot)) : imm; }
  else val = c.r[op & 15];
  uint32_t mask = 0;
  if (op & 0x00010000) mask |= 0x000000FF;
  if (op & 0x00020000) mask |= 0x0000FF00;
  if (op & 0x00040000) mask |= 0x00FF0000;
  if (op & 0x00080000) mask |= 0xFF000000;
  if (op & 0x00400000) {
    if (c.mode == MODE_USR || c.mode == MODE_SYS) return;
    c.setSPSR((c.getSPSR() & ~mask) | (val & mask));
  } else {
    if (c.mode == MODE_USR) mask &= 0xFF000000;
    uint32_t old = c.getCPSR();
    uint32_t nv = (old & ~mask) | (val & mask);
    nv = (nv & ~0x20u) | (old & 0x20);
    c.setCPSR(nv);
  }
}
static inline void dataProc(Cpu& c, uint32_t op, int32_t o2, uint32_t sc) {
  uint32_t opcode = (op >> 21) & 15;
  bool s = (op >> 20) & 1;
  uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  int32_t a = c.r[rn];
  int32_t res;
  bool logical = false;
  bool sf = s && rd != 15;
  switch (opcode) {
    case 0x0: res = a & o2; logical = true; break;
    case 0x1: res = a ^ o2; logical = true; break;
    case 0x2: res = c.sub(a, o2, sf); break;
    case 0x3: res = c.sub(o2, a, sf); break;
    case 0x4: res = c.add(a, o2, sf); break;
    case 0x5: res = c.adc(a, o2, sf); break;
    case 0x6: res = c.sbc(a, o2, sf); break;
    case 0x7: res = c.sbc(o2, a, sf); break;
    case 0x8: c.setNZ(a & o2); c.c = sc; return;
    case 0x9: c.setNZ(a ^ o2); c.c = sc; return;
    case 0xA: c.sub(a, o2, true); return;
    case 0xB: c.add(a, o2, true); return;
    case 0xC: res = a | o2; logical = true; break;
    case 0xD: res = o2; logical = true; break;
    case 0xE: res = a & ~o2; logical = true; break;
    default: res = ~o2; logical = true; break;
  }
  if (rd == 15) {
    if (s) { c.setCPSR(c.getSPSR()); c.pc = c.t ? (res & ~1u) : (res & ~3u); c.cycles += 2; }
    else c.branchTo(res);
    return;
  }
  c.r[rd] = res;
  if (s && logical) { c.setNZ(res); c.c = sc; }
}
static void armDPImm(Cpu& c, uint32_t op) {
  uint32_t rot = ((op >> 8) & 15) << 1, imm = op & 0xFF;
  uint32_t val, sc;
  if (rot) { val = (imm >> rot) | (imm << (32 - rot)); sc = val >> 31; } else { val = imm; sc = c.c; }
  dataProc(c, op, (int32_t)val, sc);
}
static void armDPRegImmShift(Cpu& c, uint32_t op) {
  int32_t val = c.shiftImm((op >> 5) & 3, c.r[op & 15], (op >> 7) & 31);
  dataProc(c, op, val, c.sc);
}
static void armDPRegRegShift(Cpu& c, uint32_t op) {
  c.r[15] += 4;
  int32_t val = c.shiftReg((op >> 5) & 3, c.r[op & 15], c.r[(op >> 8) & 15]);
  c.cycles += 1;
  dataProc(c, op, val, c.sc);
}
static inline void mulCycles(Cpu& c, int32_t rs) {
  uint32_t v = (uint32_t)rs;
  if ((v & 0xFFFFFF00) == 0 || (v & 0xFFFFFF00) == 0xFFFFFF00) c.cycles += 1;
  else if ((v & 0xFFFF0000) == 0 || (v & 0xFFFF0000) == 0xFFFF0000) c.cycles += 2;
  else if ((v & 0xFF000000) == 0 || (v & 0xFF000000) == 0xFF000000) c.cycles += 3;
  else c.cycles += 4;
}
static void armMUL(Cpu& c, uint32_t op) {
  uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
  int32_t res = (int32_t)((uint32_t)c.r[rm] * (uint32_t)c.r[rs]);
  if (op & 0x00200000) { res = (int32_t)((uint32_t)res + (uint32_t)c.r[rn]); c.cycles += 1; }
  mulCycles(c, c.r[rs]);
  c.r[rd] = res;
  if (op & 0x00100000) c.setNZ(res);
}
static void armMULL(Cpu& c, uint32_t op) {
  uint32_t rdHi = (op >> 16) & 15, rdLo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
  uint64_t res;
  if (op & 0x00400000) res = (uint64_t)((int64_t)c.r[rm] * (int64_t)c.r[rs]);
  else res = (uint64_t)(uint32_t)c.r[rm] * (uint32_t)c.r[rs];
  if (op & 0x00200000) { res += ((uint64_t)(uint32_t)c.r[rdHi] << 32) | (uint32_t)c.r[rdLo]; c.cycles += 1; }
  int32_t lo = (int32_t)(uint32_t)res, hi = (int32_t)(uint32_t)(res >> 32);
  c.r[rdLo] = lo; c.r[rdHi] = hi;
  mulCycles(c, c.r[rs]); c.cycles += 1;
  if (op & 0x00100000) { c.n = (uint32_t)hi >> 31; c.z = (hi == 0 && lo == 0); }
}
static void armSWP(Cpu& c, uint32_t op) {
  uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
  uint32_t addr = c.r[rn]; int32_t src = c.r[rm];
  if (op & 0x00400000) { uint32_t v = c.bus->read8(addr) & 0xFF; c.bus->write8(addr, src & 0xFF); c.r[rd] = v; }
  else { int32_t v = c.ldr32(addr); c.bus->write32(addr & ~3u, src); c.r[rd] = v; }
  c.cycles += 3;
}
static void armHalf(Cpu& c, uint32_t op) {
  uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, i = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
  uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
  uint32_t off = i ? (((op >> 4) & 0xF0) | (op & 15)) : (uint32_t)c.r[op & 15];
  uint32_t base = c.r[rn];
  uint32_t offAddr = u ? base + off : base - off;
  uint32_t addr = p ? offAddr : base;
  if (l) {
    int32_t v;
    if (sh == 1) v = c.ldr16(addr); else if (sh == 2) v = c.ldrs8(addr); else v = c.ldrs16(addr);
    if (!p || w) c.r[rn] = offAddr;
    c.setReg(rd, v);
    c.cycles += 2;
  } else {
    if (sh == 1) { uint32_t v = c.r[rd]; if (rd == 15) v += 4; c.bus->write16(addr & ~1u, v & 0xFFFF); }
    if (!p || w) c.r[rn] = offAddr;
    c.cycles += 1;
  }
}
static void armSDT(Cpu& c, uint32_t op) {
  uint32_t i = (op >> 25) & 1, p = (op >> 24) & 1, u = (op >> 23) & 1, b = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
  uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  uint32_t off = i ? (uint32_t)c.shiftImm((op >> 5) & 3, c.r[op & 15], (op >> 7) & 31) : (op & 0xFFF);
  uint32_t base = c.r[rn];
  uint32_t offAddr = u ? base + off : base - off;
  uint32_t addr = p ? offAddr : base;
  if (l) {
    int32_t v = b ? (int32_t)(c.bus->read8(addr) & 0xFF) : c.ldr32(addr);
    if (!p || w) c.r[rn] = offAddr;
    c.setReg(rd, v);
    c.cycles += 2;
  } else {
    uint32_t v = c.r[rd]; if (rd == 15) v += 4;
    if (b) c.bus->write8(addr, v & 0xFF); else c.bus->write32(addr & ~3u, v);
    if (!p || w) c.r[rn] = offAddr;
    c.cycles += 1;
  }
}
static void armBlock(Cpu& c, uint32_t op) {
  uint32_t p = (op >> 24) & 1, u = (op >> 23) & 1, s = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
  uint32_t rn = (op >> 16) & 15;
  uint32_t list = op & 0xFFFF;
  uint32_t base = c.r[rn];
  int count = popcount16(list);
  bool emptyList = false;
  if (!list) { list = 0x8000; count = 16; emptyList = true; }
  uint32_t addr;
  if (u) addr = p ? base + 4 : base; else addr = p ? base - count * 4 : base - count * 4 + 4;
  uint32_t newBase = u ? base + count * 4 : base - count * 4;
  bool userBank = s && (!l || !(list & 0x8000));
  c.cycles += count + (l ? 1 : 0);
  if (l) {
    if (w) c.r[rn] = newBase;
    for (int i = 0; i < 16; i++) {
      if (!(list & (1u << i))) continue;
      int32_t v = (int32_t)c.bus->read32(addr & ~3u); addr += 4;
      if (emptyList && i == 15) { c.branchTo(v); continue; }
      if (userBank) c.setUserReg(i, v);
      else if (i == 15) {
        if (s) { c.setCPSR(c.getSPSR()); c.pc = c.t ? (v & ~1u) : (v & ~3u); c.cycles += 2; }
        else c.branchTo(v);
      } else c.r[i] = v;
    }
  } else {
    bool first = true;
    for (int i = 0; i < 16; i++) {
      if (!(list & (1u << i))) continue;
      uint32_t v;
      if ((uint32_t)i == rn) v = first ? base : newBase;
      else if (userBank) v = c.getUserReg(i);
      else v = c.r[i];
      if (i == 15) v = c.r[15] + 4;
      c.bus->write32(addr & ~3u, v); addr += 4;
      first = false;
    }
    if (w) c.r[rn] = newBase;
  }
}

// ---------------- THUMB handlers ----------------
static void thumbUndefined(Cpu& c, uint32_t) { c.exception(0x04, MODE_UND, c.pc, false); }
static void tShiftImm(Cpu& c, uint32_t op) {
  int32_t res = c.shiftImm((op >> 11) & 3, c.r[(op >> 3) & 7], (op >> 6) & 31);
  c.r[op & 7] = res; c.setNZ(res); c.c = c.sc;
}
static void tAddSub(Cpu& c, uint32_t op) {
  uint32_t imm = (op >> 10) & 1, sub = (op >> 9) & 1, rn = (op >> 6) & 7, rs = (op >> 3) & 7, rd = op & 7;
  int32_t b = imm ? (int32_t)rn : c.r[rn];
  c.r[rd] = sub ? c.sub(c.r[rs], b, true) : c.add(c.r[rs], b, true);
}
static void tImm(Cpu& c, uint32_t op) {
  uint32_t o = (op >> 11) & 3, rd = (op >> 8) & 7; int32_t imm = op & 0xFF;
  switch (o) {
    case 0: c.r[rd] = imm; c.setNZ(imm); break;
    case 1: c.sub(c.r[rd], imm, true); break;
    case 2: c.r[rd] = c.add(c.r[rd], imm, true); break;
    default: c.r[rd] = c.sub(c.r[rd], imm, true); break;
  }
}
static void tALU(Cpu& c, uint32_t op) {
  uint32_t o = (op >> 6) & 15, rs = (op >> 3) & 7, rd = op & 7;
  int32_t a = c.r[rd], b = c.r[rs], res;
  switch (o) {
    case 0x0: res = a & b; c.r[rd] = res; c.setNZ(res); break;
    case 0x1: res = a ^ b; c.r[rd] = res; c.setNZ(res); break;
    case 0x2: res = c.shiftReg(0, a, b); c.r[rd] = res; c.setNZ(res); c.c = c.sc; c.cycles++; break;
    case 0x3: res = c.shiftReg(1, a, b); c.r[rd] = res; c.setNZ(res); c.c = c.sc; c.cycles++; break;
    case 0x4: res = c.shiftReg(2, a, b); c.r[rd] = res; c.setNZ(res); c.c = c.sc; c.cycles++; break;
    case 0x5: c.r[rd] = c.adc(a, b, true); break;
    case 0x6: c.r[rd] = c.sbc(a, b, true); break;
    case 0x7: res = c.shiftReg(3, a, b); c.r[rd] = res; c.setNZ(res); c.c = c.sc; c.cycles++; break;
    case 0x8: c.setNZ(a & b); break;
    case 0x9: c.r[rd] = c.sub(0, b, true); break;
    case 0xA: c.sub(a, b, true); break;
    case 0xB: c.add(a, b, true); break;
    case 0xC: res = a | b; c.r[rd] = res; c.setNZ(res); break;
    case 0xD: res = (int32_t)((uint32_t)a * (uint32_t)b); mulCycles(c, a); c.r[rd] = res; c.setNZ(res); break;
    case 0xE: res = a & ~b; c.r[rd] = res; c.setNZ(res); break;
    default: res = ~b; c.r[rd] = res; c.setNZ(res); break;
  }
}
static void tHiReg(Cpu& c, uint32_t op) {
  uint32_t o = (op >> 8) & 3, rs = (op >> 3) & 15, rd = (op & 7) | ((op >> 4) & 8);
  int32_t b = c.r[rs];
  switch (o) {
    case 0: c.setReg(rd, (int32_t)((uint32_t)c.r[rd] + (uint32_t)b)); break;
    case 1: c.sub(c.r[rd], b, true); break;
    case 2: c.setReg(rd, b); break;
    default: c.branchExchange(b); break;
  }
}
static void tLdrPC(Cpu& c, uint32_t op) {
  uint32_t addr = (c.r[15] & ~3u) + ((op & 0xFF) << 2);
  c.r[(op >> 8) & 7] = (int32_t)c.bus->read32(addr); c.cycles += 2;
}
static void tLdStReg(Cpu& c, uint32_t op) {
  uint32_t o = (op >> 9) & 7, ro = (op >> 6) & 7, rb = (op >> 3) & 7, rd = op & 7;
  uint32_t addr = (uint32_t)c.r[rb] + (uint32_t)c.r[ro];
  switch (o) {
    case 0: c.bus->write32(addr & ~3u, c.r[rd]); c.cycles += 1; break;
    case 1: c.bus->write16(addr & ~1u, c.r[rd] & 0xFFFF); c.cycles += 1; break;
    case 2: c.bus->write8(addr, c.r[rd] & 0xFF); c.cycles += 1; break;
    case 3: c.r[rd] = c.ldrs8(addr); c.cycles += 2; break;
    case 4: c.r[rd] = c.ldr32(addr); c.cycles += 2; break;
    case 5: c.r[rd] = c.ldr16(addr); c.cycles += 2; break;
    case 6: c.r[rd] = c.bus->read8(addr) & 0xFF; c.cycles += 2; break;
    default: c.r[rd] = c.ldrs16(addr); c.cycles += 2; break;
  }
}
static void tLdStImm(Cpu& c, uint32_t op) {
  uint32_t b = (op >> 12) & 1, l = (op >> 11) & 1, off = (op >> 6) & 31, rb = (op >> 3) & 7, rd = op & 7;
  if (b) {
    uint32_t addr = (uint32_t)c.r[rb] + off;
    if (l) { c.r[rd] = c.bus->read8(addr) & 0xFF; c.cycles += 2; } else { c.bus->write8(addr, c.r[rd] & 0xFF); c.cycles += 1; }
  } else {
    uint32_t addr = (uint32_t)c.r[rb] + (off << 2);
    if (l) { c.r[rd] = c.ldr32(addr); c.cycles += 2; } else { c.bus->write32(addr & ~3u, c.r[rd]); c.cycles += 1; }
  }
}
static void tLdStH(Cpu& c, uint32_t op) {
  uint32_t l = (op >> 11) & 1, off = ((op >> 6) & 31) << 1, rb = (op >> 3) & 7, rd = op & 7;
  uint32_t addr = (uint32_t)c.r[rb] + off;
  if (l) { c.r[rd] = c.ldr16(addr); c.cycles += 2; } else { c.bus->write16(addr & ~1u, c.r[rd] & 0xFFFF); c.cycles += 1; }
}
static void tLdStSP(Cpu& c, uint32_t op) {
  uint32_t l = (op >> 11) & 1, rd = (op >> 8) & 7;
  uint32_t addr = (uint32_t)c.r[13] + ((op & 0xFF) << 2);
  if (l) { c.r[rd] = c.ldr32(addr); c.cycles += 2; } else { c.bus->write32(addr & ~3u, c.r[rd]); c.cycles += 1; }
}
static void tLoadAddr(Cpu& c, uint32_t op) {
  uint32_t base = (op & 0x800) ? (uint32_t)c.r[13] : ((uint32_t)c.r[15] & ~3u);
  c.r[(op >> 8) & 7] = (int32_t)(base + ((op & 0xFF) << 2));
}
static void tAddSP(Cpu& c, uint32_t op) {
  uint32_t off = (op & 0x7F) << 2;
  c.r[13] = (op & 0x80) ? (int32_t)((uint32_t)c.r[13] - off) : (int32_t)((uint32_t)c.r[13] + off);
}
static void tPushPop(Cpu& c, uint32_t op) {
  uint32_t l = (op >> 11) & 1, rbit = (op >> 8) & 1, list = op & 0xFF;
  if (l) {
    uint32_t addr = c.r[13];
    for (int i = 0; i < 8; i++) if (list & (1u << i)) { c.r[i] = (int32_t)c.bus->read32(addr & ~3u); addr += 4; c.cycles++; }
    if (rbit) { uint32_t v = c.bus->read32(addr & ~3u); addr += 4; c.r[13] = addr; c.branchTo(v); c.cycles++; }
    else c.r[13] = addr;
    c.cycles += 1;
  } else {
    int count = rbit + popcount16(list);
    uint32_t addr = (uint32_t)c.r[13] - count * 4;
    c.r[13] = addr;
    for (int i = 0; i < 8; i++) if (list & (1u << i)) { c.bus->write32(addr & ~3u, c.r[i]); addr += 4; c.cycles++; }
    if (rbit) { c.bus->write32(addr & ~3u, c.r[14]); c.cycles++; }
  }
}
static void tLdStM(Cpu& c, uint32_t op) {
  uint32_t l = (op >> 11) & 1, rb = (op >> 8) & 7, list = op & 0xFF;
  uint32_t addr = c.r[rb];
  if (!list) {
    if (l) c.branchTo(c.bus->read32(addr & ~3u)); else c.bus->write32(addr & ~3u, c.r[15] + 2);
    c.r[rb] = addr + 0x40;
    return;
  }
  uint32_t newBase = addr + popcount16(list) * 4;
  if (l) {
    for (int i = 0; i < 8; i++) if (list & (1u << i)) { c.r[i] = (int32_t)c.bus->read32(addr & ~3u); addr += 4; c.cycles++; }
    if (!(list & (1u << rb))) c.r[rb] = newBase;
    c.cycles += 1;
  } else {
    bool first = true;
    for (int i = 0; i < 8; i++) if (list & (1u << i)) {
      uint32_t v = ((uint32_t)i == rb && !first) ? newBase : (uint32_t)c.r[i];
      c.bus->write32(addr & ~3u, v); addr += 4; c.cycles++; first = false;
    }
    c.r[rb] = newBase;
  }
}
static void tBcond(Cpu& c, uint32_t op) {
  uint32_t cond = (op >> 8) & 15;
  if (cond == 0xF) { c.exception(0x08, MODE_SVC, c.pc, false); return; }
  if (cond == 0xE) { thumbUndefined(c, op); return; }
  if (!c.condPassed(cond)) return;
  int32_t off = ((int32_t)((op & 0xFF) << 24)) >> 23;
  if (off < 0) c.checkIdle(c.pc - 2);
  c.branchTo(c.r[15] + off);
}
static void tB(Cpu& c, uint32_t op) {
  int32_t off = ((int32_t)((op & 0x7FF) << 21)) >> 20;
  if (off < 0) c.checkIdle(c.pc - 2);
  c.branchTo(c.r[15] + off);
}
static void tBL(Cpu& c, uint32_t op) {
  if (op & 0x0800) { uint32_t target = (uint32_t)c.r[14] + ((op & 0x7FF) << 1); c.r[14] = c.pc | 1; c.branchTo(target); }
  else c.r[14] = c.r[15] + (((int32_t)((op & 0x7FF) << 21)) >> 9);
}

static void buildTables() {
#if defined(PS_SMALL_TABLES)
  if (!ARM) ARM = new uint8_t[4096];
  if (!THUMB) THUMB = new uint8_t[1024];
  handlerCount = 0;
  handlerIndex(armUndefined);
#elif defined(PS_HEAP_TABLES)
  if (!ARM) ARM = new ArmFn[4096];
  if (!THUMB) THUMB = new ArmFn[1024];
#endif
  for (uint32_t key = 0; key < 4096; key++) {
    uint32_t hi = key >> 4, lo = key & 15, top3 = hi >> 5;
    ArmFn h = armUndefined;
    switch (top3) {
      case 0:
        if (lo == 9) {
          if ((hi & 0xFC) == 0x00) h = armMUL;
          else if ((hi & 0xF8) == 0x08) h = armMULL;
          else if ((hi & 0xFB) == 0x10) h = armSWP;
        } else if ((lo & 9) == 9) h = armHalf;
        else if ((hi & 0x19) == 0x10) {
          if (hi == 0x12 && lo == 1) h = armBX;
          else if ((hi & 0xFB) == 0x10 && lo == 0) h = armMRS;
          else if ((hi & 0xFB) == 0x12 && lo == 0) h = armMSR;
        } else h = (lo & 1) ? armDPRegRegShift : armDPRegImmShift;
        break;
      case 1: h = ((hi & 0x19) == 0x10) ? (((hi & 0xFB) == 0x32) ? armMSR : armUndefined) : armDPImm; break;
      case 2: h = armSDT; break;
      case 3: h = (lo & 1) ? armUndefined : armSDT; break;
      case 4: h = armBlock; break;
      case 5: h = armB; break;
      case 6: h = armUndefined; break;
      default: h = (hi & 0x10) ? armSWI : armUndefined; break;
    }
    ARM[key] = TABLE_FN(h);
  }
  for (uint32_t k = 0; k < 1024; k++) {
    uint32_t op = k << 6;
    ArmFn h = thumbUndefined;
    if ((op & 0xF800) == 0x1800) h = tAddSub;
    else if ((op & 0xE000) == 0x0000) h = tShiftImm;
    else if ((op & 0xE000) == 0x2000) h = tImm;
    else if ((op & 0xFC00) == 0x4000) h = tALU;
    else if ((op & 0xFC00) == 0x4400) h = tHiReg;
    else if ((op & 0xF800) == 0x4800) h = tLdrPC;
    else if ((op & 0xF000) == 0x5000) h = tLdStReg;
    else if ((op & 0xE000) == 0x6000) h = tLdStImm;
    else if ((op & 0xF000) == 0x8000) h = tLdStH;
    else if ((op & 0xF000) == 0x9000) h = tLdStSP;
    else if ((op & 0xF000) == 0xA000) h = tLoadAddr;
    else if ((op & 0xFF00) == 0xB000) h = tAddSP;
    else if ((op & 0xF600) == 0xB400) h = tPushPop;
    else if ((op & 0xF000) == 0xC000) h = tLdStM;
    else if ((op & 0xF000) == 0xD000) h = tBcond;
    else if ((op & 0xF800) == 0xE000) h = tB;
    else if ((op & 0xF000) == 0xF000) h = tBL;
    THUMB[k] = TABLE_FN(h);
  }
  tablesBuilt = true;
}

// =====================================================================================
// System
// =====================================================================================
#if defined(PS_HEAP_TABLES) || defined(PS_SMALL_TABLES)
void freeTables() {
  delete[] ARM; delete[] THUMB;
  ARM = THUMB = nullptr;
  tablesBuilt = false;
}
#endif

#ifdef PS_FLASH_BLOCKS
#define FL(off) (blk[(uint32_t)(off) >> 13] + ((uint32_t)(off) & 0x1FFF))
#else
#define FL(off) (flash + (uint32_t)(off))
#endif

PocketStation::PocketStation() : sampleRate(32000), audioRing(nullptr), audioRingSize(0), audioW(0), audioR(0),
                                 onFlashWrite(nullptr), cbCtx(nullptr) {
  if (!tablesBuilt) buildTables();
  cpu.bus = this;
#ifdef PS_FLASH_BLOCKS
  bios = nullptr; writableBlock = nullptr;
  for (auto& b : blk) b = nullptr;
#else
  flash = nullptr;
  memset(bios, 0, sizeof bios);
#endif
  for (int i = 0; i < 128; i++) extra[i] = 0xFFFF;
  const uint16_t ex[8] = {0x6BE7, 0x426C, 0x05CA, 0xFFFF, 0x001A, 0xFFFF, 0x0010, 0xFFFF};
  memcpy(extra, ex, sizeof ex);
}

#ifdef PS_FLASH_BLOCKS
void PocketStation::init(const uint8_t* biosData, const uint8_t* const blocks[16]) {
  bios = biosData;
  for (int i = 0; i < 16; i++) blk[i] = blocks[i];
}
#else
void PocketStation::init(const uint8_t* biosData, uint8_t* flash128k) {
  memcpy(bios, biosData, 0x4000);
  flash = flash128k;
}
#endif

void PocketStation::hardReset(const DateTime& d) {
  cpu.reset();
  memset(ram, 0, sizeof ram); memset(vram, 0, sizeof vram);
  fCtrl = 0; remapped = false; fBankFlg = 0; memset(fBankVal, 0, sizeof fBankVal);
  fWait1 = fWait2 = flashUnlock = 0; pageBase = -1;
  memset(virtMap, -1, sizeof virtMap);
  intInput = intHold = intMask = 0; buttons = docked = 0;
  memset(timers, 0, sizeof timers);
  clkMode = 0; sleeping = false;
  lcdMode = lcdCal = iopCtrl = 0; iopData = 0xFFFF; dacCtrl = dacData = battCtrl = irdaMode = comMode = comCtrl1 = comCtrl2 = 0;
  masterTime = 0; audioPhase = 0; flashDirty = false; writeCount = 0; ramHash = 0;
  rtcInit(d);
  updateIrq();
}

// ---------------- RTC ----------------
void PocketStation::rtcInit(const DateTime& d) {
  rtc.sec = d.second; rtc.min = d.minute; rtc.hour = d.hour; rtc.dow = d.dow + 1;
  rtc.day = d.day; rtc.month = d.month; rtc.year = d.year % 100;
  rtc.mode = 0; rtc.phase = 0; rtc.adjusted = false;
}
static int daysIn(int mo, int y) { static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return (mo == 2 && y % 4 == 0) ? 29 : d[(mo - 1) % 12]; }
void PocketStation::rtcIncrement(int f) {
  switch (f) {
    case 0: rtc.sec = (rtc.sec + 1) % 60; break;
    case 1: rtc.min = (rtc.min + 1) % 60; break;
    case 2: rtc.hour = (rtc.hour + 1) % 24; break;
    case 3: rtc.dow = rtc.dow % 7 + 1; break;
    case 4: rtc.day = rtc.day % daysIn(rtc.month, rtc.year) + 1; break;
    case 5: rtc.month = rtc.month % 12 + 1; break;
    case 6: rtc.year = (rtc.year + 1) % 100; break;
  }
}
void PocketStation::rtcTickSecond() {
  if (++rtc.sec < 60) return;
  rtc.sec = 0; if (++rtc.min < 60) return;
  rtc.min = 0; if (++rtc.hour < 24) return;
  rtc.hour = 0; rtc.dow = rtc.dow % 7 + 1;
  if (++rtc.day <= daysIn(rtc.month, rtc.year)) return;
  rtc.day = 1; if (++rtc.month <= 12) return;
  rtc.month = 1; rtc.year = (rtc.year + 1) % 100;
}
uint32_t PocketStation::rtcTime() const { return toBCD(rtc.sec) | (toBCD(rtc.min) << 8) | (toBCD(rtc.hour) << 16) | ((uint32_t)rtc.dow << 24); }
uint32_t PocketStation::rtcDate() const { return toBCD(rtc.day) | (toBCD(rtc.month) << 8) | (toBCD(rtc.year) << 16); }

// ---------------- interrupts ----------------
void PocketStation::setInput(uint32_t bits, bool level) {
  uint32_t old = intInput, nv = level ? (old | bits) : (old & ~bits);
  intInput = nv;
  intHold |= (nv & ~old) & ~LEVEL_MASK;
  updateIrq();
}
void PocketStation::pulse(uint32_t bits) { intHold |= bits; updateIrq(); }
void PocketStation::updateIrq() {
  uint32_t pending = ((intHold & ~LEVEL_MASK) | (intInput & LEVEL_MASK)) & intMask;
  cpu.irqLine = (pending & ~FIQ_MASK) != 0;
  cpu.fiqLine = (pending & FIQ_MASK) != 0;
  if (pending && sleeping) sleeping = false;
}
uint32_t PocketStation::intLatch() const { return ((intHold & ~LEVEL_MASK) | (intInput & LEVEL_MASK)) & intMask; }
void PocketStation::setButtons(uint32_t mask) {
  mask &= 0x1F;
  if (mask == buttons) return;
  buttons = mask;
  intInput = (intInput & ~0x1Fu) | mask;
  updateIrq();
}

// ---------------- FLASH ----------------
void PocketStation::rebuildVirtMap() {
  memset(virtMap, -1, sizeof virtMap);
  for (int i = 15; i >= 0; i--) if (fBankFlg & (1u << i)) virtMap[fBankVal[i] & 15] = (int8_t)i;
}
inline int32_t PocketStation::flash1Offset(uint32_t a) const {
  uint32_t off = a & 0x1FFFF;
  if (!(fCtrl & 2)) return (int32_t)off;
  int phys = virtMap[off >> 13];
  if (phys < 0) return -1;
  return (phys << 13) | (off & 0x1FFF);
}
void PocketStation::flashWrite(uint32_t off, uint32_t val, int width) {
  uint32_t k = off & 0x1FFFF;
  if (width == 2 && flashUnlock < 3) {
    uint32_t b = val & 0xFF;
    if (flashUnlock == 0 && k == 0x55AA && b == 0xAA) { flashUnlock = 1; return; }
    if (flashUnlock == 1 && k == 0x2A54 && b == 0x55) { flashUnlock = 2; return; }
    if (flashUnlock == 2 && k == 0x55AA && b == 0xA0) { flashUnlock = 3; pageBase = -1; return; }
  }
  if (!(fWait2 & 1) && flashUnlock < 3) return;
#ifdef PS_FLASH_BLOCKS
  uint8_t* w = writableBlock ? writableBlock(cbCtx, (int)(k >> 13)) : nullptr;
  if (!w) return;
  blk[k >> 13] = w;
  w += k & 0x1FFF;
#else
  uint8_t* w = flash + k;
#endif
  if (width == 4) wr32(w - (k & 3u), val);
  else if (width == 2) wr16(w - (k & 1u), (uint16_t)val);
  else *w = (uint8_t)val;
  int32_t page = k & ~0x7Fu;
  if (page != pageBase) { pageBase = page; if (onFlashWrite) onFlashWrite(cbCtx, page); }
  flashDirty = true;
}

// ---------------- bus ----------------
HOT uint32_t PocketStation::read32(uint32_t a) {
  switch (a >> 24) {
    case 0x00: case 0x01:
      if (!remapped && a < 0x4000) return rd32(bios + (a & 0x3FFC));
      return rd32(ram + (a & 0x7FC));
    case 0x02: { int32_t o = flash1Offset(a); return o < 0 ? 0 : rd32(FL(o & ~3)); }
    case 0x04: return rd32(bios + (a & 0x3FFC));
    case 0x08: return rd32(FL(a & 0x1FFFC));
    default: return ioRead(a & ~3u);
  }
}
HOT uint32_t PocketStation::read16(uint32_t a) {
  switch (a >> 24) {
    case 0x00: case 0x01:
      if (!remapped && a < 0x4000) return rd16(bios + (a & 0x3FFE));
      return rd16(ram + (a & 0x7FE));
    case 0x02: { int32_t o = flash1Offset(a); return o < 0 ? 0 : rd16(FL(o & ~1)); }
    case 0x04: return rd16(bios + (a & 0x3FFE));
    case 0x08: return rd16(FL(a & 0x1FFFE));
    case 0x06: if ((a & 0xFFFF00) == 0x000300) return extra[(a >> 1) & 0x7F];
    // fallthrough
    default: return (ioRead(a & ~3u) >> ((a & 2) << 3)) & 0xFFFF;
  }
}
HOT uint32_t PocketStation::read8(uint32_t a) {
  switch (a >> 24) {
    case 0x00: case 0x01:
      if (!remapped && a < 0x4000) return bios[a & 0x3FFF];
      return ram[a & 0x7FF];
    case 0x02: { int32_t o = flash1Offset(a); return o < 0 ? 0 : *FL(o); }
    case 0x04: return bios[a & 0x3FFF];
    case 0x08: return *FL(a & 0x1FFFF);
    case 0x06: if ((a & 0xFFFF00) == 0x000300) return (extra[(a >> 1) & 0x7F] >> ((a & 1) << 3)) & 0xFF;
    // fallthrough
    default: return (ioRead(a & ~3u) >> ((a & 3) << 3)) & 0xFF;
  }
}
HOT void PocketStation::write32(uint32_t a, uint32_t v) {
  switch (a >> 24) {
    case 0x00: case 0x01: { uint32_t w = a & 0x7FC; uint32_t o = rd32(ram + w); if (o != v) { wr32(ram + w, v); ramHash ^= mixWord(w, o) ^ mixWord(w, v); } return; }
    // (any non-RAM write may have side effects: always counts as a state change)
    case 0x02: { writeCount++; int32_t o = flash1Offset(a); if (o >= 0) flashWrite(o, v, 4); return; }
    case 0x08: writeCount++; flashWrite(a & 0x1FFFF, v, 4); return;
    case 0x04: return;
    default: writeCount++; ioWrite(a & ~3u, v, 0xFFFFFFFF);
  }
}
HOT void PocketStation::write16(uint32_t a, uint32_t v) {
  switch (a >> 24) {
    case 0x00: case 0x01: { uint32_t w = a & 0x7FC; uint32_t o = rd32(ram + w); wr16(ram + (a & 0x7FE), (uint16_t)v); uint32_t nw = rd32(ram + w); if (o != nw) ramHash ^= mixWord(w, o) ^ mixWord(w, nw); return; }
    case 0x02: { writeCount++; int32_t o = flash1Offset(a); if (o >= 0) flashWrite(o, v, 2); return; }
    case 0x08: writeCount++; flashWrite(a & 0x1FFFF, v, 2); return;
    case 0x04: return;
    default: { writeCount++; uint32_t sh = (a & 2) << 3; ioWrite(a & ~3u, (v & 0xFFFF) << sh, 0xFFFFu << sh); }
  }
}
HOT void PocketStation::write8(uint32_t a, uint32_t v) {
  switch (a >> 24) {
    case 0x00: case 0x01: { uint32_t w = a & 0x7FC; uint32_t o = rd32(ram + w); ram[a & 0x7FF] = (uint8_t)v; uint32_t nw = rd32(ram + w); if (o != nw) ramHash ^= mixWord(w, o) ^ mixWord(w, nw); return; }
    case 0x02: { writeCount++; int32_t o = flash1Offset(a); if (o >= 0) flashWrite(o, v, 1); return; }
    case 0x08: writeCount++; flashWrite(a & 0x1FFFF, v, 1); return;
    case 0x04: return;
    default: writeCount++; ioWrite(a & ~3u, (v & 0xFF) << ((a & 3) << 3), 0xFFFFFFFF);
  }
}

uint32_t PocketStation::ioRead(uint32_t a) {
  switch (a) {
    case 0x06000000: return fCtrl | (remapped ? 1 : 0);
    case 0x06000004: return 0;
    case 0x06000008: return fBankFlg;
    case 0x0600000C: return fWait1;
    case 0x06000010: return fWait2 | 4;
    case 0x0A000000: return intLatch();
    case 0x0A000004: return intInput;
    case 0x0A000008: case 0x0A00000C: case 0x0A000010: return intMask;
    case 0x0A800000: return timers[0].reload; case 0x0A800008: return timers[0].mode;
    case 0x0A800010: return timers[1].reload; case 0x0A800018: return timers[1].mode;
    case 0x0A800020: return timers[2].reload; case 0x0A800028: return timers[2].mode;
    // Counters change with time: a loop polling one is never "idle" (writeCount doubles as a
    // state-change counter for idle-loop detection).
    case 0x0A800004: writeCount++; return timers[0].count;
    case 0x0A800014: writeCount++; return timers[1].count;
    case 0x0A800024: writeCount++; return timers[2].count;
    case 0x0B000000: return (clkMode & 15) | 0x10;
    case 0x0B000004: return 0;
    case 0x0B800000: return rtc.mode;
    case 0x0B800008: return rtcTime();
    case 0x0B80000C: return rtcDate();
    case 0x0C000000: return comMode;
    case 0x0C000010: return comCtrl1;
    case 0x0C000018: return comCtrl2;
    case 0x0C800000: return irdaMode;
    case 0x0D000000: return lcdMode;
    case 0x0D000004: return lcdCal;
    case 0x0D800000: return iopCtrl;
    case 0x0D80000C: return (iopData & ~0x10u) | (docked ? 0x10 : 0);
    case 0x0D800010: return dacCtrl;
    case 0x0D800014: return dacData;
    case 0x0D800020: return battCtrl;
  }
  if (a >= 0x06000100 && a < 0x06000140) return fBankVal[(a >> 2) & 15];
  if (a >= 0x06000300 && a < 0x06000400) { uint32_t i = (a >> 1) & 0x7F; return extra[i] | ((uint32_t)extra[(i + 1) & 0x7F] << 16); }
  if ((a >> 24) == 0x0D && !(a & 0x800000) && (a & 0xFFFF) >= 0x100) return vram[(a >> 2) & 31];
  return 0;
}

void PocketStation::ioWrite(uint32_t a, uint32_t v, uint32_t mask) {
  switch (a) {
    case 0x06000000: if (v & 1) remapped = true; fCtrl = (v & 3) | (remapped ? 1 : 0); return;
    case 0x06000008: fBankFlg = v & 0xFFFF; rebuildVirtMap(); return;
    case 0x0600000C: fWait1 = v; return;
    case 0x06000010: fWait2 = v & ~4u; if (!(v & 1)) { flashUnlock = 0; pageBase = -1; } return;
    case 0x0A000008: intMask = (intMask | v) & 0xFFFF; updateIrq(); return;
    case 0x0A00000C: intMask = (intMask & ~v) & 0xFFFF; updateIrq(); return;
    case 0x0A000010: intHold &= ~v; updateIrq(); return;
    case 0x0A800000: timers[0].reload = v & 0xFFFF; return; case 0x0A800004: timers[0].count = v & 0xFFFF; return; case 0x0A800008: timers[0].mode = v & 0xFFFF; return;
    case 0x0A800010: timers[1].reload = v & 0xFFFF; return; case 0x0A800014: timers[1].count = v & 0xFFFF; return; case 0x0A800018: timers[1].mode = v & 0xFFFF; return;
    case 0x0A800020: timers[2].reload = v & 0xFFFF; return; case 0x0A800024: timers[2].count = v & 0xFFFF; return; case 0x0A800028: timers[2].mode = v & 0xFFFF; return;
    case 0x0B000000: clkMode = v & 15; return;
    case 0x0B000004: if ((v & 1) && !(intLatch() & intMask)) { sleeping = true; cpu.stopRequested = true; } return;
    case 0x0B800000: rtc.mode = v & 15; return;
    case 0x0B800004: { int f = (rtc.mode >> 1) & 7; if (f < 7 && !rtc.adjusted) { rtcIncrement(f); rtc.adjusted = true; } return; }
    case 0x0C000000: comMode = v; return;
    case 0x0C000010: comCtrl1 = v; return;
    case 0x0C000018: comCtrl2 = v; return;
    case 0x0C800000: irdaMode = v; return;
    case 0x0D000000: lcdMode = v & 0xFF; return;
    case 0x0D000004: lcdCal = v; return;
    case 0x0D800000: iopCtrl = v; return;
    case 0x0D800004: iopData |= v; return;
    case 0x0D800008: iopData &= ~v; return;
    case 0x0D800010: dacCtrl = v; return;
    case 0x0D800014: dacData = v; return;
    case 0x0D800020: battCtrl = v; return;
  }
  if (a >= 0x06000100 && a < 0x06000140) { fBankVal[(a >> 2) & 15] = v & 15; rebuildVirtMap(); return; }
  if (a >= 0x0D000100 && a < 0x0D000180) { uint32_t i = (a >> 2) & 31; vram[i] = (vram[i] & ~mask) | (v & mask); }
}

// ---------------- time keeping ----------------
void PocketStation::tick(uint32_t cycles) {
  for (int i = 0; i < 3; i++) {
    Timer& t = timers[i];
    if (!(t.mode & 4)) continue;
    uint32_t div = TIMER_DIV[t.mode & 3];
    t.acc += cycles;
    if (t.acc < div) continue;
    uint32_t steps = t.acc / div;
    t.acc -= steps * div;
    while (steps > 0) {
      if (steps <= t.count) { t.count -= steps; steps = 0; }
      else {
        steps -= t.count + 1;
        t.count = t.reload;
        pulse(i == 0 ? INT_T0 : i == 1 ? INT_T1 : INT_T2);
        if (t.reload == 0) steps = 0;
      }
    }
  }
  advanceMaster(cycles << clockShift());
}

void PocketStation::advanceMaster(uint32_t m) {
  masterTime += m;
  bool paused = rtc.mode & 1;
  uint32_t period = paused ? RTC_PAUSED_PERIOD : MAX_CLOCK;
  rtc.phase += m;
  while (rtc.phase >= period) {
    rtc.phase -= period;
    if (!paused) rtcTickSecond();
    rtc.adjusted = false;
    setInput(INT_RTC, true);
  }
  if (rtc.phase >= period / 2 && (intInput & INT_RTC)) setInput(INT_RTC, false);
  if (audioRing) {
    bool on = (dacCtrl & 1) && !(iopData & 0x20);
    int16_t level = on ? (int16_t)((((int32_t)(dacData << 16)) >> 22) * 51) : 0;
    audioPhase += (uint64_t)m * sampleRate;
    while (audioPhase >= MAX_CLOCK) {
      audioPhase -= MAX_CLOCK;
      uint32_t next = (audioW + 1) % audioRingSize;
      if (next != audioR) { audioRing[audioW] = level; audioW = next; }
    }
  }
}

uint32_t PocketStation::cyclesToNextEvent() const {
  uint32_t best = 1000000000u;
  for (int i = 0; i < 3; i++) {
    const Timer& t = timers[i];
    if (!(t.mode & 4)) continue;
    uint32_t div = TIMER_DIV[t.mode & 3];
    uint32_t c = (t.count + 1) * div - t.acc;
    if (c < best) best = c;
  }
  uint32_t period = (rtc.mode & 1) ? RTC_PAUSED_PERIOD : MAX_CLOCK;
  uint32_t toEdge = rtc.phase < period / 2 ? period / 2 - rtc.phase : period - rtc.phase;
  uint32_t sh = clockShift();
  uint32_t c = ((toEdge + (1u << sh) - 1) >> sh) + 1;
  if (c < best) best = c;
  return best ? best : 1;
}

void PocketStation::runMaster(uint64_t m) {
  uint64_t end = masterTime + m;
  uint32_t guard = 0;
  while (masterTime < end && guard++ < 5000000) {
    uint32_t sh = clockShift();
    uint64_t remain64 = ((end - masterTime) + (1u << sh) - 1) >> sh;
    uint32_t remain = remain64 > 1000000000u ? 1000000000u : (uint32_t)remain64;
    if (!remain) remain = 1;
    uint32_t next = cyclesToNextEvent();
    if (sleeping) { tick(remain < next ? remain : next); continue; }
    uint32_t slice = remain < 256 ? remain : 256;
    if (next < slice) slice = next;
    tick(cpu.run(slice));
    if (cpu.idle) {
      // spinning until something happens: jump to the next hardware event
      cpu.idle = false;
      uint32_t r2 = (uint32_t)(((end - masterTime) + (1u << clockShift()) - 1) >> clockShift());
      uint32_t n2 = cyclesToNextEvent();
      uint32_t skip = r2 < n2 ? r2 : n2;
      if (skip > 1) tick(skip - 1);
      for (auto& sl : cpu.idleSlots) sl.pc = 0xFFFFFFFF;
    }
  }
}
void PocketStation::runMs(uint32_t ms) { runMaster((uint64_t)ms * MAX_CLOCK / 1000); }

// ---------------- boot ----------------
bool PocketStation::bootFile(int dirIndex, const DateTime& d) {
  hardReset(d);
  const uint32_t GUI_ENTRY = 0x04001E00;
  uint32_t n = 0;
  while (cpu.pc != GUI_ENTRY && n++ < 2000000) { uint32_t before = cpu.cycles; cpu.step(); tick(cpu.cycles - before); }
  if (cpu.pc != GUI_ENTRY) return false;
  if (!dirIndex) return bootMenu(d);
  syncClock(d);
  const uint32_t stub[7] = {0xE3A00001, 0xE3A01000u | (dirIndex & 0xFF), 0xE3A02000, 0xEF000008, 0xE3A00000, 0xEF000009, 0xEAFFFFFE};
  for (int i = 0; i < 7; i++) wr32(ram + 0x7C0 + i * 4, stub[i]);
  cpu.t = 0; cpu.pc = 0x7C0;
  return true;
}

bool PocketStation::bootMenu(const DateTime& d) {
  auto atMainMenu = [this]() {
    uint32_t row = 0;
    for (int i = 0; i < 10; i++) { runMs(100); row |= displayOn() ? vram[14] : 0; }
    return (lcdMode & 0x80) ? (row >> 26) == 0x3F : (row & 0x3F) == 0x3F;
  };
  auto tap = [this](uint32_t b) { setButtons(b); runMs(100); setButtons(0); runMs(250); };
  runMs(9000);
  for (int i = 0; i < 6 && !atMainMenu(); i++) {
    if (i % 2 == 0) { tap(BTN_UP); tap(BTN_DOWN); }
    tap(BTN_FIRE);
  }
  syncClock(d);
  return true;
}

void PocketStation::syncClock(const DateTime& d) {
  uint32_t m = rtc.mode;
  rtcInit(d);
  rtc.mode = m;
  ram[0xCF] = toBCD((d.year / 100) % 100);
  ram[0xCD] = toBCD(d.year % 100);
}

void PocketStation::getFrame(uint32_t out[32]) const {
  if (!displayOn()) { memset(out, 0, 32 * 4); return; }
  if (!(lcdMode & 0x80)) { memcpy(out, vram, 32 * 4); return; }
  for (int y = 0; y < 32; y++) {   // rotate 180: reverse rows and bits
    uint32_t w = vram[31 - y], rv = 0;
    for (int x = 0; x < 32; x++) rv |= ((w >> x) & 1u) << (31 - x);
    out[y] = rv;
  }
}

// ---------------- save states ----------------
struct StateBlob {
  uint32_t magic, version;
  uint8_t ram[0x800];
  uint32_t vram[32];
  int32_t r[16], b13[6], b14[6], spsr[6], u8[5], f8[5];
  uint32_t cpsr, pc;
  uint32_t fCtrl, remapped, fBankFlg, fBankVal[16], fWait1, fWait2;
  uint32_t intInput, intHold, intMask;
  Timer timers[3];
  uint32_t clkMode, sleeping, lcdMode, lcdCal, iopCtrl, iopData, dacCtrl, dacData, battCtrl, irdaMode, rtcMode, rtcPhase;
};
size_t PocketStation::stateSize() const { return sizeof(StateBlob); }
void PocketStation::saveState(uint8_t* out) const {
  StateBlob* s = (StateBlob*)out;
  memset(s, 0, sizeof *s);
  s->magic = 0x53535350; s->version = STATE_VERSION;
  memcpy(s->ram, ram, sizeof ram); memcpy(s->vram, vram, sizeof vram);
  memcpy(s->r, cpu.r, sizeof s->r); memcpy(s->b13, cpu.bankR13, sizeof s->b13); memcpy(s->b14, cpu.bankR14, sizeof s->b14);
  memcpy(s->spsr, cpu.spsrBank, sizeof s->spsr); memcpy(s->u8, cpu.usrR8, sizeof s->u8); memcpy(s->f8, cpu.fiqR8, sizeof s->f8);
  s->cpsr = cpu.getCPSR(); s->pc = cpu.pc;
  s->fCtrl = fCtrl; s->remapped = remapped; s->fBankFlg = fBankFlg; memcpy(s->fBankVal, fBankVal, sizeof fBankVal);
  s->fWait1 = fWait1; s->fWait2 = fWait2; s->intInput = intInput & ~0x1Fu; s->intHold = intHold; s->intMask = intMask;
  memcpy(s->timers, timers, sizeof timers);
  s->clkMode = clkMode; s->sleeping = sleeping; s->lcdMode = lcdMode; s->lcdCal = lcdCal; s->iopCtrl = iopCtrl; s->iopData = iopData;
  s->dacCtrl = dacCtrl; s->dacData = dacData; s->battCtrl = battCtrl; s->irdaMode = irdaMode; s->rtcMode = rtc.mode; s->rtcPhase = rtc.phase;
}
bool PocketStation::loadState(const uint8_t* in, size_t len, const DateTime& now) {
  if (len != sizeof(StateBlob)) return false;
  const StateBlob* s = (const StateBlob*)in;
  if (s->magic != 0x53535350 || s->version != STATE_VERSION) return false;
  memcpy(ram, s->ram, sizeof ram); memcpy(vram, s->vram, sizeof vram);
  memcpy(cpu.r, s->r, sizeof s->r); memcpy(cpu.bankR13, s->b13, sizeof s->b13); memcpy(cpu.bankR14, s->b14, sizeof s->b14);
  memcpy(cpu.spsrBank, s->spsr, sizeof s->spsr); memcpy(cpu.usrR8, s->u8, sizeof s->u8); memcpy(cpu.fiqR8, s->f8, sizeof s->f8);
  uint32_t c = s->cpsr;
  cpu.n = (c >> 31) & 1; cpu.z = (c >> 30) & 1; cpu.c = (c >> 29) & 1; cpu.v = (c >> 28) & 1;
  cpu.iflag = (c >> 7) & 1; cpu.fflag = (c >> 6) & 1; cpu.t = (c >> 5) & 1; cpu.mode = c & 0x1F;
  cpu.pc = s->pc; cpu.stopRequested = false;
  fCtrl = s->fCtrl; remapped = s->remapped; fBankFlg = s->fBankFlg; memcpy(fBankVal, s->fBankVal, sizeof fBankVal); rebuildVirtMap();
  fWait1 = s->fWait1; fWait2 = s->fWait2; flashUnlock = 0; pageBase = -1;
  intInput = s->intInput; intHold = s->intHold; intMask = s->intMask;
  memcpy(timers, s->timers, sizeof timers);
  clkMode = s->clkMode; sleeping = s->sleeping; lcdMode = s->lcdMode; lcdCal = s->lcdCal; iopCtrl = s->iopCtrl; iopData = s->iopData;
  dacCtrl = s->dacCtrl; dacData = s->dacData; battCtrl = s->battCtrl; irdaMode = s->irdaMode;
  rtcInit(now); rtc.mode = s->rtcMode; rtc.phase = s->rtcPhase;
  buttons = 0;
  updateIrq();
  return true;
}

}  // namespace ps
