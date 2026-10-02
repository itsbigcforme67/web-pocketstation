import { load, boot } from './boot_game.mjs';
const t0 = Date.now();
const { ps } = boot(load(process.argv[2]), 0);
console.log('boot wall', Date.now() - t0, 'ms, emu t', ps.time.toFixed(1));
const ex = ps.cpu.exception.bind(ps.cpu);
ps.cpu.exception = (v, m, lr, f) => { if (v !== 0x18 && v !== 0x1C && v !== 0x08) console.log('EXC', v, (lr>>>0).toString(16)); ex(v, m, lr, f); };
const map = { F: 1, R: 2, L: 4, D: 8, U: 16 };
for (const k of (process.argv[3] || 'w').split(' ')) {
  if (k[0] === 'H') { ps.setButtons(map[k[1]]); ps.runFor(+k.slice(2) || 3); ps.setButtons(0); }
  else if (k !== 'w') { ps.setButtons(map[k]); ps.runFor(0.12); ps.setButtons(0); }
  const acc = new Array(32).fill(0).map(() => new Array(32).fill(0));
  for (let i = 0; i < 8; i++) { ps.runFor(0.125); const f = ps.getFrame(); for (let y=0;y<32;y++) for (let x=0;x<32;x++) acc[y][x] += f[y*32+x]; }
  console.log('== after', k, 't', ps.time.toFixed(1), 'pc', (ps.cpu.pc>>>0).toString(16));
  for (let y=0;y<32;y++) { const s = acc[y].map(v => v===0?'.':v===8?'#':'+').join(''); if (!process.env.Q) console.log(s); }
}
