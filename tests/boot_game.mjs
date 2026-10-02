import fs from 'fs';
import { PocketStation } from '../web/core/pocketstation.js';
import { normalizeCard, parseCard } from '../web/core/memcard.js';
const bios = new Uint8Array(fs.readFileSync(new URL('./J110.bin', import.meta.url)));
export function load(file) { return normalizeCard(new Uint8Array(fs.readFileSync(file))); }
export function boot(card, dir) {
  const ps = new PocketStation({ bios, flash: card });
  ps.onTTY = (ch) => process.stdout.write(String.fromCharCode(ch));
  const ok = ps.bootFile(dir, new Date(2026, 9, 1, 13, 30, 0));
  return { ps, ok };
}
export const rows = (ps) => { const f = ps.getFrame(); const o=[]; for (let y=0;y<32;y++){let s='';for(let x=0;x<32;x++) s+=f[y*32+x]?'#':'.'; o.push(s);} return o; };
export function film(ps, script) {
  const map = { F: 1, R: 2, L: 4, D: 8, U: 16 };
  let frames = [];
  for (const tok of script.split(/\s+/).filter(Boolean)) {
    if (tok[0] === 'w') ps.runFor(+tok.slice(1));
    else if (tok[0] === 's') { const [n, dt] = tok.slice(1).split('x'); for (let i=0;i<+n;i++){ ps.runFor(+dt); frames.push(['s '+ps.time.toFixed(1), rows(ps)]); } }
    else if (tok[0] === 'H') { ps.setButtons(tok.slice(1).split('').reduce((m,c)=>m|map[c],0)); ps.runFor(1.5); ps.setButtons(0); ps.runFor(0.2); }
    else { ps.setButtons(tok.split('').reduce((m,c)=>m|map[c],0)); ps.runFor(0.12); ps.setButtons(0); ps.runFor(0.15); frames.push([tok+' '+ps.time.toFixed(1), rows(ps)]); }
  }
  for (let i = 0; i < frames.length; i += 5) { const g = frames.slice(i, i+5); console.log(g.map(f=>f[0].padEnd(32)).join('  ')); for (let y=0;y<32;y++) console.log(g.map(f=>f[1][y]).join('  ')); }
}
if ((process.argv[1]||'').endsWith('boot_game.mjs')) {
  const card = load(process.argv[2]);
  const { ps, ok } = boot(card, +(process.argv[3] || 1));
  console.log('boot ok', ok, 'pc', ps.cpu.pc.toString(16));
  const t0 = Date.now();
  film(ps, process.argv[4] || 'w1 s5x1');
  console.log('wall', Date.now()-t0, 'ms; emu time', ps.time.toFixed(1), 'pc', ps.cpu.pc.toString(16), 'clk', ps.clkMode, 'sleep', ps.sleeping);
}
