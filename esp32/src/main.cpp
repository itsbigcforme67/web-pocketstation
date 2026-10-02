// Web PocketStation - M5Stack CoreS3 firmware
// Hardware: CoreS3 + Faces Bottom3 + Gamepad3 (falls back to on-screen touch buttons).
//
// Controls (Gamepad3):  D-pad = D-pad, A or B = PocketStation button,
//                       START = menu, SELECT = mute / unmute.
#include <M5Unified.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>
#include "pscore.h"
#include "storage.h"

using ps::PocketStation;

// ------------------------------------------------------------------ pad input
enum : uint8_t { P_UP = 0x01, P_DOWN = 0x02, P_LEFT = 0x04, P_RIGHT = 0x08, P_A = 0x10, P_B = 0x20, P_SELECT = 0x40, P_START = 0x80 };
static const uint8_t GP_ADDR = 0x08, GP_REG_KEYS = 0x00;
static bool gpPresent = false;
static uint8_t gpLastRaw = 0xFF, gpState = 0;
static uint8_t pad = 0, padPrev = 0, padPressed = 0;
static uint32_t padRepeatAt = 0;

static bool gpReadRaw(uint8_t& raw) { return M5.In_I2C.readRegister(GP_ADDR, GP_REG_KEYS, &raw, 1, 400000); }
static void gpInit() {
  uint8_t raw;
  gpPresent = gpReadRaw(raw);
  // The panel reports the key byte as an event: keep the first value as a baseline and
  // only act on changes (avoids phantom presses at power-on).
  if (gpPresent) gpLastRaw = raw;
  gpState = 0;
  Serial.printf("Gamepad3 %s (first byte %02x)\n", gpPresent ? "found" : "not found - using touch controls", raw);
}
static void gpPoll() {
  uint8_t raw;
  if (!gpPresent || !gpReadRaw(raw)) return;
  if (raw != gpLastRaw) { gpLastRaw = raw; gpState = (uint8_t)~raw; }
}

// Touch fallback (and touch shortcuts in the menus)
struct Zone { int16_t x, y, w, h; uint8_t bit; };
static std::vector<Zone> touchZones;
static uint8_t touchPad() {
  uint8_t m = 0;
  int n = M5.Touch.getCount();
  for (int i = 0; i < n; i++) {
    auto t = M5.Touch.getDetail(i);
    if (!t.isPressed()) continue;
    for (auto& z : touchZones) if (t.x >= z.x && t.x < z.x + z.w && t.y >= z.y && t.y < z.y + z.h) m |= z.bit;
  }
  return m;
}
static void readInput() {
  gpPoll();
  padPrev = pad;
  pad = gpState | touchPad();
  padPressed = pad & ~padPrev;
  // key repeat for menu navigation
  if (pad & (P_UP | P_DOWN)) {
    if (padPressed & (P_UP | P_DOWN)) padRepeatAt = millis() + 380;
    else if (millis() > padRepeatAt) { padPressed |= pad & (P_UP | P_DOWN); padRepeatAt = millis() + 90; }
  }
}

// ------------------------------------------------------------------ clock
static ps::DateTime nowDT() {
  struct tm tm;
  time_t t = time(nullptr);
  if (t > 1700000000) {
    localtime_r(&t, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday};
  }
  auto d = M5.Rtc.getDateTime();
  return {d.date.year, d.date.month, d.date.date, d.time.hours, d.time.minutes, d.time.seconds, d.date.weekDay};
}
static void rtcFromSystem() {
  time_t t = time(nullptr);
  if (t < 1700000000) return;
  struct tm tm; localtime_r(&t, &tm);
  m5::rtc_datetime_t d;
  d.date.year = tm.tm_year + 1900; d.date.month = tm.tm_mon + 1; d.date.date = tm.tm_mday; d.date.weekDay = tm.tm_wday;
  d.time.hours = tm.tm_hour; d.time.minutes = tm.tm_min; d.time.seconds = tm.tm_sec;
  M5.Rtc.setDateTime(d);
}

// ------------------------------------------------------------------ graphics helpers
static M5Canvas canvas(&M5.Display);
static const uint16_t C_BG = 0x10A3, C_PANEL = 0x2124, C_HI = 0x3A8F, C_TEXT = 0xEF7D, C_MUTED = 0x9CF3, C_ACCENT = 0x6D5F, C_WARN = 0xFE48;
static uint16_t LCD_BG, LCD_INK, LCD_GAP;

static void drawIcon(LGFX_Sprite& g, const uint16_t* icon, int x, int y, int scale) {
  for (int py = 0; py < 16; py++)
    for (int px = 0; px < 16; px++) {
      uint16_t c = icon[py * 16 + px];
      if (c) g.fillRect(x + px * scale, y + py * scale, scale, scale, c);
    }
}
static void toast(const String& msg, uint32_t ms = 1800) {
  canvas.fillRoundRect(16, 92, 288, 56, 10, C_PANEL);
  canvas.drawRoundRect(16, 92, 288, 56, 10, C_HI);
  canvas.setTextColor(C_TEXT);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextWrap(false);
  String m = msg;
  if (m.length() > 40) {   // two lines
    int cut = m.lastIndexOf(' ', 40); if (cut < 10) cut = 40;
    canvas.drawString(m.substring(0, cut), 160, 110);
    canvas.drawString(m.substring(cut + 1), 160, 130);
  } else canvas.drawString(m, 160, 120);
  canvas.pushSprite(0, 0);
  if (ms) delay(ms);
}
static void status(const char* msg) { toast(msg, 0); }

// ------------------------------------------------------------------ emulator session
static PocketStation emu;
static uint8_t biosBuf[0x4000];
static uint8_t* cardBuf = nullptr;      // 128 KiB, the running card (emulated FLASH)
static uint8_t* stateBuf = nullptr;
static int16_t audioRing[8192];
static int16_t audioChunks[4][512];
static int audioChunk = 0;
static volatile bool cardDirty = false;
static volatile uint32_t lastFlashWrite = 0;
static int curGame = -1;
static uint32_t frameRows[32], shownRows[32];
static bool lcdFull = true, ledShown = false;
static uint32_t lastUs = 0, lastDraw = 0, slowUs = 0, busyUs = 0;
static bool muted = false;
static int volume = 128;

enum Mode { M_LIBRARY, M_GAME, M_PAUSE, M_SYSMENU };
static Mode mode = M_LIBRARY;
static int libSel = 0, libTop = 0, menuSel = 0;

static void onFlashWrite(void*, uint32_t) { cardDirty = true; lastFlashWrite = millis(); }

// ---- the emulator runs in its own task on CPU core 0; drawing, input and audio stay on core 1
static TaskHandle_t emuTask = nullptr;
static volatile bool emuWanted = false, emuIdle = true;
static volatile uint32_t emuButtons = 0, emuLoadPct = 0;
static uint8_t* cardShadow = nullptr;

static void emuTaskFn(void*) {
  uint32_t last = micros(), busy = 0, winStart = millis();
  for (;;) {
    if (!emuWanted) { emuIdle = true; vTaskDelay(pdMS_TO_TICKS(5)); last = micros(); continue; }
    emuIdle = false;
    emu.setButtons(emuButtons);
    uint32_t now = micros(), dt = now - last;
    last = now;
    if (dt > 100000) dt = 100000;
    if (dt) emu.runMaster((uint64_t)dt * ps::MAX_CLOCK / 1000000);
    busy += micros() - now;
    if (millis() - winStart >= 5000) {
      emuLoadPct = (uint32_t)((uint64_t)busy * 100 / ((millis() - winStart) * 1000ull));
      busy = 0; winStart = millis();
    }
    vTaskDelay(1);  // always yield (keeps the core-0 watchdog happy); time lost here is made up next round
  }
}
static void emuStart() { lastUs = micros(); emuWanted = true; }
static void emuStop() { emuWanted = false; while (!emuIdle) delay(1); }

static void saveCardIfDirty(bool force) {
  if (!cardDirty || curGame < 0) return;
  if (!force && millis() - lastFlashWrite < 3000) return;
  int ci = store::games[curGame].card;
  bool running = emuWanted;
  if (running) emuStop();
  memcpy(cardShadow, cardBuf, 0x20000);   // ~1 ms; then write the copy while the game keeps running
  cardDirty = false;
  if (running) emuStart();
  if (store::saveCard(ci, cardShadow)) {
    cardDirty = false;
    if (store::cards[ci].id.length() && !store::cards[ci].dirty) { store::cards[ci].dirty = true; store::saveIndex(); }
  }
}
static void saveState() {
  if (curGame < 0) return;
  auto& g = store::games[curGame];
  emu.saveState(stateBuf);
  File f = LittleFS.open(store::statePath(g.card, g.dir), "w");
  if (f) { f.write(stateBuf, emu.stateSize()); f.close(); }
}

// LCD geometry: 6x scale in gamepad mode (192 px), 5x with touch controls
static int lcdScale = 6, lcdX = 64, lcdY = 4;

static void drawGameChrome() {
  M5.Display.fillScreen(C_BG);
  int sz = 32 * lcdScale;
  M5.Display.fillRoundRect(lcdX - 8, lcdY - 2, sz + 16, sz + 6, 8, 0x5ACB);
  M5.Display.fillRect(lcdX, lcdY + 1, sz, sz, LCD_BG);
  M5.Display.setTextDatum(top_left);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(C_MUTED, C_BG);
  auto& g = store::games[curGame];
  String t = g.title; if (t.length() > 26) t = t.substring(0, 26);
  touchZones.clear();
  if (gpPresent) {
    M5.Display.drawString(t, 6, 222);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString("START: menu", 314, 222);
  } else {
    // on-screen controls under the LCD
    int y0 = lcdY + sz + 8;
    int cx = 52, cy = y0 + 32, b = 26;
    const char* lbl[4] = {"^", "v", "<", ">"};
    int bx[4] = {cx - b / 2, cx - b / 2, cx - b / 2 - b, cx + b / 2};
    int by[4] = {cy - b / 2 - b, cy + b / 2, cy - b / 2, cy - b / 2};
    uint8_t bits[4] = {P_UP, P_DOWN, P_LEFT, P_RIGHT};
    M5.Display.setTextDatum(middle_center);
    for (int i = 0; i < 4; i++) {
      M5.Display.fillRoundRect(bx[i], by[i], b, b, 5, 0xCE59);
      M5.Display.setTextColor(0x4208);
      M5.Display.drawString(lbl[i], bx[i] + b / 2, by[i] + b / 2);
      touchZones.push_back({(int16_t)(bx[i] - 6), (int16_t)(by[i] - 4), (int16_t)(b + 12), (int16_t)(b + 8), bits[i]});
    }
    M5.Display.fillCircle(270, cy, 24, 0xEF7D);
    touchZones.push_back({236, (int16_t)(cy - 36), 84, 72, P_A});
    M5.Display.fillRoundRect(130, cy - 12, 60, 24, 12, 0x5ACB);
    M5.Display.setTextColor(C_TEXT);
    M5.Display.drawString("MENU", 160, cy);
    touchZones.push_back({126, (int16_t)(cy - 18), 68, 36, P_START});
  }
  lcdFull = true;
}

static M5Canvas lcdStrip(&M5.Display);   // one LCD row (32*scale x scale), rendered in RAM

static void drawLcd() {
  emu.getFrame(frameRows);
  int s = lcdScale, w = 32 * s;
  if (lcdStrip.width() != w || lcdStrip.height() != s) { lcdStrip.deleteSprite(); lcdStrip.setColorDepth(16); lcdStrip.createSprite(w, s); }
  M5.Display.startWrite();
  for (int y = 0; y < 32; y++) {
    uint32_t now = frameRows[y];
    if (!lcdFull && now == shownRows[y]) continue;
    lcdStrip.fillScreen(LCD_GAP);
    for (int x = 0; x < 32; x++) lcdStrip.fillRect(x * s, 0, s - 1, s - 1, ((now >> x) & 1) ? LCD_INK : LCD_BG);
    lcdStrip.pushSprite(&M5.Display, lcdX, lcdY + 1 + y * s);
    shownRows[y] = now;
  }
  bool led = emu.ledOn();
  if (led != ledShown || lcdFull) {
    int lx = lcdX - 24, ly = lcdY + 14;
    M5.Display.fillCircle(lx, ly, 8, led ? 0xF800 : 0x3000);
    if (led) M5.Display.drawCircle(lx, ly, 9, 0xFB2C); else M5.Display.drawCircle(lx, ly, 9, C_BG);
    ledShown = led;
  }
  M5.Display.endWrite();
  lcdFull = false;
}

static void pumpAudio() {
  uint32_t size = emu.audioRingSize;
  uint32_t avail = (emu.audioW - emu.audioR + size) % size;
  while (avail >= 512) {
    if (!muted && M5.Speaker.isPlaying(0) >= 2) break;
    int16_t* out = audioChunks[audioChunk];
    uint32_t r = emu.audioR;
    for (int i = 0; i < 512; i++) { out[i] = audioRing[r]; r = (r + 1) % size; }
    emu.audioR = r;
    avail -= 512;
    if (!muted) { M5.Speaker.playRaw(out, 512, emu.sampleRate, false, 1, 0); audioChunk = (audioChunk + 1) & 3; }
  }
}

static void startGame(int gi) {
  emuStop();
  auto& g = store::games[gi];
  if (!store::loadCard(g.card, cardBuf)) { toast("Couldn't read that card"); return; }
  curGame = gi;
  cardDirty = false;
  emu.init(biosBuf, cardBuf);
  emu.idleSkip = store::config.idleSkip;
  emu.onFlashWrite = onFlashWrite;
  emu.audioRing = audioRing; emu.audioRingSize = 8192; emu.audioW = emu.audioR = 0; emu.sampleRate = 16000;
  bool resumed = false;
  File f = LittleFS.open(store::statePath(g.card, g.dir), "r");
  if (f) {
    size_t n = f.read(stateBuf, emu.stateSize() + 1);
    f.close();
    emu.hardReset(nowDT());
    resumed = emu.loadState(stateBuf, n, nowDT());
  }
  if (!resumed) {
    status(g.dir ? "Starting..." : "Starting PocketStation menu...");
    if (!emu.bootFile(g.dir, nowDT())) { toast("This BIOS isn't recognised"); curGame = -1; return; }
  }
  lcdScale = gpPresent ? 6 : 5;
  lcdX = (320 - 32 * lcdScale) / 2;
  lcdY = gpPresent ? 12 : 4;
  memset(shownRows, 0, sizeof shownRows);
  drawGameChrome();
  mode = M_GAME;
  emuStart();
  Serial.printf("Started %s (dir %d) %s\n", g.title.c_str(), g.dir, resumed ? "from save state" : "fresh");
}

static void drawLibrary();

static void quitGame() {
  emuStop();
  saveState();
  saveCardIfDirty(true);
  M5.Speaker.stop();
  int ci = curGame >= 0 ? store::games[curGame].card : -1;
  curGame = -1;
  mode = M_LIBRARY;
  touchZones.clear();
  // Changed cards go straight back to the PC when we're online.
  if (ci >= 0 && store::cards[ci].dirty && WiFi.status() == WL_CONNECTED) {
    status("Sending game data to your PC...");
    String msg; store::pushToPC(msg);
    toast(msg, 1400);
  }
  store::scan();
  drawLibrary();
}

// ------------------------------------------------------------------ library screen
static const int ROW_H = 46, LIST_Y = 34, ROWS = 4;

static void drawLibrary() {
  touchZones.clear();
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("PocketStation", 10, 16);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_right);
  String right = String(M5.Power.getBatteryLevel()) + "%";
  if (WiFi.status() == WL_CONNECTED) right = "PC sync on  " + right;
  canvas.drawString(right, 312, 16);
  auto& games = store::games;
  if (games.empty()) {
    canvas.setTextDatum(middle_center);
    canvas.drawString("No memory cards found.", 160, 100);
    canvas.drawString("Add cards to data/cards and run uploadfs,", 160, 124);
    canvas.drawString("or set up Wi-Fi sync in config.json.", 160, 144);
  }
  if (libSel >= (int)games.size()) libSel = games.size() ? games.size() - 1 : 0;
  if (libSel < libTop) libTop = libSel;
  if (libSel >= libTop + ROWS) libTop = libSel - ROWS + 1;
  for (int r = 0; r < ROWS && libTop + r < (int)games.size(); r++) {
    int i = libTop + r;
    auto& g = games[i];
    auto& c = store::cards[g.card];
    int y = LIST_Y + r * ROW_H;
    bool sel = i == libSel;
    canvas.fillRoundRect(6, y + 2, 308, ROW_H - 4, 8, sel ? C_HI : C_PANEL);
    canvas.fillRoundRect(12, y + 5, 36, 36, 5, 0x0000);
    drawIcon(canvas, g.icon, 14, y + 7, 2);
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(C_TEXT);
    String t = g.title; if (t.length() > 24) t = t.substring(0, 24);
    canvas.drawString(t, 56, y + 6);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(c.conflict ? C_WARN : c.dirty ? C_WARN : C_MUTED);
    String sub = g.dir ? store::displayName(c) : String("BIOS menu, clock & files");
    if (c.conflict) sub += "  - changed on PC too";
    else if (c.dirty) sub += "  - not synced yet";
    canvas.drawString(sub, 56, y + 28);
    touchZones.push_back({0, (int16_t)y, 320, ROW_H, 0});  // handled specially (row tap)
  }
  // scroll hints
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  if (libTop > 0) canvas.drawString("^", 160, LIST_Y - 2);
  canvas.fillRect(0, 218, 320, 22, C_PANEL);
  canvas.setFont(&fonts::Font0);
  canvas.drawString(gpPresent ? "A: play    START: sync & settings" : "Tap a game to play   Hold top bar: settings", 160, 229);
  canvas.pushSprite(0, 0);
}

static int libraryTapRow() {
  static bool wasDown = false;
  bool down = false; int ty = -1, tx = -1;
  if (M5.Touch.getCount()) { auto t = M5.Touch.getDetail(0); if (t.isPressed()) { down = true; ty = t.y; tx = t.x; } }
  int r = -1;
  if (down && !wasDown) {
    if (ty < 30) r = -2;  // top bar -> settings
    else if (ty >= LIST_Y && ty < LIST_Y + ROWS * ROW_H) r = libTop + (ty - LIST_Y) / ROW_H;
    else if (ty >= 210) r = -3;  // footer -> scroll down
    (void)tx;
  }
  wasDown = down;
  return r;
}

// ------------------------------------------------------------------ menus
static int drawMenu(const char* title, const std::vector<String>& items, int sel) {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(title, 12, 18);
  for (size_t i = 0; i < items.size(); i++) {
    int y = 38 + i * 34;
    canvas.fillRoundRect(8, y, 304, 30, 7, (int)i == sel ? C_HI : C_PANEL);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(items[i], 20, y + 15);
  }
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_center);
  canvas.drawString(gpPresent ? "UP/DOWN choose   A select   B back" : "Tap an option", 160, 229);
  canvas.pushSprite(0, 0);
  return 0;
}
static int menuTap(int count) {
  static bool wasDown = false;
  bool down = false; int ty = -1;
  if (M5.Touch.getCount()) { auto t = M5.Touch.getDetail(0); if (t.isPressed()) { down = true; ty = t.y; } }
  int r = -1;
  if (down && !wasDown && ty >= 38) { int i = (ty - 38) / 34; if (i < count) r = i; }
  wasDown = down;
  return r;
}

static std::vector<String> pauseItems() {
  return {"Resume", "Restart game", "Save & back to library", String("Sound: ") + (muted ? "off" : String(volume * 100 / 255) + "%"), "Set clock from network time"};
}
static std::vector<String> sysItems() {
  std::vector<String> v;
  v.push_back("Back");
  v.push_back(store::wifiConfigured() ? "Sync with PC now" : "Sync with PC (set Wi-Fi in config.json)");
  v.push_back("Send this device's changes to PC");
  v.push_back("Send changes, overwrite PC copy");
  v.push_back(String("Sound: ") + (muted ? "off" : String(volume * 100 / 255) + "%"));
  return v;
}

static void cycleVolume() {
  if (muted) { muted = false; volume = 64; }
  else if (volume < 100) volume = 128;
  else if (volume < 180) volume = 220;
  else muted = true;
  M5.Speaker.setVolume(muted ? 0 : volume);
}

static void doSync(bool push, bool force) {
  if (!store::wifiConnect(10000, status)) { toast("Wi-Fi not connected (check config.json)"); return; }
  String msg;
  if (push) { status("Sending to PC..."); store::pushToPC(msg, force); toast(msg); }
  status("Checking your PC for new cards...");
  store::pullFromPC(msg);
  toast(msg);
}

// ------------------------------------------------------------------ setup / loop
static void fatal(const String& a, const String& b) {
  M5.Display.fillScreen(C_BG);
  M5.Display.setTextColor(C_TEXT);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(a, 160, 100);
  M5.Display.setTextColor(C_MUTED);
  M5.Display.drawString(b, 160, 130);
  Serial.println(a + " - " + b);
  while (true) delay(1000);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);
  LCD_BG = M5.Display.color565(168, 179, 154);
  LCD_INK = M5.Display.color565(29, 36, 24);
  LCD_GAP = M5.Display.color565(150, 161, 136);
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(320, 240);
  status("PocketStation starting...");

  cardBuf = (uint8_t*)heap_caps_malloc(0x20000, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!cardBuf) cardBuf = (uint8_t*)ps_malloc(0x20000);
  stateBuf = (uint8_t*)malloc(emu.stateSize() + 16);
  cardShadow = (uint8_t*)ps_malloc(0x20000);
  if (!cardShadow) cardShadow = (uint8_t*)malloc(0x20000);
  if (!cardBuf || !stateBuf || !cardShadow) fatal("Out of memory", "");
  xTaskCreatePinnedToCore(emuTaskFn, "pocketstation", 8192, nullptr, 2, &emuTask, 0);
  Serial.printf("card buffer at %p, free heap %u, free PSRAM %u\n", cardBuf, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  if (!store::begin()) fatal("Storage error", "Run: pio run -t uploadfs");
  volume = store::config.volume;
  M5.Speaker.begin();
  M5.Speaker.setVolume(volume);
  gpInit();

  if (store::wifiConfigured()) {
    if (store::wifiConnect(10000, status)) {
      status("Getting the time...");
      if (store::syncTime()) rtcFromSystem();
      String msg;
      status("Checking your PC for cards...");
      if (store::pullFromPC(msg) >= 0) toast(msg, 1200); else toast(msg, 1800);
    } else toast("Wi-Fi didn't connect - playing offline", 1500);
  }
  if (!store::loadBios(biosBuf)) {
    if (WiFi.status() == WL_CONNECTED && store::fetchBiosFromPC() && store::loadBios(biosBuf)) toast("BIOS copied from your PC", 1000);
    else fatal("No PocketStation BIOS found", "Put bios.bin in data/ and run uploadfs");
  }
  {
    // a quick check that this is a BIOS we know how to boot
    char ver[5] = {0}; memcpy(ver, biosBuf + 0x3FFC, 4);
    Serial.printf("BIOS version %s\n", ver);
  }
  drawLibrary();
}

void loop() {
  M5.update();
  readInput();

  switch (mode) {
    case M_LIBRARY: {
      int n = store::games.size();
      int tap = libraryTapRow();
      bool redraw = false;
      if ((padPressed & P_DOWN) || tap == -3) { if (libSel < n - 1) { libSel++; redraw = true; } }
      if (padPressed & P_UP) { if (libSel > 0) { libSel--; redraw = true; } }
      if (padPressed & P_RIGHT) { libSel = min(n - 1, libSel + ROWS); redraw = true; }
      if (padPressed & P_LEFT) { libSel = max(0, libSel - ROWS); redraw = true; }
      if (tap >= 0 && tap < n) { libSel = tap; startGame(libSel); break; }
      if ((padPressed & (P_A | P_B)) && n) { startGame(libSel); break; }
      if ((padPressed & P_START) || tap == -2) { mode = M_SYSMENU; menuSel = 0; drawMenu("Settings & sync", sysItems(), menuSel); break; }
      if (padPressed & P_SELECT) { cycleVolume(); redraw = true; }
      if (redraw) drawLibrary();
      delay(10);
      break;
    }

    case M_GAME: {
      if (padPressed & P_START) { emuStop(); saveCardIfDirty(true); M5.Speaker.stop(); mode = M_PAUSE; menuSel = 0; drawMenu("Paused", pauseItems(), menuSel); break; }
      if (padPressed & P_SELECT) { muted = !muted; M5.Speaker.setVolume(muted ? 0 : volume); }
      uint32_t btn = 0;
      if (pad & P_UP) btn |= ps::BTN_UP;
      if (pad & P_DOWN) btn |= ps::BTN_DOWN;
      if (pad & P_LEFT) btn |= ps::BTN_LEFT;
      if (pad & P_RIGHT) btn |= ps::BTN_RIGHT;
      if (pad & (P_A | P_B)) btn |= ps::BTN_FIRE;
      emuButtons = btn;
      pumpAudio();
      if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        drawLcd();
      }
      saveCardIfDirty(false);
      static uint32_t lastReport = 0;
      if (millis() - lastReport > 5000) {
        lastReport = millis();
        Serial.printf("emu load %u%%  clk mode %u  sleeping %d\n", (unsigned)emuLoadPct, emu.clkMode, emu.sleeping);
      }
      delay(4);
      break;
    }

    case M_PAUSE: {
      auto items = pauseItems();
      int tap = menuTap(items.size());
      if (padPressed & P_DOWN) menuSel = (menuSel + 1) % items.size();
      if (padPressed & P_UP) menuSel = (menuSel + items.size() - 1) % items.size();
      int choice = -1;
      if (padPressed & P_A) choice = menuSel;
      if (tap >= 0) choice = tap;
      if (padPressed & (P_B | P_START)) choice = 0;
      if (choice == 0) { drawGameChrome(); mode = M_GAME; emuStart(); break; }
      if (choice == 1) {
        LittleFS.remove(store::statePath(store::games[curGame].card, store::games[curGame].dir));
        status("Restarting...");
        emu.bootFile(store::games[curGame].dir, nowDT());
        drawGameChrome(); mode = M_GAME; emuStart(); break;
      }
      if (choice == 2) { quitGame(); break; }
      if (choice == 3) cycleVolume();
      if (choice == 4) {
        if (store::wifiConnect(8000, status) && store::syncTime()) { rtcFromSystem(); emu.syncClock(nowDT()); toast("Clock set", 900); }
        else toast("Couldn't get network time");
      }
      if (padPressed || tap >= 0) drawMenu("Paused", pauseItems(), menuSel);
      delay(10);
      break;
    }

    case M_SYSMENU: {
      auto items = sysItems();
      int tap = menuTap(items.size());
      if (padPressed & P_DOWN) menuSel = (menuSel + 1) % items.size();
      if (padPressed & P_UP) menuSel = (menuSel + items.size() - 1) % items.size();
      int choice = -1;
      if (padPressed & P_A) choice = menuSel;
      if (tap >= 0) choice = tap;
      if (padPressed & (P_B | P_START)) choice = 0;
      if (choice == 0) { mode = M_LIBRARY; drawLibrary(); break; }
      if (choice == 1) doSync(false, false);
      if (choice == 2) doSync(true, false);
      if (choice == 3) doSync(true, true);
      if (choice == 4) cycleVolume();
      if (choice >= 1 && choice <= 3) { mode = M_LIBRARY; drawLibrary(); break; }
      if (padPressed || tap >= 0) drawMenu("Settings & sync", sysItems(), menuSel);
      delay(10);
      break;
    }
  }
}
