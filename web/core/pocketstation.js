// Web PocketStation - Copyright (C) 2026 the Web PocketStation authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// PocketStation (SCPH-4000) system emulation: memory map, FLASH banking/programming,
// interrupt controller, timers, clock control/sleep, RTC, LCD, buttons, LED, speaker DAC.
// Hardware reference: psx-spx "Pocketstation" chapters + behaviour observed in the retail BIOS.

import { ARM7 } from './arm7.js';

// Bump when a fix means older save states may be stuck/invalid (they're then discarded).
export const STATE_VERSION = 2;

export const BTN = { FIRE: 1, RIGHT: 2, LEFT: 4, DOWN: 8, UP: 16 };

const INT_COM = 1 << 6, INT_T0 = 1 << 7, INT_T1 = 1 << 8, INT_RTC = 1 << 9,
  INT_T2 = 1 << 13;
const FIQ_MASK = INT_COM | INT_T2;
const LEVEL_MASK = 0x1F | (1 << 10) | (1 << 11); // buttons, battery, dock: level-sensitive
const MAX_CLOCK = 7995392; // CLK_MODE >= 8

function clockFor(mode) {
  const f = mode & 15;
  return f >= 8 ? MAX_CLOCK : MAX_CLOCK / (1 << (8 - f));
}
const toBCD = (n) => ((Math.floor(n / 10) << 4) | (n % 10)) & 0xFF;
const fromBCD = (b) => (b >> 4) * 10 + (b & 15);
const TIMER_DIV = [2, 32, 512, 2];

export class PocketStation {
  constructor(opts = {}) {
    this.bios = new Uint8Array(0x4000);
    if (opts.bios) this.bios.set(opts.bios.subarray(0, 0x4000));
    this.flash = new Uint8Array(0x20000).fill(0xFF);
    if (opts.flash) this.flash.set(opts.flash.subarray(0, 0x20000));
    this.ram = new Uint8Array(0x800);
    this.vram = new Uint32Array(32);
    this.biosDV = new DataView(this.bios.buffer);
    this.flashDV = new DataView(this.flash.buffer);
    this.ramDV = new DataView(this.ram.buffer);
    // "Extra FLASH" (serial number, LCD calibration) - values from a real unit
    this.extra = new Uint16Array(128).fill(0xFFFF);
    this.extra.set([0x6BE7, 0x426C, 0x05CA, 0xFFFF, 0x001A, 0xFFFF, 0x0010, 0xFFFF]);
    this.sampleRate = opts.sampleRate || 32000;
    this.audioOut = null; // optional callback(Float32Array)
    this.onFlashWrite = null; // callback(physicalOffset) when a FLASH page is programmed
    this.onTTY = null;
    this.cpu = new ARM7(this);
    this.cpu.onUndefined = (cpu, op) => {
      // E6000010h: debug TTY output of chr(r0) - trapped here (harmless if the kernel also does)
      if ((op >>> 0) === 0xE6000010) { if (this.onTTY) this.onTTY(cpu.r[0] & 0xFF); return true; }
      return false;
    };
    this.hardReset(opts.date);
  }

  // Full power-on reset (like pressing the Reset button on the back).
  hardReset(date) {
    this.cpu.reset();
    this.ram.fill(0);
    this.vram.fill(0);
    this.fCtrl = 0; this.remapped = false; this.fBankFlg = 0; this.fBankVal = new Uint32Array(16);
    this.fWait1 = 0; this.fWait2 = 0; this.flashUnlock = 0;
    this.virtMap = new Int8Array(16).fill(-1);
    this.intInput = 0; this.intHold = 0; this.intMask = 0;
    this.buttons = 0; this.docked = 0;
    this.timers = [0, 1, 2].map(() => ({ reload: 0, count: 0, mode: 0, acc: 0 }));
    this.clkMode = 0; this.sleeping = false;
    this.lcdMode = 0; this.lcdCal = 0;
    this.iopCtrl = 0; this.iopData = 0xFFFF; this.dacCtrl = 0; this.dacData = 0;
    this.battCtrl = 0; this.irdaMode = 0; this.comMode = 0; this.comCtrl1 = 0; this.comCtrl2 = 0;
    this.frameCount = 0;
    this.time = 0; // emulated seconds since reset
    this.audioPhase = 0; this.audioBuf = new Float32Array(1024); this.audioPos = 0;
    this.rtcInit(date || new Date());
    this.updateIrq();
  }

  get clock() { return clockFor(this.clkMode); }

  // ---------------- RTC ----------------
  rtcInit(d) {
    this.rtc = {
      sec: d.getSeconds(), min: d.getMinutes(), hour: d.getHours(), dow: d.getDay() + 1,
      day: d.getDate(), month: d.getMonth() + 1, year: d.getFullYear() % 100,
      mode: 0, phase: 0,
    };
  }
  rtcSetFromDate(d) {
    const m = this.rtc.mode;
    this.rtcInit(d);
    this.rtc.mode = m;
  }
  rtcIncrement(field) {
    const r = this.rtc;
    const dim = (mo, y) => [31, (y % 4 === 0) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][mo - 1];
    switch (field) {
      case 0: r.sec = (r.sec + 1) % 60; break;
      case 1: r.min = (r.min + 1) % 60; break;
      case 2: r.hour = (r.hour + 1) % 24; break;
      case 3: r.dow = r.dow % 7 + 1; break;
      case 4: r.day = r.day % dim(r.month, r.year) + 1; break;
      case 5: r.month = r.month % 12 + 1; break;
      case 6: r.year = (r.year + 1) % 100; break;
    }
  }
  rtcTickSecond() {
    const r = this.rtc;
    r.sec++;
    if (r.sec < 60) return;
    r.sec = 0; r.min++;
    if (r.min < 60) return;
    r.min = 0; r.hour++;
    if (r.hour < 24) return;
    r.hour = 0; r.dow = r.dow % 7 + 1;
    const dim = [31, (r.year % 4 === 0) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][r.month - 1];
    r.day++;
    if (r.day <= dim) return;
    r.day = 1; r.month++;
    if (r.month <= 12) return;
    r.month = 1; r.year = (r.year + 1) % 100;
  }
  rtcTime() {
    const r = this.rtc;
    return (toBCD(r.sec) | (toBCD(r.min) << 8) | (toBCD(r.hour) << 16) | (r.dow << 24)) >>> 0;
  }
  rtcDate() {
    const r = this.rtc;
    return (toBCD(r.day) | (toBCD(r.month) << 8) | (toBCD(r.year) << 16)) >>> 0;
  }

  // ---------------- interrupts ----------------
  setInput(bits, level) {
    const old = this.intInput;
    const nv = level ? (old | bits) : (old & ~bits);
    const rising = nv & ~old;
    this.intInput = nv;
    this.intHold |= rising & ~LEVEL_MASK;
    this.updateIrq();
  }
  pulse(bits) { // momentary edge (timers)
    this.intHold |= bits;
    this.updateIrq();
  }
  updateIrq() {
    const pending = ((this.intHold & ~LEVEL_MASK) | (this.intInput & LEVEL_MASK)) & this.intMask;
    this.cpu.irqLine = (pending & ~FIQ_MASK) !== 0;
    this.cpu.fiqLine = (pending & FIQ_MASK) !== 0;
    if (pending && this.sleeping) this.sleeping = false;
  }
  // INT_LATCH reads back only requests that are enabled in INT_MASK (the GUI's IRQ handler
  // relies on this: it checks the Fire bit to detect wake-up).
  intLatch() { return (((this.intHold & ~LEVEL_MASK) | (this.intInput & LEVEL_MASK)) & this.intMask) >>> 0; }

  // Buttons: mask of BTN.* currently held
  setButtons(mask) {
    mask &= 0x1F;
    if (mask === this.buttons) return;
    this.buttons = mask;
    this.intInput = (this.intInput & ~0x1F) | mask;
    this.updateIrq();
  }

  // ---------------- FLASH ----------------
  rebuildVirtMap() {
    this.virtMap.fill(-1);
    for (let i = 15; i >= 0; i--) {
      if (this.fBankFlg & (1 << i)) this.virtMap[this.fBankVal[i] & 15] = i;
    }
  }
  flash1Offset(addr) {
    const off = addr & 0x1FFFF;
    if (!(this.fCtrl & 2)) return off; // virtual mapping disabled -> physical
    const phys = this.virtMap[off >>> 13];
    if (phys < 0) return -1;
    return (phys << 13) | (off & 0x1FFF);
  }
  flashWrite(off, val, width) {
    // Unlock sequence (AA->55AA, 55->2A54, A0->55AA) arrives as 16-bit writes of FFxx.
    const k = off & 0x1FFFF;
    if (width === 2 && this.flashUnlock < 3) {
      const b = val & 0xFF;
      if (this.flashUnlock === 0 && k === 0x55AA && b === 0xAA) { this.flashUnlock = 1; return; }
      if (this.flashUnlock === 1 && k === 0x2A54 && b === 0x55) { this.flashUnlock = 2; return; }
      if (this.flashUnlock === 2 && k === 0x55AA && b === 0xA0) { this.flashUnlock = 3; this.pageBase = -1; return; }
    }
    if (!(this.fWait2 & 1) && this.flashUnlock < 3) return; // not in programming mode
    if (width === 4) this.flashDV.setUint32(k, val >>> 0, true);
    else if (width === 2) this.flashDV.setUint16(k, val & 0xFFFF, true);
    else this.flash[k] = val & 0xFF;
    const page = k & ~0x7F;
    if (page !== this.pageBase) {
      this.pageBase = page;
      if (this.onFlashWrite) this.onFlashWrite(page);
    }
    this.flashDirty = true;
  }

  // ---------------- bus: reads ----------------
  read32(a) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01:
        if (!this.remapped && a < 0x4000) return this.biosDV.getUint32(a & 0x3FFC, true);
        return this.ramDV.getUint32(a & 0x7FC, true);
      case 0x02: {
        const o = this.flash1Offset(a);
        return o < 0 ? 0 : this.flashDV.getUint32(o & ~3, true);
      }
      case 0x04: return this.biosDV.getUint32(a & 0x3FFC, true);
      case 0x08: return this.flashDV.getUint32(a & 0x1FFFC, true);
      default: return this.ioRead(a & ~3) >>> 0;
    }
  }
  read16(a) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01:
        if (!this.remapped && a < 0x4000) return this.biosDV.getUint16(a & 0x3FFE, true);
        return this.ramDV.getUint16(a & 0x7FE, true);
      case 0x02: {
        const o = this.flash1Offset(a);
        return o < 0 ? 0 : this.flashDV.getUint16(o & ~1, true);
      }
      case 0x04: return this.biosDV.getUint16(a & 0x3FFE, true);
      case 0x08: return this.flashDV.getUint16(a & 0x1FFFE, true);
      case 0x06:
        if ((a & 0xFFFF00) === 0x000300) return this.extra[(a >>> 1) & 0x7F];
      // fallthrough
      default: return (this.ioRead(a & ~3) >>> ((a & 2) << 3)) & 0xFFFF;
    }
  }
  read8(a) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01:
        if (!this.remapped && a < 0x4000) return this.bios[a & 0x3FFF];
        return this.ram[a & 0x7FF];
      case 0x02: {
        const o = this.flash1Offset(a);
        return o < 0 ? 0 : this.flash[o];
      }
      case 0x04: return this.bios[a & 0x3FFF];
      case 0x08: return this.flash[a & 0x1FFFF];
      case 0x06:
        if ((a & 0xFFFF00) === 0x000300) return (this.extra[(a >>> 1) & 0x7F] >>> ((a & 1) << 3)) & 0xFF;
      // fallthrough
      default: return (this.ioRead(a & ~3) >>> ((a & 3) << 3)) & 0xFF;
    }
  }

  // ---------------- bus: writes ----------------
  write32(a, v) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01: this.ramDV.setUint32(a & 0x7FC, v >>> 0, true); return;
      case 0x02: { const o = this.flash1Offset(a); if (o >= 0) this.flashWrite(o, v, 4); return; }
      case 0x08: this.flashWrite(a & 0x1FFFF, v, 4); return;
      case 0x04: return;
      default: this.ioWrite(a & ~3, v | 0, 0xFFFFFFFF);
    }
  }
  write16(a, v) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01: this.ramDV.setUint16(a & 0x7FE, v & 0xFFFF, true); return;
      case 0x02: { const o = this.flash1Offset(a); if (o >= 0) this.flashWrite(o, v, 2); return; }
      case 0x08: this.flashWrite(a & 0x1FFFF, v, 2); return;
      case 0x04: return;
      default: {
        const sh = (a & 2) << 3;
        this.ioWrite(a & ~3, (v & 0xFFFF) << sh, 0xFFFF << sh);
      }
    }
  }
  write8(a, v) {
    a >>>= 0;
    switch (a >>> 24) {
      case 0x00: case 0x01: this.ram[a & 0x7FF] = v; return;
      case 0x02: { const o = this.flash1Offset(a); if (o >= 0) this.flashWrite(o, v, 1); return; }
      case 0x08: this.flashWrite(a & 0x1FFFF, v, 1); return;
      case 0x04: return;
      default: {
        // byte writes to 32-bit I/O registers: treat as zero-extended full write (as the BIOS
        // uses STRB for F_CTRL and similar registers)
        this.ioWrite(a & ~3, (v & 0xFF) << ((a & 3) << 3), 0xFFFFFFFF);
      }
    }
  }

  // ---------------- I/O registers ----------------
  ioRead(a) {
    switch (a) {
      // FLASH control
      case 0x06000000: return this.fCtrl | (this.remapped ? 1 : 0);
      case 0x06000004: return 0;
      case 0x06000008: return this.fBankFlg;
      case 0x0600000C: return this.fWait1;
      case 0x06000010: return (this.fWait2 | 4) >>> 0; // bit2: program cycle complete
      // interrupt controller
      case 0x0A000000: return this.intLatch();
      case 0x0A000004: return this.intInput;
      case 0x0A000008: return this.intMask;
      case 0x0A00000C: return this.intMask;
      case 0x0A000010: return this.intMask;
      // timers
      case 0x0A800000: return this.timers[0].reload;
      case 0x0A800004: return this.timers[0].count;
      case 0x0A800008: return this.timers[0].mode;
      case 0x0A800010: return this.timers[1].reload;
      case 0x0A800014: return this.timers[1].count;
      case 0x0A800018: return this.timers[1].mode;
      case 0x0A800020: return this.timers[2].reload;
      case 0x0A800024: return this.timers[2].count;
      case 0x0A800028: return this.timers[2].mode;
      // clock
      case 0x0B000000: return (this.clkMode & 15) | 0x10; // PLL always locked
      case 0x0B000004: return 0;
      // RTC
      case 0x0B800000: return this.rtc.mode;
      case 0x0B800008: return this.rtcTime();
      case 0x0B80000C: return this.rtcDate();
      // COM (memory card port) - not docked
      case 0x0C000000: return this.comMode;
      case 0x0C000004: return 0;
      case 0x0C000008: return 0;
      case 0x0C000010: return this.comCtrl1;
      case 0x0C000014: return 0;
      case 0x0C000018: return this.comCtrl2;
      // IrDA
      case 0x0C800000: return this.irdaMode;
      case 0x0C800004: return 0;
      case 0x0C800008: return 0;
      case 0x0C80000C: return 0;
      // LCD
      case 0x0D000000: return this.lcdMode;
      case 0x0D000004: return this.lcdCal;
      // IOP / DAC / battery
      case 0x0D800000: return this.iopCtrl;
      case 0x0D800004: return 0;
      case 0x0D800008: return 0;
      case 0x0D80000C: return (this.iopData & ~0x10) | (this.docked ? 0x10 : 0);
      case 0x0D800010: return this.dacCtrl;
      case 0x0D800014: return this.dacData;
      case 0x0D800020: return this.battCtrl;
    }
    if (a >= 0x06000100 && a < 0x06000140) return this.fBankVal[(a >>> 2) & 15];
    if (a >= 0x06000300 && a < 0x06000400) {
      const i = (a >>> 1) & 0x7F;
      return (this.extra[i] | (this.extra[i + 1] << 16)) >>> 0;
    }
    if (a >= 0x0D000100 && a < 0x0D000180) return this.vram[(a >>> 2) & 31];
    if ((a >>> 24) === 0x0D && (a & 0x800000) === 0 && (a & 0xFFFF) >= 0x100) return this.vram[(a >>> 2) & 31]; // mirrors
    return 0;
  }

  ioWrite(a, v, mask) {
    switch (a) {
      case 0x06000000: if (v & 1) this.remapped = true; this.fCtrl = (v & 3) | (this.remapped ? 1 : 0); return; // GENREM is sticky once set
      case 0x06000004: return;
      case 0x06000008: this.fBankFlg = v & 0xFFFF; this.rebuildVirtMap(); return;
      case 0x0600000C: this.fWait1 = v; return;
      case 0x06000010:
        this.fWait2 = v & ~4;
        if (!(v & 1)) { this.flashUnlock = 0; this.pageBase = -1; }
        return;
      case 0x0A000000: return;
      case 0x0A000004: return;
      case 0x0A000008: this.intMask = (this.intMask | v) & 0xFFFF; this.updateIrq(); return;
      case 0x0A00000C: this.intMask = (this.intMask & ~v) & 0xFFFF; this.updateIrq(); return;
      case 0x0A000010: this.intHold &= ~v; this.updateIrq(); return;
      case 0x0A800000: this.timers[0].reload = v & 0xFFFF; return;
      case 0x0A800004: this.timers[0].count = v & 0xFFFF; return;
      case 0x0A800008: this.timerMode(0, v); return;
      case 0x0A800010: this.timers[1].reload = v & 0xFFFF; return;
      case 0x0A800014: this.timers[1].count = v & 0xFFFF; return;
      case 0x0A800018: this.timerMode(1, v); return;
      case 0x0A800020: this.timers[2].reload = v & 0xFFFF; return;
      case 0x0A800024: this.timers[2].count = v & 0xFFFF; return;
      case 0x0A800028: this.timerMode(2, v); return;
      case 0x0B000000: this.clkMode = v & 15; return;
      case 0x0B000004:
        if (v & 1) {
          const pending = this.intLatch() & this.intMask;
          if (!pending) { this.sleeping = true; this.cpu.stopRequested = true; }
        }
        return;
      case 0x0B800000: this.rtc.mode = v & 15; return;
      case 0x0B800004: {
        // only one adjustment step per RTC square-wave cycle (extra writes are ignored)
        const field = (this.rtc.mode >>> 1) & 7;
        if (field < 7 && !this.rtc.adjusted) { this.rtcIncrement(field); this.rtc.adjusted = true; }
        return;
      }
      case 0x0C000000: this.comMode = v; return;
      case 0x0C000010: this.comCtrl1 = v; return;
      case 0x0C000018: this.comCtrl2 = v; return;
      case 0x0C800000: this.irdaMode = v; return;
      case 0x0D000000: this.lcdMode = v & 0xFF; return;
      case 0x0D000004: this.lcdCal = v; return;
      case 0x0D800000: this.iopCtrl = v; return;
      case 0x0D800004: this.iopData |= v; return;   // IOP_STOP: set bits
      case 0x0D800008: this.iopData &= ~v; return;  // IOP_START: clear bits
      case 0x0D80000C: return;
      case 0x0D800010: this.dacCtrl = v; return;
      case 0x0D800014: this.dacData = v; return;
      case 0x0D800020: this.battCtrl = v; return;
    }
    if (a >= 0x06000100 && a < 0x06000140) { this.fBankVal[(a >>> 2) & 15] = v & 15; this.rebuildVirtMap(); return; }
    if (a >= 0x0D000100 && a < 0x0D000180) {
      const i = (a >>> 2) & 31;
      this.vram[i] = ((this.vram[i] & ~mask) | (v & mask)) >>> 0;
      return;
    }
  }

  timerMode(i, v) {
    const t = this.timers[i];
    // Re-enabling does not reload the counter (the GUI stops/starts T0 every frame and
    // relies on it continuing, giving an exact 30 Hz frame rate).
    t.mode = v & 0xFFFF;
  }

  // ---------------- time keeping ----------------
  // Advance hardware by `cycles` system clocks.
  tick(cycles) {
    const clk = this.clock;
    for (let i = 0; i < 3; i++) {
      const t = this.timers[i];
      if (!(t.mode & 4)) continue;
      const div = TIMER_DIV[t.mode & 3];
      t.acc += cycles;
      if (t.acc < div) continue;
      let steps = Math.floor(t.acc / div);
      t.acc -= steps * div;
      while (steps > 0) {
        if (steps <= t.count) { t.count -= steps; steps = 0; }
        else {
          steps -= t.count + 1;
          t.count = t.reload;
          this.pulse(i === 0 ? INT_T0 : i === 1 ? INT_T1 : INT_T2);
          if (t.reload === 0 && steps > 0) { steps = 0; }
        }
      }
    }
    const dt = cycles / clk;
    this.advanceRealtime(dt);
  }

  advanceRealtime(dt) {
    this.time += dt;
    // RTC square wave: 1 Hz (4096 Hz when paused); rising edge at phase 0
    const r = this.rtc;
    const paused = r.mode & 1;
    const period = paused ? 1 / 4096 : 1;
    r.phase += dt;
    while (r.phase >= period) {
      r.phase -= period;
      if (!paused) this.rtcTickSecond();
      r.adjusted = false;
      this.setInput(INT_RTC, 1);
    }
    if (r.phase >= period / 2 && (this.intInput & INT_RTC)) this.setInput(INT_RTC, 0);
    // audio
    if (this.audioOut) {
      const step = 1 / this.sampleRate;
      const on = (this.dacCtrl & 1) && !(this.iopData & 0x20);
      const level = on ? (((this.dacData << 16) >> 22) / 512) * 0.8 : 0;
      this.audioPhase += dt;
      while (this.audioPhase >= step) {
        this.audioPhase -= step;
        this.audioBuf[this.audioPos++] = level;
        if (this.audioPos === this.audioBuf.length) { this.audioOut(this.audioBuf); this.audioBuf = new Float32Array(1024); this.audioPos = 0; }
      }
    }
  }

  cyclesToNextEvent() {
    let best = 1e9;
    for (let i = 0; i < 3; i++) {
      const t = this.timers[i];
      if (!(t.mode & 4)) continue;
      const div = TIMER_DIV[t.mode & 3];
      const c = (t.count + 1) * div - t.acc;
      if (c < best) best = c;
    }
    const r = this.rtc;
    const period = (r.mode & 1) ? 1 / 4096 : 1;
    const toEdge = (r.phase < period / 2 ? period / 2 - r.phase : period - r.phase);
    const c = Math.ceil(toEdge * this.clock) + 1;
    if (c < best) best = c;
    return Math.max(1, best);
  }

  // Run for `seconds` of emulated time (the CPU clock may change while running).
  runFor(seconds) {
    const end = this.time + seconds;
    let guard = 0;
    while (this.time < end && guard++ < 5e6) {
      const remain = Math.max(1, Math.ceil((end - this.time) * this.clock));
      if (this.sleeping) {
        // fast-forward to the next hardware event
        this.tick(Math.min(remain, this.cyclesToNextEvent()));
        continue;
      }
      const slice = Math.min(remain, 256, this.cyclesToNextEvent());
      this.tick(this.cpu.run(slice));
    }
  }

  // Cold boot straight into a FLASH file (directory index 1..15), skipping the BIOS GUI:
  // run the kernel's reset code until it hands control to the GUI, set the clock from the
  // host, then call the kernel's own PrepareExecute/DoExecute (SWI 08h/09h) from a stub.
  // Returns false if the kernel didn't reach the GUI entry (unknown BIOS).
  bootFile(dirIndex, date = new Date()) {
    this.hardReset(date);
    const GUI_ENTRY = 0x04001E00;
    const c = this.cpu;
    let n = 0;
    while (c.pc !== GUI_ENTRY && n++ < 2000000) {
      const before = c.cycles; c.step(); this.tick(c.cycles - before);
    }
    if (c.pc !== GUI_ENTRY) return false;
    if (!dirIndex) return this.bootMenu(date);
    this.syncClock(date);
    const stub = [
      0xE3A00001,                        // mov r0,#1
      0xE3A01000 | (dirIndex & 0xFF),    // mov r1,#dir
      0xE3A02000,                        // mov r2,#0
      0xEF000008,                        // swi 8   PrepareExecute(1,dir,0)
      0xE3A00000,                        // mov r0,#0
      0xEF000009,                        // swi 9   DoExecute(0)
      0xEAFFFFFE,                        // b .
    ];
    const base = 0x7C0;
    stub.forEach((w, i) => this.ramDV.setUint32(base + i * 4, w >>> 0, true));
    c.t = 0;
    c.pc = base;
    return true;
  }

  // Boot into the BIOS menu with the clock already set. A cold-booted GUI insists on its
  // "set the date" screen (it keeps the "clock was set" flag in battery-backed RAM), and
  // changing the RTC underneath that screen confuses it. So: let the GUI reach its date
  // screen, accept it with the Fire button like a user would, then set the real time.
  // Called from bootFile() after hardReset + kernel init.
  bootMenu(date = new Date()) {
    // The main menu is recognised by the memory-card icon frame (a solid 6-pixel bar at the
    // left of row 14). Blinking is defeated by OR-ing frames over a second.
    const atMainMenu = () => {
      let row = 0;
      for (let i = 0; i < 10; i++) { this.runFor(0.1); row |= this.displayOn ? this.vram[14] : 0; }
      return ((this.lcdMode & 0x80) ? (row >>> 26) === 0x3F : (row & 0x3F) === 0x3F);
    };
    const tap = (b) => { this.setButtons(b); this.runFor(0.1); this.setButtons(0); this.runFor(0.25); };
    this.runFor(9); // "hello" + heart animation
    // The date screen only accepts Fire after a field was edited, so nudge the year up and
    // back down first (the real time is written afterwards anyway).
    for (let i = 0; i < 6 && !atMainMenu(); i++) {
      if (i % 2 === 0) { tap(16); tap(8); }
      tap(1);
    }
    this.syncClock(date);
    return true;
  }

  // Set the RTC (and the kernel's century bytes) from a host Date.
  syncClock(date = new Date()) {
    this.rtcSetFromDate(date);
    const y = date.getFullYear();
    this.ram[0xCF] = toBCD(Math.floor(y / 100) % 100);
    this.ram[0xCD] = toBCD(y % 100);
  }

  // ---------------- video ----------------
  // Returns Uint8Array(1024) of pixels (1 = black) in on-screen orientation.
  get displayOn() { return (this.lcdMode & 0x40) !== 0; }
  get ledOn() { return (this.iopData & 2) === 0 && (this.iopCtrl & 2) !== 0; }
  getFrame(out) {
    out = out || new Uint8Array(1024);
    if (!this.displayOn) { out.fill(0); return out; }
    const rot = (this.lcdMode & 0x80) !== 0;
    for (let y = 0; y < 32; y++) {
      const w = this.vram[y];
      for (let x = 0; x < 32; x++) {
        const px = (w >>> x) & 1;
        if (rot) out[(31 - y) * 32 + (31 - x)] = px; else out[y * 32 + x] = px;
      }
    }
    return out;
  }

  // ---------------- save states ----------------
  saveState() {
    const c = this.cpu;
    return {
      v: STATE_VERSION,
      ram: Array.from(this.ram), vram: Array.from(this.vram),
      cpu: {
        r: Array.from(c.r), b13: Array.from(c.bankR13), b14: Array.from(c.bankR14), spsr: Array.from(c.spsrBank),
        u8: Array.from(c.usrR8), f8: Array.from(c.fiqR8), cpsr: c.getCPSR(), pc: c.pc,
      },
      io: {
        fCtrl: this.fCtrl, remapped: this.remapped, fBankFlg: this.fBankFlg, fBankVal: Array.from(this.fBankVal), fWait1: this.fWait1, fWait2: this.fWait2,
        intInput: this.intInput & ~0x1F, intHold: this.intHold, intMask: this.intMask,
        timers: this.timers.map((t) => ({ ...t })), clkMode: this.clkMode, sleeping: this.sleeping,
        lcdMode: this.lcdMode, lcdCal: this.lcdCal, iopCtrl: this.iopCtrl, iopData: this.iopData,
        dacCtrl: this.dacCtrl, dacData: this.dacData, battCtrl: this.battCtrl, irdaMode: this.irdaMode,
        rtcMode: this.rtc.mode, rtcPhase: this.rtc.phase,
      },
    };
  }
  loadState(s, date) {
    const c = this.cpu;
    this.ram.set(s.ram); this.vram.set(s.vram);
    c.mode = 0x1F; // neutral; setCPSR below performs banking
    c.r.set(s.cpu.r); c.bankR13.set(s.cpu.b13); c.bankR14.set(s.cpu.b14); c.spsrBank.set(s.cpu.spsr);
    c.usrR8.set(s.cpu.u8); c.fiqR8.set(s.cpu.f8);
    const cpsr = s.cpu.cpsr;
    c.n = (cpsr >>> 31) & 1; c.z = (cpsr >>> 30) & 1; c.c = (cpsr >>> 29) & 1; c.v = (cpsr >>> 28) & 1;
    c.iflag = (cpsr >>> 7) & 1; c.fflag = (cpsr >>> 6) & 1; c.t = (cpsr >>> 5) & 1; c.mode = cpsr & 0x1F;
    c.pc = s.cpu.pc;
    const io = s.io;
    Object.assign(this, {
      fCtrl: io.fCtrl, remapped: io.remapped !== false, fBankFlg: io.fBankFlg, fWait1: io.fWait1, fWait2: io.fWait2,
      intInput: io.intInput, intHold: io.intHold, intMask: io.intMask, clkMode: io.clkMode, sleeping: io.sleeping,
      lcdMode: io.lcdMode, lcdCal: io.lcdCal, iopCtrl: io.iopCtrl, iopData: io.iopData, dacCtrl: io.dacCtrl,
      dacData: io.dacData, battCtrl: io.battCtrl, irdaMode: io.irdaMode,
    });
    this.fBankVal.set(io.fBankVal); this.rebuildVirtMap();
    this.timers = io.timers.map((t) => ({ ...t }));
    this.rtcInit(date || new Date());
    this.rtc.mode = io.rtcMode; this.rtc.phase = io.rtcPhase;
    this.buttons = 0;
    this.updateIrq();
  }
}
