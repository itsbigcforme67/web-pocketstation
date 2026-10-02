import fs from 'fs';
import { disasmARM, disasmThumb } from '../web/core/disasm.js';
const b = fs.readFileSync(new URL('./J110.bin', import.meta.url));
const [,, start, count, mode] = process.argv;
let a = parseInt(start, 16);
for (let i = 0; i < +count; i++) {
  const off = a & 0x3FFF;
  if (mode === 't') { const op = b.readUInt16LE(off); console.log((0x04000000+off).toString(16), op.toString(16).padStart(4,'0'), disasmThumb(op, 0x04000000+off)); a += 2; }
  else { const op = b.readUInt32LE(off); console.log((0x04000000+off).toString(16), op.toString(16).padStart(8,'0'), disasmARM(op, 0x04000000+off)); a += 4; }
}
