// Web PocketStation - Copyright (C) 2026 the Web PocketStation authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// Web PocketStation - phone UI: library (synced from the PC companion), player, local storage.
import { PocketStation, STATE_VERSION } from './core/pocketstation.js';
import { normalizeCard, parseCard, decodeIcon, buildCardFromFile, CARD_SIZE, BLOCK } from './core/memcard.js';

const $ = (s) => document.querySelector(s);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ------------------------------------------------------------------ storage (IndexedDB)
const db = (() => {
  let p;
  const open = () => p || (p = new Promise((res, rej) => {
    const r = indexedDB.open('web-pocketstation', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  }));
  const tx = async (mode, fn) => {
    const d = await open();
    return new Promise((res, rej) => {
      const t = d.transaction('kv', mode);
      const req = fn(t.objectStore('kv'));
      t.oncomplete = () => res(req && req.result);
      t.onerror = () => rej(t.error);
    });
  };
  return {
    get: (k) => tx('readonly', (s) => s.get(k)),
    set: (k, v) => tx('readwrite', (s) => s.put(v, k)),
    del: (k) => tx('readwrite', (s) => s.delete(k)),
    keys: () => tx('readonly', (s) => s.getAllKeys()),
  };
})();

const prefs = {
  get(k, d) { try { const v = localStorage.getItem('wps.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('wps.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

// ------------------------------------------------------------------ server API
const api = {
  base: '',
  online: false,
  standalone: false, // served as a plain website (no PocketSync behind it): everything lives on this device
  token() {
    const q = new URLSearchParams(location.search).get('token');
    if (q) prefs.set('token', q);
    return prefs.get('token', '');
  },
  headers(extra = {}) { const t = this.token(); return t ? { ...extra, 'X-Token': t } : extra; },
  async json(path, opts = {}) {
    const r = await fetch(this.base + path, { ...opts, headers: this.headers(opts.headers) });
    if (!r.ok) { const e = new Error(`${r.status}`); e.status = r.status; e.body = await r.text().catch(() => ''); throw e; }
    return r.json();
  },
  async bytes(path) {
    const r = await fetch(this.base + path, { headers: this.headers() });
    if (!r.ok) { const e = new Error(`${r.status}`); e.status = r.status; throw e; }
    return new Uint8Array(await r.arrayBuffer());
  },
};

async function sha1(bytes) {
  const h = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(h), (b) => b.toString(16).padStart(2, '0')).join('');
}

// ------------------------------------------------------------------ UI helpers
function toast(msg, ms = 2600) {
  const t = $('#toast');
  t.textContent = msg; t.classList.remove('hidden');
  clearTimeout(toast.timer); toast.timer = setTimeout(() => t.classList.add('hidden'), ms);
}
function sheet(title, bodyHTML, actions) {
  return new Promise((resolve) => {
    $('#sheet-title').textContent = title;
    $('#sheet-body').innerHTML = bodyHTML || '';
    const box = $('#sheet-actions');
    box.innerHTML = '';
    for (const a of actions) {
      const b = document.createElement('button');
      b.className = 'btn ' + (a.cls || '');
      b.textContent = a.label;
      b.onclick = () => { close(); resolve(a.value); };
      box.appendChild(b);
    }
    const s = $('#sheet');
    const close = () => { s.classList.add('hidden'); s.onclick = null; };
    s.onclick = (e) => { if (e.target === s) { close(); resolve(null); } };
    s.classList.remove('hidden');
  });
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function showView(id) {
  document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.id === id));
}
function iconCanvas(card, block, size = 16) {
  const c = document.createElement('canvas');
  c.width = 16; c.height = 16;
  const ctx = c.getContext('2d');
  const img = new ImageData(decodeIcon(card, block, 0), 16, 16);
  ctx.putImageData(img, 0, 0);
  c.style.width = c.style.height = size + 'px';
  return c;
}
function shortTitle(save) {
  // Many titles are "Series/Game" - show the most specific part first
  const t = save.title || save.filename;
  const m = t.match(/[「『](.+?)[」』]/);
  return m ? m[1] : t;
}

// ------------------------------------------------------------------ library state
// A "card" record in IndexedDB (key card:<id>):
// { id, name, source: 'pc'|'local', data: Uint8Array(128K), baseSha1, version, dirty, conflict, updated }
let cards = [];

async function loadLocalCards() {
  const keys = (await db.keys()).filter((k) => String(k).startsWith('card:'));
  const out = [];
  for (const k of keys) out.push(await db.get(k));
  return out.filter(Boolean);
}
async function saveCard(c) { c.updated = Date.now(); await db.set('card:' + c.id, c); }
async function dropStates(cardId) {
  for (const k of await db.keys()) if (String(k).startsWith(`state:${cardId}:`)) await db.del(k);
}

async function refreshFromPC() {
  let list;
  try {
    list = await api.json('/api/cards');
    api.online = true; api.standalone = false;
    prefs.set('standalone', false);
    document.documentElement.classList.remove('standalone');
  } catch (e) {
    api.online = false;
    // a plain web host answers 404 for the companion's API; remember that for when we're offline later
    if (e.status === 404) prefs.set('standalone', true);
    api.standalone = e.status === 404 || (!e.status && prefs.get('standalone', false));
    document.documentElement.classList.toggle('standalone', api.standalone);
    if (api.standalone) {
      $('#sync-status').textContent = 'Your cards and progress stay on this device';
    } else if (e.status === 401) {
      $('#sync-status').textContent = 'PC needs an access token (⋯ → Connection)';
    } else {
      $('#sync-status').textContent = 'Offline: showing cards saved on this phone';
    }
    return;
  }
  let changed = 0;
  for (const info of list.cards) {
    let c = await db.get('card:' + info.id);
    if (c && c.baseSha1 === info.sha1) {
      if (c.name !== info.name) { c.name = info.name; await saveCard(c); }
      continue;
    }
    if (c && c.dirty) {
      if (!c.conflict || c.conflict !== info.sha1) { c.conflict = info.sha1; await saveCard(c); }
      continue;
    }
    const raw = await api.bytes(`/api/cards/${encodeURIComponent(info.id)}/data`);
    const data = normalizeCard(raw);
    if (!data) continue;
    c = { id: info.id, name: info.name, source: 'pc', data, baseSha1: info.sha1, version: ((c && c.version) || 0) + 1, dirty: false, conflict: null, mtime: info.mtime };
    await dropStates(c.id);
    await saveCard(c);
    changed++;
  }
  const n = list.cards.length;
  $('#sync-status').textContent = `Synced with ${list.host || 'your PC'}: ${n} card${n === 1 ? '' : 's'}` + (changed ? `, ${changed} updated` : '');
}

async function renderLibrary() {
  cards = await loadLocalCards();
  cards.sort((a, b) => (a.source === b.source ? a.name.localeCompare(b.name) : a.source === 'pc' ? -1 : 1));
  const grid = $('#game-grid');
  const list = $('#card-list');
  grid.innerHTML = ''; list.innerHTML = '';
  let games = 0;
  for (const c of cards) {
    const parsed = parseCard(c.data);
    for (const s of parsed.saves.filter((x) => x.pocketstation)) {
      games++;
      const b = document.createElement('button');
      b.className = 'game';
      const thumb = document.createElement('div');
      thumb.className = 'thumb';
      thumb.appendChild(iconCanvas(c.data, s.blocks[0], 64));
      b.appendChild(thumb);
      b.insertAdjacentHTML('beforeend', `<div class="name">${esc(shortTitle(s))}</div><div class="meta">${esc(c.name)}</div>`);
      if (c.dirty && c.source === 'pc') b.insertAdjacentHTML('beforeend', '<span class="badge">unsynced</span>');
      b.onclick = () => play(c.id, s.dirIndex);
      grid.appendChild(b);
    }
    // memory card panel
    const d = document.createElement('details');
    d.className = 'mc';
    const used = 15 - parsed.freeBlocks;
    d.innerHTML = `<summary><span class="mc-icon"></span><div style="flex:1;min-width:0"><div class="mc-name">${esc(c.name)}</div>
      <div class="mc-sub">${c.source === 'pc' ? 'From PC' : 'On this phone'} · ${parsed.saves.length} save${parsed.saves.length === 1 ? '' : 's'} · ${used}/15 blocks${c.dirty ? ' · <span style="color:#ffc864">changed on phone</span>' : ''}${c.conflict ? ' · <span style="color:#ff6a6a">conflict</span>' : ''}</div></div></summary>`;
    const body = document.createElement('div');
    body.className = 'mc-body';
    for (const s of parsed.saves) {
      const row = document.createElement('div');
      row.className = 'save-row';
      row.appendChild(iconCanvas(c.data, s.blocks[0], 32));
      row.insertAdjacentHTML('beforeend', `<div class="t"><div>${esc(s.title)}</div><div>${esc(s.filename)} · ${s.blocks.length} block${s.blocks.length === 1 ? '' : 's'}</div></div>`);
      if (s.pocketstation) row.insertAdjacentHTML('beforeend', '<span class="pill">Pocket</span>');
      body.appendChild(row);
    }
    if (!parsed.saves.length) body.insertAdjacentHTML('beforeend', '<p class="muted">Empty card.</p>');
    const acts = document.createElement('div');
    acts.className = 'mc-actions';
    const mk = (label, fn, cls = '') => { const b = document.createElement('button'); b.className = 'btn small ' + cls; b.textContent = label; b.onclick = fn; acts.appendChild(b); };
    mk('PocketStation menu', () => play(c.id, 0));
    if (c.source === 'pc' && c.dirty) mk('Send changes to PC', () => syncCard(c.id));
    if (c.conflict) mk('Resolve conflict', () => resolveConflict(c.id));
    mk('Download', () => downloadCard(c));
    mk(c.source === 'pc' ? 'Reset to PC copy' : 'Delete', () => removeCard(c), 'danger');
    body.appendChild(acts);
    d.appendChild(body);
    list.appendChild(d);
  }
  $('#no-games').classList.toggle('hidden', games > 0);
  if (!cards.length) list.innerHTML = '<p class="empty">No memory cards yet.</p>';
}

function downloadCard(c) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([c.data], { type: 'application/octet-stream' }));
  a.download = c.name.replace(/[^\w\- ().[\]]+/g, '_') + (/\.(mcd|mcr)$/i.test(c.name) ? '' : '.mcd');
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

async function removeCard(c) {
  const isPC = c.source === 'pc';
  const ok = await sheet(isPC ? 'Reset to PC copy?' : 'Delete card?',
    isPC ? '<p>Discards anything changed on this phone for this card and reloads it from your PC.</p>' : '<p>Removes this card and its game progress from this phone.</p>',
    [{ label: isPC ? 'Reset' : 'Delete', value: true, cls: 'danger' }, { label: 'Cancel', value: false }]);
  if (!ok) return;
  await db.del('card:' + c.id);
  await dropStates(c.id);
  if (isPC) await refreshFromPC();
  renderLibrary();
}

async function syncCard(id, force = false) {
  const c = await db.get('card:' + id);
  if (!c) return false;
  try {
    const r = await fetch(`/api/cards/${encodeURIComponent(id)}/data${force ? '?force=1' : ''}`, {
      method: 'PUT', body: c.data,
      headers: api.headers({ 'Content-Type': 'application/octet-stream', 'X-Base-Sha1': c.baseSha1 || '' }),
    });
    if (r.status === 409) { c.conflict = (await r.json()).sha1; await saveCard(c); await resolveConflict(id); return false; }
    if (!r.ok) throw new Error(await r.text());
    const j = await r.json();
    c.baseSha1 = j.sha1; c.dirty = false; c.conflict = null;
    await saveCard(c);
    toast('Sent to your PC (a backup of the old card was kept)');
    renderLibrary();
    updateUnsynced();
    return true;
  } catch (e) {
    toast('Could not reach your PC: ' + e.message);
    return false;
  }
}

async function resolveConflict(id) {
  const choice = await sheet('Card changed in two places',
    '<p>This card was changed on your PC (DuckStation) <em>and</em> on this phone since the last sync.</p><p>Keep the phone version (it overwrites the PC card; a backup is kept on the PC) or take the PC version (discards phone changes for this card).</p><p>Tip: close the game in DuckStation before sending, or DuckStation may overwrite the card again.</p>',
    [{ label: 'Keep phone version → send to PC', value: 'phone', cls: 'primary' }, { label: 'Use PC version', value: 'pc', cls: 'danger' }, { label: 'Decide later', value: null }]);
  if (choice === 'phone') await syncCard(id, true);
  if (choice === 'pc') {
    const c = await db.get('card:' + id);
    c.dirty = false; c.conflict = null; c.baseSha1 = '';
    await saveCard(c);
    await refreshFromPC();
    renderLibrary();
  }
}

// ------------------------------------------------------------------ importing
async function importFiles(files) {
  let n = 0;
  for (const f of files) {
    const buf = new Uint8Array(await f.arrayBuffer());
    let data = normalizeCard(buf);
    const base = f.name.replace(/\.[^.]+$/, '');
    if (!data || data[0] !== 0x4D) {
      // single save file formats
      try {
        if (buf[0] === 0x53 && buf[1] === 0x43 && buf.length % BLOCK === 0) {          // raw "SC" file
          data = buildCardFromFile(buf, 'BISLPSP00000' + base.replace(/[^A-Z0-9]/gi, '').slice(0, 8).toUpperCase());
        } else if (buf[0] === 0x51 && (buf.length - 128) % BLOCK === 0) {              // .mcs: dir frame + data
          let name = ''; for (let i = 0x0A; i < 0x1E && buf[i]; i++) name += String.fromCharCode(buf[i]);
          data = buildCardFromFile(buf.subarray(128), name || 'BISLPSP00000IMPORT');
        } else if (buf.length >= 54 + BLOCK && buf[54] === 0x53 && buf[55] === 0x43) {  // .psx (Action Replay)
          let name = ''; for (let i = 0; i < 20 && buf[i]; i++) name += String.fromCharCode(buf[i]);
          data = buildCardFromFile(buf.subarray(54), name);
        }
      } catch (e) { data = null; }
    }
    if (!data || data.length !== CARD_SIZE) { toast(`Couldn't read ${f.name}`); continue; }
    await saveCard({ id: 'local-' + Date.now() + '-' + n, name: base, source: 'local', data, baseSha1: '', version: 1, dirty: false });
    n++;
  }
  if (n) toast(`Imported ${n} file${n === 1 ? '' : 's'}`);
  renderLibrary();
}

// ------------------------------------------------------------------ BIOS
async function getBios() {
  let b = await db.get('bios');
  if (b) return b;
  try {
    b = await api.bytes('/api/bios');
    if (b.length === 0x4000) { await db.set('bios', b); return b; }
  } catch { /* none on PC */ }
  return null;
}
async function setBiosFromFile(file) {
  const b = new Uint8Array(await file.arrayBuffer());
  if (b.length !== 0x4000) { toast('That isn\'t a PocketStation BIOS (expected exactly 16 KB)'); return; }
  const ver = String.fromCharCode(...b.subarray(0x3FFC, 0x4000));
  await db.set('bios', b);
  toast(`BIOS loaded (${/^J\d{3}$/.test(ver) ? ver : 'unknown version'})`);
  checkBios();
}
async function checkBios() {
  $('#bios-banner').classList.toggle('hidden', !!(await getBios()));
}

// ------------------------------------------------------------------ player
const player = {
  ps: null, cardId: null, dir: 0, running: false, raf: 0, last: 0,
  lcdLevels: new Float32Array(1024), frame: new Uint8Array(1024),
  audio: null, muted: prefs.get('muted', false), dirtyTimer: 0, saveTimer: 0,
};

async function play(cardId, dir) {
  const bios = await getBios();
  if (!bios) { toast('Load your PocketStation BIOS first'); $('#bios-input').click(); return; }
  const c = await db.get('card:' + cardId);
  if (!c) return;
  const parsed = parseCard(c.data);
  const save = parsed.saves.find((s) => s.dirIndex === dir);
  $('#game-title').textContent = save ? shortTitle(save) : 'PocketStation';
  $('#game-sub').textContent = c.name;
  const ps = new PocketStation({ bios, flash: c.data, sampleRate: 48000 });
  const st = await db.get(`state:${cardId}:${dir}`);
  if (st && st.version === c.version && st.state && st.state.v === STATE_VERSION) {
    try { ps.loadState(st.state, new Date()); } catch (e) { console.warn('state load failed', e); ps.bootFile(dir); }
  } else if (!ps.bootFile(dir)) {
    toast('Unrecognised BIOS: starting from the BIOS menu');
    ps.hardReset();
  }
  ps.onFlashWrite = () => markDirty();
  Object.assign(player, { ps, cardId, dir, card: c });
  player.lcdLevels.fill(0);
  showView('player');
  setupAudio();
  updateUnsynced();
  start();
  if (navigator.wakeLock) navigator.wakeLock.request('screen').then((l) => { player.wake = l; }).catch(() => {});
}

function start() {
  if (player.running) return;
  player.running = true;
  player.last = performance.now();
  const loop = (now) => {
    if (!player.running) return;
    const dt = Math.min(0.1, Math.max(0, (now - player.last) / 1000));
    player.last = now;
    try { player.ps.runFor(dt); } catch (e) { console.error(e); stop(); toast('Emulator error: ' + e.message); return; }
    draw();
    player.raf = requestAnimationFrame(loop);
  };
  player.raf = requestAnimationFrame(loop);
  clearInterval(player.saveTimer);
  player.saveTimer = setInterval(() => persist(), 15000);
}
function stop() {
  player.running = false;
  cancelAnimationFrame(player.raf);
  clearInterval(player.saveTimer);
}

const lcd = $('#lcd');
const lctx = lcd.getContext('2d');
function draw() {
  const ps = player.ps;
  const f = ps.getFrame(player.frame);
  const L = player.lcdLevels;
  const W = lcd.width, cell = W / 32, gap = Math.max(1, Math.round(cell * 0.08));
  lctx.fillStyle = '#a8b39a';
  lctx.fillRect(0, 0, W, W);
  // slow LCD response: blend toward the new frame
  for (let i = 0; i < 1024; i++) {
    const t = f[i];
    L[i] += (t - L[i]) * (t > L[i] ? 0.75 : 0.5);
  }
  for (let y = 0; y < 32; y++) {
    for (let x = 0; x < 32; x++) {
      const v = L[y * 32 + x];
      if (v < 0.04) {
        lctx.fillStyle = 'rgba(29,36,24,0.05)';
      } else {
        lctx.fillStyle = `rgba(29,36,24,${(0.08 + v * 0.9).toFixed(3)})`;
      }
      lctx.fillRect(x * cell, y * cell, cell - gap, cell - gap);
    }
  }
  $('#led').classList.toggle('on', ps.ledOn);
}

async function persist() {
  if (!player.ps) return;
  const c = player.card;
  try {
    await db.set(`state:${player.cardId}:${player.dir}`, { version: c.version, state: player.ps.saveState(), saved: Date.now() });
    if (c.dirty) await saveCard(c);
  } catch (e) { console.warn('persist failed', e); }
}

function markDirty() {
  const c = player.card;
  if (!c) return;
  // flash array is shared with the emulator; copy into the record lazily
  c.data = player.ps.flash;
  if (!c.dirty) { c.dirty = true; updateUnsynced(); }
  clearTimeout(player.dirtyTimer);
  player.dirtyTimer = setTimeout(() => saveCard(c), 800);
}
function updateUnsynced() {
  const c = player.card;
  $('#unsynced').classList.toggle('hidden', !(c && c.dirty && c.source === 'pc'));
}

async function leavePlayer() {
  stop();
  await persist();
  if (player.wake) { player.wake.release().catch(() => {}); player.wake = null; }
  if (player.audio) player.audio.ctx.suspend().catch(() => {});
  player.ps = null;
  showView('library');
  renderLibrary();
}

// ---- audio
function setupAudio() {
  if (!player.audio) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    const ctx = new AC();
    const ring = new Float32Array(16384);
    const a = { ctx, ring, r: 0, w: 0 };
    const node = ctx.createScriptProcessor(1024, 0, 1);
    node.onaudioprocess = (e) => {
      const out = e.outputBuffer.getChannelData(0);
      const avail = (a.w - a.r + ring.length) % ring.length;
      for (let i = 0; i < out.length; i++) {
        if (i < avail) { out[i] = ring[a.r]; a.r = (a.r + 1) % ring.length; } else out[i] = 0;
      }
      // keep latency bounded
      const left = (a.w - a.r + ring.length) % ring.length;
      if (left > 6000) a.r = (a.w - 2048 + ring.length) % ring.length;
    };
    const gain = ctx.createGain(); gain.gain.value = 0.35;
    node.connect(gain); gain.connect(ctx.destination);
    a.node = node; a.gain = gain;
    player.audio = a;
  }
  const a = player.audio;
  player.ps.sampleRate = a.ctx.sampleRate;
  player.ps.audioOut = player.muted ? null : (buf) => {
    for (let i = 0; i < buf.length; i++) { a.ring[a.w] = buf[i]; a.w = (a.w + 1) % a.ring.length; }
  };
  if (!player.muted) a.ctx.resume().catch(() => {});
  $('#btn-sound').textContent = player.muted ? '🔇' : '🔈';
}

// ---- input
let held = 0;
const pointerBtn = new Map();
function applyButtons() {
  let m = held;
  for (const b of pointerBtn.values()) m |= b;
  if (player.ps) player.ps.setButtons(m);
  document.querySelectorAll('.pbtn').forEach((el) => el.classList.toggle('pressed', (m & +el.dataset.btn) !== 0));
}
function btnAt(x, y) {
  const el = document.elementFromPoint(x, y);
  const b = el && el.closest && el.closest('.pbtn');
  return b ? +b.dataset.btn : 0;
}
function setupInput() {
  const dev = $('#device');
  const onDown = (e) => {
    const b = btnAt(e.clientX, e.clientY);
    if (!b) return;
    e.preventDefault();
    dev.setPointerCapture?.(e.pointerId);
    pointerBtn.set(e.pointerId, b);
    if (navigator.vibrate) navigator.vibrate(8);
    if (player.audio && player.audio.ctx.state !== 'running' && !player.muted) player.audio.ctx.resume();
    applyButtons();
  };
  const onMove = (e) => {
    if (!pointerBtn.has(e.pointerId)) return;
    const b = btnAt(e.clientX, e.clientY);
    // sliding a thumb across the d-pad switches direction (but never drops to nothing mid-slide)
    if (b && b !== pointerBtn.get(e.pointerId)) { pointerBtn.set(e.pointerId, b); if (navigator.vibrate) navigator.vibrate(5); applyButtons(); }
  };
  const onUp = (e) => { if (pointerBtn.delete(e.pointerId)) applyButtons(); };
  dev.addEventListener('pointerdown', onDown);
  dev.addEventListener('pointermove', onMove);
  dev.addEventListener('pointerup', onUp);
  dev.addEventListener('pointercancel', onUp);
  dev.addEventListener('contextmenu', (e) => e.preventDefault());
  const keys = { ArrowUp: 16, ArrowDown: 8, ArrowLeft: 4, ArrowRight: 2, z: 1, x: 1, ' ': 1, Enter: 1 };
  addEventListener('keydown', (e) => { const b = keys[e.key]; if (b && player.ps) { held |= b; applyButtons(); e.preventDefault(); } });
  addEventListener('keyup', (e) => { const b = keys[e.key]; if (b) { held &= ~b; applyButtons(); } });
  addEventListener('blur', () => { held = 0; pointerBtn.clear(); applyButtons(); });
}

// ------------------------------------------------------------------ menus
async function playerMenu() {
  const c = player.card;
  const acts = [
    { label: 'Restart game', value: 'restart' },
    { label: 'Open PocketStation menu (clock, files)', value: 'bios' },
    { label: 'Sync clock to this phone', value: 'clock' },
  ];
  if (c.source === 'pc' && c.dirty) acts.unshift({ label: 'Send game data to PC', value: 'sync', cls: 'primary' });
  acts.push({ label: 'Close', value: null });
  const v = await sheet('Options', '<p>The PocketStation keeps running only while this screen is open. Progress is saved on this phone automatically.</p>', acts);
  if (v === 'restart') { await db.del(`state:${player.cardId}:${player.dir}`); player.ps.bootFile(player.dir); }
  if (v === 'bios') { await persist(); stop(); play(player.cardId, 0); }
  if (v === 'clock') { player.ps.syncClock(new Date()); toast('Clock set'); }
  if (v === 'sync') { await persist(); syncCard(player.cardId); }
}

// Where this copy came from, for the links in Settings.
function siteLinks() {
  const m = location.hostname.match(/^([a-z0-9-]+)\.github\.io$/i);
  const repo = location.pathname.split('/').filter(Boolean)[0];
  return { source: m && repo ? `https://github.com/${m[1]}/${repo}` : '', home: prefs.get('home', '') };
}
// Arrived from a console-picker page on the same site? Remember it so Settings can link back.
(function rememberHome() {
  try {
    const r = document.referrer && new URL(document.referrer);
    const here = new URL('.', location.href).pathname;
    if (r && r.origin === location.origin && !r.pathname.startsWith(here)) prefs.set('home', r.origin + r.pathname);
  } catch { /* no referrer */ }
})();

async function settingsMenu() {
  const { source, home } = siteLinks();
  const about = `<p class="about">An emulator for the PocketStation that runs in your browser. It includes no BIOS and no games: load your own BIOS dump and memory cards, and they never leave this device.${
    source ? ` Free software (GPL-3.0): <a href="${esc(source)}" target="_blank" rel="noopener">source code</a>.` : ''}</p>`;
  const body = api.standalone
    ? `<p>This copy runs entirely on this device: the BIOS, memory cards and game progress are kept in this browser's storage. Clearing the browser's site data erases them, so download cards you care about (Memory cards → Download).</p>
       <p>Want cards synced with DuckStation on a PC? Run the PocketSync companion from the source code.</p>${about}`
    : `<p>${api.online ? 'Connected to the companion app on your PC.' : 'Not connected to your PC. Open this page from the address the companion app prints (e.g. <code>http://192.168.1.20:8765</code>).'}</p>
    <label class="field">Access token (only if you set one on the PC)<input id="tok" value="${esc(prefs.get('token', ''))}" autocomplete="off"></label>${about}`;
  const acts = [];
  if (!api.standalone) acts.push({ label: 'Save token & refresh', value: 'tok', cls: 'primary' });
  acts.push({ label: 'Replace BIOS file', value: 'bios' });
  if (home) acts.push({ label: '‹ All consoles', value: 'home' });
  acts.push({ label: 'Erase everything on this device', value: 'wipe', cls: 'danger' }, { label: 'Close', value: null });
  const v = await sheet('Settings', body, acts);
  if (v === 'tok') { prefs.set('token', (document.getElementById('tok')?.value || '').trim()); await boot(); }
  if (v === 'bios') $('#bios-input').click();
  if (v === 'home') location.href = home;
  if (v === 'wipe') {
    const ok = await sheet('Erase all local data?', `<p>Removes the BIOS, all cards and game progress stored on this device.${api.standalone ? '' : ' Cards on your PC are not touched.'}</p>`, [{ label: 'Erase', value: true, cls: 'danger' }, { label: 'Cancel', value: false }]);
    if (ok) { for (const k of await db.keys()) await db.del(k); location.reload(); }
  }
}

// ------------------------------------------------------------------ live updates from the PC
function listenForChanges() {
  if (!window.EventSource || !api.online) return;
  const t = api.token();
  const es = new EventSource('/api/events' + (t ? '?token=' + encodeURIComponent(t) : ''));
  es.addEventListener('cards', async () => {
    if ($('#library').classList.contains('active')) { await refreshFromPC(); renderLibrary(); }
    else toast('A memory card changed on your PC; it will sync when you go back');
  });
}

// ------------------------------------------------------------------ boot
async function boot() {
  await checkBios();
  await refreshFromPC();
  await renderLibrary();
}

$('#bios-input').addEventListener('change', (e) => { if (e.target.files[0]) setBiosFromFile(e.target.files[0]); e.target.value = ''; });
$('#import-input').addEventListener('change', (e) => { importFiles([...e.target.files]); e.target.value = ''; });
$('#btn-back').onclick = leavePlayer;
$('#btn-menu').onclick = settingsMenu;
$('#btn-pmenu').onclick = playerMenu;
$('#btn-sync-now').onclick = async () => { await persist(); syncCard(player.cardId); };
$('#btn-sound').onclick = () => { player.muted = !player.muted; prefs.set('muted', player.muted); if (player.ps) setupAudio(); };
document.addEventListener('visibilitychange', () => {
  if (!player.ps) return;
  if (document.hidden) { stop(); persist(); }
  else if ($('#player').classList.contains('active')) start();
});
addEventListener('pagehide', () => { if (player.ps) persist(); });
setupInput();
boot().then(listenForChanges);
if ('serviceWorker' in navigator && (location.protocol === 'https:' || location.hostname === 'localhost')) {
  navigator.serviceWorker.register('sw.js').catch(() => {});
}
window.__wps = { player, db };
