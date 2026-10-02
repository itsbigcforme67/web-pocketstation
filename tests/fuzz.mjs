import { load, boot, rows } from './boot_game.mjs';
const card = load(process.argv[2]);
const valid = (pc) => { pc >>>= 0; return pc < 0x800 || (pc >= 0x02000000 && pc < 0x02020000) || (pc >= 0x04000000 && pc < 0x04004000); };
let seed = +(process.argv[3] || 1);
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
for (let run = 0; run < 6; run++) {
  const { ps } = boot(card, 0);
  const ex = ps.cpu.exception.bind(ps.cpu); ps.cpu.exception = (v, m, lr, f) => { if (v !== 0x18 && v !== 0x1C && v !== 0x08) throw new Error('exception vector ' + v.toString(16) + ' from ' + (lr>>>0).toString(16)); ex(v, m, lr, f); };
  const scr = new Set();
  const log = [];
  try {
    for (let i = 0; i < 40; i++) {
      const b = [1, 2, 4, 8, 16][Math.floor(rnd() * 5)];
      const hold = rnd() < 0.3 ? 2 : 0.15;
      log.push((hold > 1 ? 'H' : '') + 'FRLDU'[[1,2,4,8,16].indexOf(b)]);
      ps.setButtons(b); ps.runFor(hold); ps.setButtons(0); ps.runFor(0.4);
      scr.add(rows(ps).join(''));
      { const pcs = new Set(); for (let k = 0; k < 20; k++) { ps.runFor(0.1); pcs.add(ps.cpu.pc >>> 4); } if (pcs.size === 1 && !ps.sleeping) throw new Error('stuck at ' + (ps.cpu.pc>>>0).toString(16)); }
      if (!valid(ps.cpu.pc)) throw new Error('bad pc ' + (ps.cpu.pc >>> 0).toString(16) + ' mode ' + ps.cpu.mode.toString(16));
    }
    if (run < 3) console.log('run', run, 'distinct screens', scr.size);
  } catch (e) { console.log('run', run, 'FAIL', e.message, '\n  seq', log.join(' ')); process.exit(0); }
}
console.log('no failure');
