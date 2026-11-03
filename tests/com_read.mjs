// Dock the PocketStation and talk to it like a PSX: memory card read + PocketStation commands.
import fs from 'fs';
import { PocketStation } from '../web/core/pocketstation.js';
import { normalizeCard } from '../web/core/memcard.js';
const bios = new Uint8Array(fs.readFileSync(new URL('./J110.bin', import.meta.url)));
const card = process.argv[2] ? normalizeCard(new Uint8Array(fs.readFileSync(process.argv[2]))) : null;
const ps = new PocketStation({ bios, flash: card || undefined });
ps.bootFile(+(process.argv[4] ?? 0), new Date(2026, 10, 2, 13, 30));
ps.runFor(2);
ps.dock(true);
for (let i = 0; i < 6; i++) { ps.runFor(0.25); console.log('t', ps.time.toFixed(2), 'ComFlags', ps.ramDV.getUint32(0xC0, true).toString(16), 'pc', ps.cpu.pc.toString(16), 'sleep', ps.sleeping); }
const hex = (a) => a.map((v) => (v & 0x100 ? '' : '!') + (v & 0xFF).toString(16).padStart(2, '0')).join(' ');
export function xfer(bytes) {
  const out = [];
  for (const [i, b] of bytes.entries()) { const r = ps.comByte(b, i === 0); out.push(r); if (!(r & 0x100)) break; }
  ps.comEnd(); ps.runFor(0.002);
  return out;
}
const sector = +(process.argv[3] || 0);
const rd = xfer([0x81, 0x52, 0, 0, sector >> 8, sector & 255, ...Array(140).fill(0)]);
console.log('read', hex(rd.slice(0, 12)), '...', hex(rd.slice(-6)), 'len', rd.length);
console.log('id  ', hex(xfer([0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])));
console.log('5a  ', hex(xfer([0x81, 0x5a, ...Array(24).fill(0)])));
console.log('58  ', hex(xfer([0x81, 0x58, ...Array(12).fill(0)])));
const data = Array.from({ length: 128 }, (_, i) => (i * 7 + 3) & 255);
const chk = data.reduce((x, v) => x ^ v, 0x00 ^ 0x3f);
const wr = xfer([0x81, 0x57, 0, 0, 0x00, 0x3f, ...data, chk, 0, 0, 0]);
console.log('write', hex(wr.slice(0, 10)), '...', hex(wr.slice(-6)), 'len', wr.length);
console.log('read3f', hex(xfer([0x81, 0x52, 0, 0, 0, 0x3f, ...Array(140).fill(0)]).slice(8, 20)));
console.log('flag now', hex(xfer([0x81, 0x52, 0, 0]).slice(0, 2)));
