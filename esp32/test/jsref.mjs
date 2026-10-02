import fs from 'fs';
import { PocketStation } from '../../web/core/pocketstation.js';
import { normalizeCard } from '../../web/core/memcard.js';
const [,, biosP, cardP, dir, script] = process.argv;
const ps = new PocketStation({ bios: new Uint8Array(fs.readFileSync(biosP)), flash: normalizeCard(new Uint8Array(fs.readFileSync(cardP))) });
ps.bootFile(+dir, new Date(2026, 9, 1, 13, 30, 0));
for (const tok of (script || 'w1000 p').split(' ')) {
  if (tok[0] === 'w') ps.runFor(+tok.slice(1) / 1000);
  else if (tok[0] === 'b') ps.setButtons(+tok.slice(1));
  else if (tok[0] === 'p') {
    const rows = []; const f = ps.getFrame();
    for (let y = 0; y < 32; y++) { let w = 0; for (let x = 0; x < 32; x++) if (f[y*32+x]) w |= 1 << x; rows.push((w >>> 0).toString(16).padStart(8, '0')); }
    console.log('F t=' + Math.floor(ps.time * 1000 + 1e-6), rows.join(' '));
  }
}
