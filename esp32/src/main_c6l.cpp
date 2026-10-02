// Web PocketStation - M5Stack Unit C6L build (ESP32-C6, 64x48 OLED, one button).
// A performance test bench as much as a player: it benchmarks every game at start-up and
// reports speed on the OLED and the serial monitor.
//
// Controls
//   Button (short)     PocketStation action button
//   Button (hold 2 s)  next game
//   Serial monitor     w a s d = D-pad, space or k = action button
//                      n / p = next / previous game, r = restart game
//                      b = run the benchmark again, i = idle-skip on/off, ? = help
#include <M5Unified.h>
#include <LittleFS.h>
#include <time.h>
#include "pscore.h"
#include "storage.h"

using ps::PocketStation;

static PocketStation emu;
static uint8_t biosBuf[0x4000];
static uint8_t* cardBuf = nullptr;
static uint8_t* stateBuf = nullptr;
static M5Canvas oled(&M5.Display);

static int curGame = -1;
static volatile bool cardDirty = false;
static uint32_t lastFlashWrite = 0;
static uint32_t shownRows[32];
static bool forceDraw = true, ledShown = false;
static int shownSpeed = -1;

// timing statistics
static uint32_t lastUs = 0, lastDraw = 0, winStart = 0;
static uint64_t winBusyUs = 0, winEmuUs = 0, winWallUs = 0;
static int speedPct = 100, loadPct = 0;

// serial "held key" emulation: each received character holds its button for a short time
static uint32_t keyUntil[5] = {0, 0, 0, 0, 0};  // fire, right, left, down, up
static const uint32_t KEY_HOLD_MS = 160;
static uint32_t btnDownAt = 0;
static bool btnLongFired = false;

static void onFlashWrite(void*, uint32_t) { cardDirty = true; lastFlashWrite = millis(); }

static ps::DateTime nowDT() {
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tm; localtime_r(&t, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday};
  }
  return {2026, 1, 1, 12, 0, 0, 4};  // no clock source on this board without Wi-Fi
}

// ------------------------------------------------------------------ display
// The 32x32 PocketStation LCD is scaled 1.5x to 48x48 (pixels alternate 2 and 1 wide), leaving a
// 16-pixel column on the right for the LED and a speed read-out. Lit OLED pixel = dark LCD pixel.
static void message(const char* a, const char* b = "", const char* c = "") {
  oled.fillScreen(0);
  oled.setFont(&fonts::Font0);
  oled.setTextColor(1);
  oled.setCursor(0, 2); oled.print(a);
  oled.setCursor(0, 16); oled.print(b);
  oled.setCursor(0, 30); oled.print(c);
  oled.pushSprite(0, 0);
  forceDraw = true;
}

static void drawFrame() {
  uint32_t rows[32];
  emu.getFrame(rows);
  bool led = emu.ledOn();
  bool changed = forceDraw || led != ledShown || shownSpeed != speedPct;
  if (!changed) for (int y = 0; y < 32; y++) if (rows[y] != shownRows[y]) { changed = true; break; }
  if (!changed) return;
  oled.fillScreen(0);
  for (int dy = 0; dy < 48; dy++) {
    uint32_t r = rows[dy * 2 / 3];
    if (!r) continue;
    for (int dx = 0; dx < 48; dx++) if ((r >> (dx * 2 / 3)) & 1) oled.drawPixel(dx, dy, 1);
  }
  // right-hand column: speed %, LED
  oled.setFont(&fonts::Font0);
  oled.setTextColor(1);
  char buf[6];
  snprintf(buf, sizeof buf, "%d", speedPct > 99 ? 99 : speedPct);
  oled.setCursor(51, 2); oled.print(buf);
  oled.setCursor(51, 12); oled.print("%");
  if (led) oled.fillCircle(56, 40, 4, 1); else oled.drawCircle(56, 40, 4, 1);
  oled.pushSprite(0, 0);
  memcpy(shownRows, rows, sizeof rows);
  ledShown = led; shownSpeed = speedPct; forceDraw = false;
}

// ------------------------------------------------------------------ game lifecycle
static void saveCardIfDirty(bool force) {
  if (!cardDirty || curGame < 0) return;
  if (!force && millis() - lastFlashWrite < 3000) return;
  cardDirty = false;
  store::saveCard(store::games[curGame].card, cardBuf);
  lastUs = micros();  // don't count the pause as emulation lag
}
static void saveState() {
  if (curGame < 0) return;
  auto& g = store::games[curGame];
  emu.saveState(stateBuf);
  File f = LittleFS.open(store::statePath(g.card, g.dir), "w");
  if (f) { f.write(stateBuf, emu.stateSize()); f.close(); }
}

static bool loadGame(int gi, bool allowResume) {
  auto& g = store::games[gi];
  if (!store::loadCard(g.card, cardBuf)) return false;
  emu.init(biosBuf, cardBuf);
  emu.idleSkip = store::config.idleSkip;
  emu.onFlashWrite = onFlashWrite;
  emu.audioRing = nullptr;  // no sound in this build
  cardDirty = false;
  bool resumed = false;
  if (allowResume) {
    File f = LittleFS.open(store::statePath(g.card, g.dir), "r");
    if (f) {
      size_t n = f.read(stateBuf, emu.stateSize() + 1);
      f.close();
      emu.hardReset(nowDT());
      resumed = emu.loadState(stateBuf, n, nowDT());
    }
  }
  if (!resumed && !emu.bootFile(g.dir, nowDT())) return false;
  return true;
}

static void startGame(int gi, bool allowResume = true) {
  if (store::games.empty()) return;
  if (curGame >= 0) { saveState(); saveCardIfDirty(true); }
  gi = (gi % (int)store::games.size() + store::games.size()) % store::games.size();
  auto& g = store::games[gi];
  message("Loading", g.title.substring(0, 10).c_str());
  curGame = gi;
  if (!loadGame(gi, allowResume)) { message("Can't", "start", "game"); curGame = -1; delay(1500); return; }
  Serial.printf("\nPlaying [%d] %s\n", gi, g.title.c_str());
  memset(shownRows, 0, sizeof shownRows);
  forceDraw = true;
  lastUs = micros(); winStart = millis(); winBusyUs = winEmuUs = winWallUs = 0;
}

// ------------------------------------------------------------------ benchmark
// Runs each game flat out for `seconds` of emulated time (tapping the action button now and
// then so it gets past title screens) and reports speed relative to a real PocketStation.
static bool benchAbort() {
  M5.update();
  if (M5.BtnA.wasPressed()) return true;
  if (Serial.available()) { while (Serial.available()) Serial.read(); return true; }
  return false;
}

static void benchmark(int seconds) {
  if (curGame >= 0) { saveState(); saveCardIfDirty(true); }
  curGame = -1;
  Serial.printf("\n=== Benchmark: %d emulated seconds per game, idle-skip %s ===\n", seconds, store::config.idleSkip ? "on" : "off");
  Serial.println("(press the button or send any key to stop)");
  Serial.printf("%-22s %8s %9s  %s\n", "game", "speed", "CPU need", "verdict");
  bool aborted = false;
  for (size_t gi = 0; gi < store::games.size() && !aborted; gi++) {
    auto& g = store::games[gi];
    if (g.dir == 0) continue;  // skip the BIOS-menu entries
    message("Bench", g.title.substring(0, 10).c_str());
    if (!loadGame(gi, false)) { Serial.printf("%-22s  could not start\n", g.title.c_str()); continue; }
    uint64_t busy = 0;
    const uint32_t sliceMs = 20;
    uint32_t sinceYield = millis();
    for (uint32_t ms = 0; ms < (uint32_t)seconds * 1000 && !aborted; ms += sliceMs) {
      // a 150 ms button tap every 3 emulated seconds
      uint32_t phase = ms % 3000;
      emu.setButtons((ms >= 3000 && phase < 150) ? ps::BTN_FIRE : 0);
      uint32_t t0 = micros();
      emu.runMs(sliceMs);
      busy += micros() - t0;
      if (millis() - sinceYield > 80) { delay(1); sinceYield = millis(); if (benchAbort()) aborted = true; }
    }
    if (aborted) break;
    uint32_t need = (uint32_t)(busy / ((uint64_t)seconds * 10000));   // % of one core needed for real time
    uint32_t speed = need ? 10000 / need : 999;                        // % of real speed when flat out
    Serial.printf("%-22.22s %7u%% %8u%%  %s\n", g.title.c_str(), (unsigned)speed, (unsigned)need,
                  need <= 85 ? "full speed" : need <= 100 ? "full speed, no headroom" : "too slow");
    char l2[12], l3[12];
    snprintf(l2, sizeof l2, "%u%% speed", (unsigned)(speed > 999 ? 999 : speed));
    snprintf(l3, sizeof l3, "%s", need <= 100 ? "OK" : "SLOW");
    message(g.title.substring(0, 10).c_str(), l2, l3);
    delay(900);
  }
  Serial.println(aborted ? "=== benchmark stopped ===" : "=== benchmark done ===");
}

// ------------------------------------------------------------------ input
static void help() {
  Serial.println("\nKeys: w a s d = D-pad, space/k = action, n/p = next/previous game, r = restart,");
  Serial.println("      b = benchmark, i = idle-skip on/off, ? = this help");
}

static void handleSerial() {
  while (Serial.available()) {
    int ch = Serial.read();
    uint32_t until = millis() + KEY_HOLD_MS;
    switch (ch) {
      case ' ': case 'k': case 'K': keyUntil[0] = until; break;
      case 'd': case 'D': keyUntil[1] = until; break;
      case 'a': case 'A': keyUntil[2] = until; break;
      case 's': case 'S': keyUntil[3] = until; break;
      case 'w': case 'W': keyUntil[4] = until; break;
      case 'n': startGame(curGame + 1); break;
      case 'p': startGame(curGame - 1); break;
      case 'r':
        if (curGame >= 0) { LittleFS.remove(store::statePath(store::games[curGame].card, store::games[curGame].dir)); int g = curGame; curGame = -1; startGame(g, false); }
        break;
      case 'b': { int g = curGame < 0 ? 0 : curGame; benchmark(20); startGame(g); break; }
      case 'i':
        store::config.idleSkip = !store::config.idleSkip; emu.idleSkip = store::config.idleSkip;
        Serial.printf("idle-skip %s\n", store::config.idleSkip ? "on" : "off");
        break;
      case '?': case 'h': help(); break;
      default: break;
    }
  }
}

static uint32_t readButtons() {
  uint32_t now = millis(), b = 0;
  for (int i = 0; i < 5; i++) if ((int32_t)(keyUntil[i] - now) > 0) b |= (1u << i);
  if (M5.BtnA.isPressed()) {
    if (!btnDownAt) { btnDownAt = now ? now : 1; btnLongFired = false; }
    if (!btnLongFired && now - btnDownAt > 2000) { btnLongFired = true; startGame(curGame + 1); return 0; }
    if (!btnLongFired) b |= ps::BTN_FIRE;
  } else btnDownAt = 0;
  return b;
}

// ------------------------------------------------------------------ setup / loop
static void fatal(const char* a, const char* b, const char* c = "") {
  message(a, b, c);
  Serial.printf("FATAL: %s %s %s\n", a, b, c);
  while (true) delay(1000);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = false;   // keep the single core for the emulator
  cfg.internal_mic = false;
  cfg.fallback_board = m5::board_t::board_M5UnitC6L;
  M5.begin(cfg);
  oled.setColorDepth(1);
  oled.createSprite(M5.Display.width(), M5.Display.height());
  message("Pocket", "Station", "C6L");
  delay(600);
  Serial.printf("\nWeb PocketStation - Unit C6L build. Display %dx%d, free heap %u\n", (int)M5.Display.width(), (int)M5.Display.height(), (unsigned)ESP.getFreeHeap());

  cardBuf = (uint8_t*)malloc(0x20000);
  stateBuf = (uint8_t*)malloc(emu.stateSize() + 16);
  if (!cardBuf || !stateBuf) fatal("Out of", "memory");
  if (!store::begin()) fatal("Storage", "error:", "uploadfs");
  if (!store::loadBios(biosBuf)) fatal("No BIOS:", "bios.bin", "uploadfs");
  Serial.printf("%u game entries, free heap %u\n", (unsigned)store::games.size(), (unsigned)ESP.getFreeHeap());
  if (store::games.empty()) fatal("No cards", "found:", "uploadfs");
  help();

  benchmark(20);
  startGame(0);
}

void loop() {
  M5.update();
  handleSerial();
  if (curGame < 0) { delay(50); return; }

  uint32_t now = micros();
  uint32_t dt = now - lastUs;
  if (dt < 2000) { delay(1); return; }   // run in >= 2 ms steps; the delay also feeds the watchdog
  lastUs = now;
  uint32_t run = dt > 100000 ? 100000 : dt;   // if we fall more than 100 ms behind, drop the rest (slowdown)
  emu.setButtons(readButtons());
  if (curGame < 0) return;                    // readButtons() may have switched games
  uint32_t t0 = micros();
  emu.runMaster((uint64_t)run * ps::MAX_CLOCK / 1000000);
  winBusyUs += micros() - t0;
  winEmuUs += run; winWallUs += dt;

  if (millis() - winStart >= 2000) {
    speedPct = winWallUs ? (int)(winEmuUs * 100 / winWallUs) : 100;
    loadPct = winWallUs ? (int)(winBusyUs * 100 / winWallUs) : 0;
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 6000) {
      lastPrint = millis();
      Serial.printf("speed %d%%  emu load %d%%  clk mode %u  sleeping %d\n", speedPct, loadPct, emu.clkMode, emu.sleeping);
    }
    winStart = millis(); winBusyUs = winEmuUs = winWallUs = 0;
  }
  if (millis() - lastDraw >= 50) { lastDraw = millis(); drawFrame(); }
  saveCardIfDirty(false);
  delay(1);   // single core: always leave a moment for the system tasks
}
