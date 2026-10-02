import fs from 'fs';
import { PocketStation, BTN } from '../web/core/pocketstation.js';
import { formatCard } from '../web/core/memcard.js';
import { disasmARM, disasmThumb } from '../web/core/disasm.js';
const bios = fs.readFileSync(new URL('./J110.bin', import.meta.url));
const flashFile = process.env.FLASH;
const flash = flashFile ? new Uint8Array(fs.readFileSync(flashFile)) : formatCard();
export const ps = new PocketStation({ bios: new Uint8Array(bios), flash, date: new Date(2026, 9, 1, 13, 5, 0) });
ps.onTTY = (c) => process.stdout.write(String.fromCharCode(c));
export function show() {
  const f = ps.getFrame();
  let s = '';
  for (let y = 0; y < 32; y++) { for (let x = 0; x < 32; x++) s += f[y*32+x] ? '██' : '  '; s += '|\n'; }
  console.log('+' + '-'.repeat(64) + '+\n' + s);
  const c = ps.cpu;
  console.log(`pc=${c.pc.toString(16)} t=${c.t} mode=${c.mode.toString(16)} clk=${ps.clkMode} lcd=${ps.lcdMode.toString(16)} mask=${ps.intMask.toString(16)} sleep=${ps.sleeping} t0=${JSON.stringify(ps.timers[0])} time=${ps.time.toFixed(2)}`);
}
export function trace(n) {
  const c = ps.cpu;
  for (let i = 0; i < n; i++) {
    const pc = c.pc;
    const d = c.t ? disasmThumb(ps.read16(pc), pc) : disasmARM(ps.read32(pc), pc);
    console.log(pc.toString(16), d);
    c.step(); ps.tick(1);
  }
}
if ((process.argv[1]||'').endsWith('run_bios.mjs')) {
  const secs = +(process.argv[2] || 2);
  const t0 = Date.now();
  for (let i = 0; i < secs * 30; i++) ps.runFor(1/30);
  console.log('wall ms', Date.now() - t0);
  show();
  if (process.argv[3]) trace(+process.argv[3]);
}
