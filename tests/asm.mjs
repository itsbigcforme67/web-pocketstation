// Tiny two-pass ARM assembler (just enough to build test programs / the built-in demo).
const COND = { eq: 0, ne: 1, cs: 2, hs: 2, cc: 3, lo: 3, mi: 4, pl: 5, vs: 6, vc: 7, hi: 8, ls: 9, ge: 10, lt: 11, gt: 12, le: 13, al: 14, '': 14 };
const DP = { and: 0, eor: 1, sub: 2, rsb: 3, add: 4, adc: 5, sbc: 6, rsc: 7, tst: 8, teq: 9, cmp: 10, cmn: 11, orr: 12, mov: 13, bic: 14, mvn: 15 };
const SH = { lsl: 0, lsr: 1, asr: 2, ror: 3 };

export function encImm(v) {
  v >>>= 0;
  for (let rot = 0; rot < 16; rot++) {
    const r = ((v << (rot * 2)) | (v >>> (32 - rot * 2))) >>> 0;
    if (r < 256) return (rot << 8) | r;
  }
  return -1;
}

export class Asm {
  constructor(base) { this.base = base; this.items = []; this.labels = {}; this.lits = []; }
  get pc() { return this.base + this.items.length * 4; }
  label(n) { this.labels[n] = this.pc; return this; }
  word(fnOrVal) { this.items.push(fnOrVal); return this; }
  // data processing: dp('add','ne',true, rd, rn, op2) ; op2 = {imm} | {r, sh, amt} | {r, sh, rs}
  dp(op, cond, s, rd, rn, o2) {
    let w = (COND[cond] << 28) | (DP[op] << 21) | ((s ? 1 : 0) << 20) | (rn << 16) | (rd << 12);
    if (typeof o2 === 'number') {
      const e = encImm(o2); if (e < 0) throw new Error('bad imm ' + o2.toString(16));
      w |= (1 << 25) | e;
    } else if (o2.rs !== undefined) w |= (o2.rs << 8) | (SH[o2.sh] << 5) | 0x10 | o2.r;
    else w |= ((o2.amt || 0) << 7) | (SH[o2.sh || 'lsl'] << 5) | o2.r;
    return this.word(w >>> 0);
  }
  ldst(l, b, cond, rd, rn, off, { pre = true, wb = false } = {}) {
    const u = off >= 0 ? 1 : 0;
    const w = (COND[cond] << 28) | (1 << 26) | ((pre ? 1 : 0) << 24) | (u << 23) | (b << 22) | ((wb ? 1 : 0) << 21) | (l << 20) | (rn << 16) | (rd << 12) | Math.abs(off);
    return this.word(w >>> 0);
  }
  ldr(rd, rn, off = 0, cond = 'al', o) { return this.ldst(1, 0, cond, rd, rn, off, o); }
  str(rd, rn, off = 0, cond = 'al', o) { return this.ldst(0, 0, cond, rd, rn, off, o); }
  ldrb(rd, rn, off = 0, cond = 'al') { return this.ldst(1, 1, cond, rd, rn, off); }
  strb(rd, rn, off = 0, cond = 'al') { return this.ldst(0, 1, cond, rd, rn, off); }
  // load 32-bit constant via literal pool
  li(rd, val, cond = 'al') {
    const idx = this.items.length;
    const lit = { val: val >>> 0 };
    this.lits.push(lit);
    this.items.push(() => {
      const pc = this.base + idx * 4;
      const off = lit.addr - (pc + 8);
      return ((COND[cond] << 28) | 0x05900000 | ((off >= 0 ? 1 : 0) << 23) | (15 << 16) | (rd << 12) | Math.abs(off)) >>> 0;
    });
    return this;
  }
  b(target, cond = 'al', link = false) {
    const idx = this.items.length;
    this.items.push(() => {
      const pc = this.base + idx * 4;
      const t = typeof target === 'string' ? this.labels[target] : target;
      if (t === undefined) throw new Error('no label ' + target);
      return ((COND[cond] << 28) | ((link ? 0xB : 0xA) << 24) | (((t - pc - 8) >> 2) & 0xFFFFFF)) >>> 0;
    });
    return this;
  }
  bl(t, c) { return this.b(t, c, true); }
  swi(n, cond = 'al') { return this.word(((COND[cond] << 28) | 0x0F000000 | n) >>> 0); }
  bx(rm, cond = 'al') { return this.word(((COND[cond] << 28) | 0x012FFF10 | rm) >>> 0); }
  stmfd(rn, list) { return this.word((0xE9200000 | (rn << 16) | list) >>> 0); }
  ldmfd(rn, list) { return this.word((0xE8B00000 | (rn << 16) | list) >>> 0); }
  assemble() {
    // place literal pool after code
    let addr = this.pc;
    for (const l of this.lits) { l.addr = addr; addr += 4; }
    const out = new Uint32Array(this.items.length + this.lits.length);
    this.items.forEach((it, i) => { out[i] = typeof it === 'function' ? it() : it; });
    this.lits.forEach((l, i) => { out[this.items.length + i] = l.val; });
    return new Uint8Array(out.buffer);
  }
}
