// Small ARM/Thumb disassembler (debugging aid).
const COND = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv'];
const DP = ['and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn', 'orr', 'mov', 'bic', 'mvn'];
const SH = ['lsl', 'lsr', 'asr', 'ror'];
const R = (i) => ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'sp', 'lr', 'pc'][i];
const hex = (v) => '0x' + (v >>> 0).toString(16);
const rlist = (l) => { const a = []; for (let i = 0; i < 16; i++) if (l & (1 << i)) a.push(R(i)); return '{' + a.join(',') + '}'; };

export function disasmARM(op, addr) {
  op >>>= 0;
  const c = COND[op >>> 28];
  const rn = (op >>> 16) & 15, rd = (op >>> 12) & 15, rs = (op >>> 8) & 15, rm = op & 15;
  if ((op & 0x0FFFFFF0) === 0x012FFF10) return `bx${c} ${R(rm)}`;
  if ((op & 0x0FC000F0) === 0x00000090) return `${op & 0x200000 ? 'mla' : 'mul'}${c}${op & 0x100000 ? 's' : ''} ${R(rn)},${R(rm)},${R(rs)}${op & 0x200000 ? ',' + R(rd) : ''}`;
  if ((op & 0x0F8000F0) === 0x00800090) return `${op & 0x400000 ? 's' : 'u'}${op & 0x200000 ? 'mlal' : 'mull'}${c} ${R(rd)},${R(rn)},${R(rm)},${R(rs)}`;
  if ((op & 0x0FB00FF0) === 0x01000090) return `swp${c}${op & 0x400000 ? 'b' : ''} ${R(rd)},${R(rm)},[${R(rn)}]`;
  if ((op & 0x0E000090) === 0x00000090) {
    const sh = (op >>> 5) & 3, l = op & 0x100000;
    const nm = l ? ['', 'ldrh', 'ldrsb', 'ldrsh'][sh] : ['', 'strh', '?', '?'][sh];
    const off = op & 0x400000 ? '#' + hex(((op >>> 4) & 0xF0) | rm) : R(rm);
    const sign = op & 0x800000 ? '' : '-';
    return op & 0x1000000 ? `${nm}${c} ${R(rd)},[${R(rn)},${sign}${off}]${op & 0x200000 ? '!' : ''}` : `${nm}${c} ${R(rd)},[${R(rn)}],${sign}${off}`;
  }
  if ((op & 0x0FBF0FFF) === 0x010F0000) return `mrs${c} ${R(rd)},${op & 0x400000 ? 'spsr' : 'cpsr'}`;
  if ((op & 0x0DB0F000) === 0x0120F000) {
    const f = (op >>> 16) & 15;
    const src = op & 0x2000000 ? '#' + hex(((op & 0xFF) >>> (((op >>> 8) & 15) * 2)) | ((op & 0xFF) << (32 - ((op >>> 8) & 15) * 2))) : R(rm);
    return `msr${c} ${op & 0x400000 ? 'spsr' : 'cpsr'}_${f & 1 ? 'c' : ''}${f & 2 ? 'x' : ''}${f & 4 ? 's' : ''}${f & 8 ? 'f' : ''},${src}`;
  }
  const t = (op >>> 25) & 7;
  if (t === 0 || t === 1) {
    const opc = (op >>> 21) & 15, s = (op >>> 20) & 1;
    let o2;
    if (t === 1) { const rot = ((op >>> 8) & 15) * 2; const imm = op & 0xFF; o2 = '#' + hex(rot ? (imm >>> rot) | (imm << (32 - rot)) : imm); }
    else if (op & 0x10) o2 = `${R(rm)},${SH[(op >>> 5) & 3]} ${R(rs)}`;
    else { const a = (op >>> 7) & 31, ty = (op >>> 5) & 3; o2 = R(rm) + ((a || ty) ? (ty === 3 && !a ? ',rrx' : `,${SH[ty]} #${a || 32}`) : ''); }
    const nm = DP[opc] + c + (s && (opc < 8 || opc > 11) ? 's' : '');
    if (opc >= 8 && opc <= 11) return `${nm} ${R(rn)},${o2}`;
    if (opc === 13 || opc === 15) return `${nm} ${R(rd)},${o2}`;
    return `${nm} ${R(rd)},${R(rn)},${o2}`;
  }
  if (t === 2 || t === 3) {
    const l = op & 0x100000, b = op & 0x400000 ? 'b' : '';
    let off;
    if (t === 2) off = '#' + (op & 0x800000 ? '' : '-') + hex(op & 0xFFF);
    else { const a = (op >>> 7) & 31; off = (op & 0x800000 ? '' : '-') + R(rm) + (a ? `,${SH[(op >>> 5) & 3]} #${a}` : ''); }
    let extra = '';
    if (rn === 15 && t === 2 && (op & 0x1000000)) extra = ` ; =${hex((addr + 8 + (op & 0x800000 ? 1 : -1) * (op & 0xFFF)))}`;
    return op & 0x1000000 ? `${l ? 'ldr' : 'str'}${c}${b} ${R(rd)},[${R(rn)},${off}]${op & 0x200000 ? '!' : ''}${extra}` : `${l ? 'ldr' : 'str'}${c}${b} ${R(rd)},[${R(rn)}],${off}`;
  }
  if (t === 4) {
    const mode = (op & 0x800000 ? 'i' : 'd') + (op & 0x1000000 ? 'b' : 'a');
    return `${op & 0x100000 ? 'ldm' : 'stm'}${c}${mode} ${R(rn)}${op & 0x200000 ? '!' : ''},${rlist(op & 0xFFFF)}${op & 0x400000 ? '^' : ''}`;
  }
  if (t === 5) return `b${op & 0x1000000 ? 'l' : ''}${c} ${hex(addr + 8 + ((op << 8) >> 6))}`;
  if (t === 7 && (op & 0x1000000)) return `swi${c} ${hex(op & 0xFFFFFF)}`;
  return `undef ${hex(op)}`;
}

export function disasmThumb(op, addr) {
  const rd = op & 7, rs = (op >>> 3) & 7;
  if ((op & 0xF800) === 0x1800) return `${op & 0x200 ? 'sub' : 'add'} ${R(rd)},${R(rs)},${op & 0x400 ? '#' + ((op >>> 6) & 7) : R((op >>> 6) & 7)}`;
  if ((op & 0xE000) === 0x0000) return `${SH[(op >>> 11) & 3]} ${R(rd)},${R(rs)},#${(op >>> 6) & 31}`;
  if ((op & 0xE000) === 0x2000) return `${['mov', 'cmp', 'add', 'sub'][(op >>> 11) & 3]} ${R((op >>> 8) & 7)},#${hex(op & 0xFF)}`;
  if ((op & 0xFC00) === 0x4000) return `${['and', 'eor', 'lsl', 'lsr', 'asr', 'adc', 'sbc', 'ror', 'tst', 'neg', 'cmp', 'cmn', 'orr', 'mul', 'bic', 'mvn'][(op >>> 6) & 15]} ${R(rd)},${R(rs)}`;
  if ((op & 0xFC00) === 0x4400) {
    const o = (op >>> 8) & 3, h1 = (op & 7) | ((op >>> 4) & 8), h2 = (op >>> 3) & 15;
    if (o === 3) return `bx ${R(h2)}`;
    return `${['add', 'cmp', 'mov'][o]} ${R(h1)},${R(h2)}`;
  }
  if ((op & 0xF800) === 0x4800) return `ldr ${R((op >>> 8) & 7)},[pc,#${hex((op & 0xFF) << 2)}] ; =${hex(((addr + 4) & ~3) + ((op & 0xFF) << 2))}`;
  if ((op & 0xF000) === 0x5000) return `${['str', 'strh', 'strb', 'ldsb', 'ldr', 'ldrh', 'ldrb', 'ldsh'][(op >>> 9) & 7]} ${R(rd)},[${R(rs)},${R((op >>> 6) & 7)}]`;
  if ((op & 0xE000) === 0x6000) { const b = op & 0x1000; return `${op & 0x800 ? 'ldr' : 'str'}${b ? 'b' : ''} ${R(rd)},[${R(rs)},#${hex(((op >>> 6) & 31) << (b ? 0 : 2))}]`; }
  if ((op & 0xF000) === 0x8000) return `${op & 0x800 ? 'ldrh' : 'strh'} ${R(rd)},[${R(rs)},#${hex(((op >>> 6) & 31) << 1)}]`;
  if ((op & 0xF000) === 0x9000) return `${op & 0x800 ? 'ldr' : 'str'} ${R((op >>> 8) & 7)},[sp,#${hex((op & 0xFF) << 2)}]`;
  if ((op & 0xF000) === 0xA000) return `add ${R((op >>> 8) & 7)},${op & 0x800 ? 'sp' : 'pc'},#${hex((op & 0xFF) << 2)}`;
  if ((op & 0xFF00) === 0xB000) return `add sp,#${op & 0x80 ? '-' : ''}${hex((op & 0x7F) << 2)}`;
  if ((op & 0xF600) === 0xB400) { let l = op & 0xFF; if (op & 0x100) l |= op & 0x800 ? 0x8000 : 0x4000; return `${op & 0x800 ? 'pop' : 'push'} ${rlist(l)}`; }
  if ((op & 0xF000) === 0xC000) return `${op & 0x800 ? 'ldmia' : 'stmia'} ${R((op >>> 8) & 7)}!,${rlist(op & 0xFF)}`;
  if ((op & 0xFF00) === 0xDF00) return `swi ${hex(op & 0xFF)}`;
  if ((op & 0xF000) === 0xD000) return `b${COND[(op >>> 8) & 15]} ${hex(addr + 4 + (((op & 0xFF) << 24) >> 23))}`;
  if ((op & 0xF800) === 0xE000) return `b ${hex(addr + 4 + (((op & 0x7FF) << 21) >> 20))}`;
  if ((op & 0xF800) === 0xF000) return `bl.hi ${hex(((op & 0x7FF) << 21) >> 9)}`;
  if ((op & 0xF800) === 0xF800) return `bl.lo ${hex((op & 0x7FF) << 1)}`;
  return `undef ${hex(op)}`;
}
