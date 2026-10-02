import fs from 'fs';
import { load, boot, rows } from './boot_game.mjs';
import { buildCardFromFile } from '../web/core/memcard.js';
const dir = '../games/';
for (const f of fs.readdirSync(dir)) {
  let card;
  if (f.endsWith('.bin')) card = buildCardFromFile(new Uint8Array(fs.readFileSync(dir + f)), 'BISLPSP00000TETRIS');
  else card = load(dir + f);
  const { ps, ok } = boot(card, 1);
  const t0 = Date.now();
  const frames = [];
  let sound = 0; ps.sampleRate = 8000; ps.audioOut = (b) => { for (const v of b) if (v) { sound++; break; } };
  for (const [t, btn] of [[3, 0], [1, 1], [3, 1], [3, 2], [3, 0]]) {
    ps.setButtons(btn); ps.runFor(0.15); ps.setButtons(0); ps.runFor(t); frames.push(rows(ps));
  }
  console.log(`== ${f.slice(0, 40)} ok=${ok} wall=${Date.now()-t0}ms emu=${ps.time.toFixed(1)}s pc=${ps.cpu.pc.toString(16)} clk=${ps.clkMode} lcd=${ps.lcdMode.toString(16)} sleep=${ps.sleeping} soundBufs=${sound}`);
  for (let y = 0; y < 32; y += 2) console.log(frames.map(fr => { let s=''; for (let x=0;x<32;x++){ const a=fr[y][x]==='#', b=fr[y+1][x]==='#'; s += a&&b?'█':a?'▀':b?'▄':' '; } return s; }).join(' │ '));
}
