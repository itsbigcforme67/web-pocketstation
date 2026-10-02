// Web PocketStation - Copyright (C) 2026 the Web PocketStation authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// PlayStation memory card image (128 KiB raw .mcd/.mcr) parsing + helpers.
export const CARD_SIZE = 0x20000;
export const BLOCK = 0x2000;
export const FRAME = 0x80;

function xorChecksum(buf, off) {
  let x = 0;
  for (let i = 0; i < 0x7F; i++) x ^= buf[off + i];
  return x;
}

export function formatCard() {
  const c = new Uint8Array(CARD_SIZE);
  c[0] = 0x4D; c[1] = 0x43; c[0x7F] = xorChecksum(c, 0);
  for (let i = 1; i < 16; i++) {
    const o = i * FRAME;
    c[o] = 0xA0; c[o + 8] = 0xFF; c[o + 9] = 0xFF;
    c[o + 0x7F] = xorChecksum(c, o);
  }
  for (let i = 16; i < 36; i++) {
    const o = i * FRAME;
    c.fill(0xFF, o, o + 4);
    c[o + 8] = 0xFF; c[o + 9] = 0xFF;
    c[o + 0x7F] = xorChecksum(c, o);
  }
  c.fill(0xFF, 36 * FRAME, 63 * FRAME);
  c.set(c.subarray(0, FRAME), 63 * FRAME);
  return c;
}

// Accepts raw images and a few common container formats; returns a 128 KiB Uint8Array or null.
export function normalizeCard(buf) {
  buf = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  const ascii = (o, n) => String.fromCharCode(...buf.subarray(o, o + n));
  if (buf.length === CARD_SIZE && ascii(0, 2) === 'MC') return buf;
  if (buf.length === CARD_SIZE + 3904 && ascii(0, 11) === '123-456-STD') return buf.slice(3904); // .gme
  if (buf.length === CARD_SIZE + 64 && ascii(0, 2) === 'VgsM'.slice(0, 2)) return buf.slice(64);   // .mem/.vgs
  if (buf.length === CARD_SIZE + 0xF40 && ascii(0, 11) === '123-456-STD') return buf.slice(0xF40);
  if (buf.length === CARD_SIZE) return buf; // unformatted but right size
  return null;
}

let sjis = null;
function decodeTitle(bytes) {
  let end = 0;
  while (end < bytes.length && bytes[end] !== 0) end++;
  bytes = bytes.subarray(0, end);
  try {
    if (!sjis) sjis = new TextDecoder('shift_jis');
    return sjis.decode(bytes).normalize('NFKC').replace(/\s+/g, ' ').trim();
  } catch (e) {
    return String.fromCharCode(...bytes.filter((b) => b >= 0x20 && b < 0x7F)).trim();
  }
}

function rgb555(v) {
  const r = (v & 31) << 3, g = ((v >> 5) & 31) << 3, b = ((v >> 10) & 31) << 3;
  const a = (v === 0) ? 0 : 255;
  return [r | (r >> 5), g | (g >> 5), b | (b >> 5), a];
}

// Decodes the first-frame 16x16 icon of a save into RGBA (Uint8ClampedArray 16*16*4)
export function decodeIcon(card, firstBlock, frame = 0) {
  const base = firstBlock * BLOCK;
  const pal = [];
  for (let i = 0; i < 16; i++) pal.push(rgb555(card[base + 0x60 + i * 2] | (card[base + 0x61 + i * 2] << 8)));
  const out = new Uint8ClampedArray(16 * 16 * 4);
  const io = base + FRAME * (1 + frame);
  for (let i = 0; i < 128; i++) {
    const b = card[io + i];
    const p0 = pal[b & 15], p1 = pal[b >> 4];
    out.set(p0, (i * 2) * 4); out.set(p1, (i * 2 + 1) * 4);
  }
  return out;
}

// Parse the directory. Returns { valid, saves: [...] , freeBlocks }
export function parseCard(card) {
  const saves = [];
  const valid = card[0] === 0x4D && card[1] === 0x43;
  let freeBlocks = 0;
  for (let i = 1; i < 16; i++) {
    const o = i * FRAME;
    const state = card[o];
    if ((state & 0xF0) === 0xA0) freeBlocks++;
    if (state !== 0x51) continue;
    const size = card[o + 4] | (card[o + 5] << 8) | (card[o + 6] << 16) | (card[o + 7] << 24);
    const fnBytes = card.subarray(o + 0x0A, o + 0x0A + 20);
    let filename = '';
    for (const b of fnBytes) { if (!b) break; filename += String.fromCharCode(b); }
    const blocks = [i];
    let next = card[o + 8] | (card[o + 9] << 8);
    let guard = 0;
    while (next !== 0xFFFF && next < 15 && guard++ < 15) {
      blocks.push(next + 1);
      const no = (next + 1) * FRAME;
      next = card[no + 8] | (card[no + 9] << 8);
    }
    const b0 = i * BLOCK;
    const hasHeader = card[b0] === 0x53 && card[b0 + 1] === 0x43; // "SC"
    const title = hasHeader ? decodeTitle(card.subarray(b0 + 4, b0 + 0x44)) : filename;
    const iconFlag = card[b0 + 2];
    const iconFrames = iconFlag === 0x11 ? 1 : iconFlag === 0x12 ? 2 : iconFlag === 0x13 ? 3 : 1;
    const magic = String.fromCharCode(card[b0 + 0x52], card[b0 + 0x53], card[b0 + 0x54], card[b0 + 0x55]);
    const pocket = hasHeader && (magic === 'MCX0' || magic === 'MCX1');
    const entry = (card[b0 + 0x5C] | (card[b0 + 0x5D] << 8) | (card[b0 + 0x5E] << 16) | (card[b0 + 0x5F] << 24)) >>> 0;
    saves.push({
      dirIndex: i, filename, title, size, blocks, iconFrames,
      region: filename.slice(0, 2), productCode: filename.slice(2, 12),
      pocketstation: pocket, pocketMagic: pocket ? magic : null, entrypoint: pocket ? entry : null,
    });
  }
  return { valid, saves, freeBlocks };
}

export async function sha1Hex(bytes) {
  const h = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(h), (b) => b.toString(16).padStart(2, '0')).join('');
}

// Wrap a standalone save file (starts with "SC" title frame, multiple of 8 KiB) into a
// freshly formatted card at block 1.
export function buildCardFromFile(file, filename) {
  const blocks = Math.max(1, Math.ceil(file.length / BLOCK));
  if (blocks > 15) throw new Error('file too large');
  const card = formatCard();
  for (let b = 0; b < blocks; b++) {
    const o = (1 + b) * FRAME;
    card[o] = b === 0 ? 0x51 : (b === blocks - 1 ? 0x53 : 0x52);
    if (blocks === 1) card[o] = 0x51;
    if (b === 0) new DataView(card.buffer).setUint32(o + 4, blocks * BLOCK, true);
    const next = b === blocks - 1 ? 0xFFFF : b + 1;
    card[o + 8] = next & 0xFF; card[o + 9] = next >> 8;
    if (b === 0) for (let i = 0; i < Math.min(20, filename.length); i++) card[o + 0x0A + i] = filename.charCodeAt(i);
    card[o + 0x7F] = xorChecksum(card, o);
  }
  card.set(file.subarray(0, blocks * BLOCK), BLOCK);
  return card;
}
