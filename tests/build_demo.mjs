// Builds a tiny PocketStation test program ("Pocket Test") as a memory card image.
// Move a box with the D-pad, FIRE inverts the screen and beeps, hold FIRE+UP to exit.
import fs from 'fs';
import { Asm } from './asm.mjs';
import { formatCard } from '../web/core/memcard.js';

const CODE_OFF = 0x200;               // code starts in sector 4 of the file
const ENTRY = 0x02000000 + CODE_OFF;
const a = new Asm(ENTRY);
const R = { r0: 0, r1: 1, r2: 2, r3: 3, r4: 4, r5: 5, r6: 6, r7: 7, r8: 8, r9: 9, r10: 10, r11: 11, r12: 12, sp: 13, lr: 14 };
const { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12 } = R;
const DELAY = +(process.env.DELAY || 1500);

a.li(r11, 0x0D000000);                       // LCD
a.dp('mov', 'al', 0, r0, 0, 0x58); a.str(r0, r11, 0);
a.li(r10, 0x0A000000);                       // INT
a.li(r9, 0x0D800000);                        // IOP/DAC
a.dp('mov', 'al', 0, r0, 0, 1); a.str(r0, r9, 0x10);     // DAC_CTRL on
a.dp('mov', 'al', 0, r0, 0, 0x20); a.str(r0, r9, 0x08);  // IOP_START bit5: speaker on
a.dp('mov', 'al', 0, r4, 0, 14);
a.dp('mov', 'al', 0, r5, 0, 14);
a.dp('mov', 'al', 0, r6, 0, 0);
a.dp('mov', 'al', 0, r7, 0, 0);
a.dp('mov', 'al', 0, r8, 0, 0);
a.label('loop');
a.ldr(r0, r10, 4);                            // INT_INPUT
a.dp('and', 'al', 0, r0, r0, 0x1F);
a.dp('bic', 'al', 0, r1, r0, { r: r8 });      // newly pressed
a.dp('mov', 'al', 0, r8, 0, { r: r0 });
a.dp('tst', 'al', 1, 0, r1, 1);
a.dp('eor', 'ne', 0, r6, r6, 1);
a.dp('mov', 'ne', 0, r7, 0, 6);
a.dp('tst', 'al', 1, 0, r0, 2); a.dp('add', 'ne', 0, r4, r4, 1);
a.dp('tst', 'al', 1, 0, r0, 4); a.dp('sub', 'ne', 0, r4, r4, 1);
a.dp('tst', 'al', 1, 0, r0, 8); a.dp('add', 'ne', 0, r5, r5, 1);
a.dp('tst', 'al', 1, 0, r0, 16); a.dp('sub', 'ne', 0, r5, r5, 1);
a.dp('cmp', 'al', 1, 0, r4, 0); a.dp('mov', 'lt', 0, r4, 0, 0);
a.dp('cmp', 'al', 1, 0, r4, 28); a.dp('mov', 'gt', 0, r4, 0, 28);
a.dp('cmp', 'al', 1, 0, r5, 0); a.dp('mov', 'lt', 0, r5, 0, 0);
a.dp('cmp', 'al', 1, 0, r5, 28); a.dp('mov', 'gt', 0, r5, 0, 28);
// draw
a.dp('mov', 'al', 0, r1, 0, 0);
a.dp('add', 'al', 0, r2, r11, 0x100);
a.dp('rsb', 'al', 0, r3, r6, 0);              // 0 or -1
a.dp('mov', 'al', 0, r12, 0, 0xF);
a.label('row');
a.dp('mov', 'al', 0, r0, 0, { r: r3 });
a.dp('sub', 'al', 0, r9, r1, { r: r5 });      // reuse r9 temporarily
a.dp('cmp', 'al', 1, 0, r9, 3);
a.dp('eor', 'ls', 0, r0, r0, { r: r12, sh: 'lsl', rs: r4 });
// border: rows 0 and 31 full lines, others edge pixels
a.dp('cmp', 'al', 1, 0, r1, 0);
a.dp('mvn', 'eq', 0, r0, 0, { r: r0 });
a.dp('cmp', 'al', 1, 0, r1, 31);
a.dp('mvn', 'eq', 0, r0, 0, { r: r0 });
a.str(r0, r2, 4, 'al', { pre: false });       // str r0,[r2],#4
a.dp('add', 'al', 0, r1, r1, 1);
a.dp('cmp', 'al', 1, 0, r1, 32);
a.b('row', 'lt');
a.li(r9, 0x0D800000);
// delay + beep
a.li(r1, DELAY);
a.label('delay');
a.dp('cmp', 'al', 1, 0, r7, 0);
a.b('nobeep', 'eq');
a.dp('and', 'al', 1, r0, r1, 16);
a.dp('mov', 'ne', 0, r0, 0, 0x7F00);
a.dp('mov', 'eq', 0, r0, 0, 0x8100);
a.str(r0, r9, 0x14);
a.label('nobeep');
a.dp('sub', 'al', 1, r1, r1, 1);
a.b('delay', 'ne');
a.dp('cmp', 'al', 1, 0, r7, 0);
a.dp('sub', 'ne', 0, r7, r7, 1);
a.dp('and', 'al', 0, r0, r8, 0x11);
a.dp('cmp', 'al', 1, 0, r0, 0x11);
a.b('loop', 'ne');
// exit back to the GUI: PrepareExecute(1, 0, 0) ; DoExecute(0)
a.dp('mov', 'al', 0, r0, 0, 0); a.str(r0, r9, 0x10);
a.dp('mov', 'al', 0, r0, 0, 1);
a.dp('mov', 'al', 0, r1, 0, 0);
a.dp('mov', 'al', 0, r2, 0, 0);
a.swi(8);
a.dp('mov', 'al', 0, r0, 0, 0);
a.swi(9);
a.label('hang'); a.b('hang');

const code = a.assemble();

function bitmapFromArt(art) {
  const out = new Uint8Array(128);
  art.forEach((line, y) => {
    let w = 0;
    for (let x = 0; x < 32; x++) if (line[x] === '#') w |= (1 << x);
    new DataView(out.buffer).setUint32(y * 4, w >>> 0, true);
  });
  return out;
}

const mono = bitmapFromArt([
  '################################',
  '#..............................#',
  '#..............................#',
  '#...####...####....###..#...#..#',
  '#...#...#.#....#..#.....#..#...#',
  '#...#...#.#....#.#......#.#....#',
  '#...####..#....#.#......##.....#',
  '#...#.....#....#.#......#.#....#',
  '#...#.....#....#..#.....#..#...#',
  '#...#......####....###..#...#..#',
  '#..............................#',
  '#..............................#',
  '#..#####.#####..####.#####.....#',
  '#....#...#.....#.......#.......#',
  '#....#...####...###....#.......#',
  '#....#...#.........#...#.......#',
  '#....#...#####.####....#.......#',
  '#..............................#',
  '#..............................#',
  '#.............####.............#',
  '#.............####.............#',
  '#.............####.............#',
  '#.............####.............#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '#..............................#',
  '################################',
]);

export function buildFile() {
  const file = new Uint8Array(0x2000);
  const dv = new DataView(file.buffer);
  file.set([0x53, 0x43, 0x11, 0x01]);              // "SC", 1 icon frame, 1 block
  const title = 'POCKET TEST';
  for (let i = 0; i < title.length; i++) file[4 + i] = title.charCodeAt(i);
  dv.setUint16(0x50, 0, true);                       // viewer icons: use exec icon
  file.set([0x4D, 0x43, 0x58, 0x30], 0x52);          // "MCX0"
  file[0x56] = 1;                                     // 1 exec mono icon entry
  file[0x57] = 0;
  dv.setUint32(0x5C, ENTRY, true);
  // palette: 0 transparent, 1 black, 2 white, 3 blue
  dv.setUint16(0x60 + 2, 0x8000, true); dv.setUint16(0x60 + 4, 0xFFFF, true); dv.setUint16(0x60 + 6, 0xFC00 | 0x0210, true);
  // 16x16 colour icon: blue screen with white box
  for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
    let c = (x === 0 || y === 0 || x === 15 || y === 15) ? 1 : 3;
    if (x >= 6 && x <= 9 && y >= 6 && y <= 9) c = 2;
    const i = 0x80 + y * 8 + (x >> 1);
    file[i] |= (x & 1) ? (c << 4) : c;
  }
  // exec mono icon list at 0x100: 1 frame, delay 1, address
  dv.setUint16(0x100, 1, true); dv.setUint16(0x102, 1, true); dv.setUint32(0x104, 0x02000180, true);
  file.set(mono, 0x180);
  file.set(code, CODE_OFF);
  return file;
}

export function buildCard() {
  const card = formatCard();
  const name = 'BISLPSP99999TEST';
  const o = 0x80;
  card[o] = 0x51; new DataView(card.buffer).setUint32(o + 4, 0x2000, true);
  card[o + 8] = 0xFF; card[o + 9] = 0xFF;
  for (let i = 0; i < name.length; i++) card[o + 0x0A + i] = name.charCodeAt(i);
  let x = 0; for (let i = 0; i < 0x7F; i++) x ^= card[o + i]; card[o + 0x7F] = x;
  card.set(buildFile(), 0x2000);
  return card;
}

if ((process.argv[1] || '').endsWith('build_demo.mjs')) {
  fs.writeFileSync(new URL('./demo.mcd', import.meta.url), buildCard());
  console.log('wrote tests/demo.mcd, code bytes', code.length);
}
