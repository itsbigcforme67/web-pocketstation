#include "storage.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <algorithm>

namespace store {

Config config;
std::vector<CardInfo> cards;
std::vector<GameEntry> games;

static const char* INDEX_PATH = "/index.json";

static uint8_t* bigAlloc(size_t n) {
  uint8_t* p = (uint8_t*)ps_malloc(n);
  if (!p) p = (uint8_t*)malloc(n);
  return p;
}

static void loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  config.wifiSsid = doc["wifi_ssid"] | "";
  config.wifiPass = doc["wifi_pass"] | "";
  config.server = doc["server"] | "";
  config.token = doc["token"] | "";
  config.tz = doc["timezone"] | "EST5EDT,M3.2.0,M11.1.0";
  config.volume = doc["volume"] | 128;
  config.idleSkip = doc["idle_skip"] | true;
  while (config.server.endsWith("/")) config.server.remove(config.server.length() - 1);
}

static void loadIndex() {
  cards.clear();
  File f = LittleFS.open(INDEX_PATH, "r");
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    for (JsonObject o : doc["cards"].as<JsonArray>()) {
      CardInfo c;
      c.file = o["file"] | ""; c.name = o["name"] | ""; c.id = o["id"] | ""; c.sha = o["sha"] | "";
      c.dirty = o["dirty"] | false; c.conflict = o["conflict"] | false;
      if (c.file.length()) cards.push_back(c);
    }
  }
  f.close();
}

void saveIndex() {
  JsonDocument doc;
  JsonArray arr = doc["cards"].to<JsonArray>();
  for (auto& c : cards) {
    JsonObject o = arr.add<JsonObject>();
    o["file"] = c.file; o["name"] = c.name; o["id"] = c.id; o["sha"] = c.sha;
    o["dirty"] = c.dirty; o["conflict"] = c.conflict;
  }
  File f = LittleFS.open("/index.tmp", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(INDEX_PATH);
  LittleFS.rename("/index.tmp", INDEX_PATH);
}

bool begin() {
  if (!LittleFS.begin(true)) return false;
  if (!LittleFS.exists("/cards")) LittleFS.mkdir("/cards");
  if (!LittleFS.exists("/st")) LittleFS.mkdir("/st");
  loadConfig();
  loadIndex();
  scan();
  return true;
}

static bool isCardFile(const String& n) {
  String l = n; l.toLowerCase();
  return l.endsWith(".mcd") || l.endsWith(".mcr") || l.endsWith(".gme") || l.endsWith(".mem") || l.endsWith(".bin");
}

static int headerLenFor(File& f) {
  size_t sz = f.size();
  if (sz == ::cards::CARD_SIZE) return 0;
  uint8_t h[16] = {0};
  f.seek(0); f.read(h, 16);
  int r = ::cards::containerHeader(h, sz);
  return r;
}

String displayName(const CardInfo& c) {
  String n = c.name.length() ? c.name : c.file;
  int cut = n.length();
  int a = n.indexOf('('), b = n.indexOf('['), d = n.lastIndexOf('.');
  if (a > 0 && a < cut) cut = a;
  if (b > 0 && b < cut) cut = b;
  if (!c.name.length() && d > 0 && d < cut) cut = d;
  n = n.substring(0, cut);
  n.trim();
  return n.length() ? n : c.file;
}

void scan() {
  // 1. sync index with files on disk
  std::vector<String> files;
  File root = LittleFS.open("/cards");
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    String n = e.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (!e.isDirectory() && isCardFile(n)) files.push_back(n);
    e.close();
  }
  root.close();
  std::vector<CardInfo> kept;
  for (auto& c : cards) for (auto& fn : files) if (fn == c.file) { kept.push_back(c); break; }
  for (auto& fn : files) {
    bool known = false;
    for (auto& c : kept) if (c.file == fn) known = true;
    if (!known) { CardInfo c; c.file = fn; c.name = fn; int d = c.name.lastIndexOf('.'); if (d > 0) c.name = c.name.substring(0, d); kept.push_back(c); }
  }
  cards = kept;
  std::sort(cards.begin(), cards.end(), [](const CardInfo& a, const CardInfo& b) { return displayName(a) < displayName(b); });
  saveIndex();

  // 2. build the game list
  games.clear();
  static uint8_t dir[0x800], head[0x100];
  std::vector<GameEntry> menus;
  for (size_t ci = 0; ci < cards.size(); ci++) {
    File f = LittleFS.open("/cards/" + cards[ci].file, "r");
    if (!f) continue;
    int hl = headerLenFor(f);
    if (hl < 0) { f.close(); continue; }
    f.seek(hl); f.read(dir, sizeof dir);
    auto saves = ::cards::parseDirectory(dir);
    GameEntry menu; menu.card = ci; menu.dir = 0; menu.title = "Menu: " + displayName(cards[ci]);
    memset(menu.icon, 0, sizeof menu.icon);
    bool menuIcon = false;
    for (auto& s : saves) {
      f.seek(hl + s.firstBlock * ::cards::BLOCK); f.read(head, sizeof head);
      ::cards::parseHeader(s, head);
      if (!menuIcon) { ::cards::decodeIcon(head, menu.icon, false); menuIcon = true; }
      if (!s.pocket) continue;
      GameEntry g; g.card = ci; g.dir = s.dirIndex;
      g.title = (s.title.length() && !s.titlePartial) ? String(s.title.c_str()) : displayName(cards[ci]);
      ::cards::decodeIcon(head, g.icon, false);
      games.push_back(g);
    }
    menus.push_back(menu);
    f.close();
  }
  for (auto& m : menus) games.push_back(m);
}

bool loadCard(int idx, uint8_t* buf) {
  File f = LittleFS.open("/cards/" + cards[idx].file, "r");
  if (!f) return false;
  int hl = headerLenFor(f);
  if (hl < 0) { f.close(); return false; }
  f.seek(hl);
  size_t n = f.read(buf, ::cards::CARD_SIZE);
  f.close();
  return n == ::cards::CARD_SIZE;
}

bool saveCard(int idx, const uint8_t* buf) {
  String path = "/cards/" + cards[idx].file;
  uint8_t header[3904];
  int hl = 0;
  {
    File f = LittleFS.open(path, "r");
    if (f) { hl = headerLenFor(f); if (hl > 0) { f.seek(0); f.read(header, hl); } else hl = 0; f.close(); }
  }
  File o = LittleFS.open("/cards/.tmp", "w");
  if (!o) return false;
  if (hl) o.write(header, hl);
  size_t n = o.write(buf, ::cards::CARD_SIZE);
  o.close();
  if (n != ::cards::CARD_SIZE) { LittleFS.remove("/cards/.tmp"); return false; }
  LittleFS.remove(path);
  return LittleFS.rename("/cards/.tmp", path);
}

bool loadBios(uint8_t* buf) {
  File f = LittleFS.open("/bios.bin", "r");
  if (!f || f.size() != 0x4000) { if (f) f.close(); return false; }
  f.read(buf, 0x4000);
  f.close();
  return true;
}

String statePath(int card, int dir) {
  return "/st/" + String(::cards::crc32((const uint8_t*)cards[card].file.c_str(), cards[card].file.length()), HEX) + "_" + String(dir) + ".st";
}
void dropStates(int card) { for (int d = 0; d < 16; d++) LittleFS.remove(statePath(card, d)); }

// ------------------------------------------------------------------ network
bool wifiConfigured() { return config.wifiSsid.length() > 0; }

bool wifiConnect(uint32_t timeoutMs, void (*status)(const char*)) {
  if (!wifiConfigured()) return false;
  if (WiFi.status() == WL_CONNECTED) return true;
  if (status) status(("Connecting to " + config.wifiSsid + "...").c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

bool syncTime() {
  configTzTime(config.tz.c_str(), "pool.ntp.org", "time.nist.gov");
  struct tm tm;
  return getLocalTime(&tm, 6000);
}

static void addAuth(HTTPClient& http) {
  if (config.token.length()) http.addHeader("X-Token", config.token);
}

static String urlEncode(const String& s) {
  String o;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}

static bool httpGetBytes(const String& url, uint8_t* buf, size_t want) {
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(url)) return false;
  addAuth(http);
  int code = http.GET();
  if (code != 200 || (size_t)http.getSize() != want) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < want && millis() - t0 < 20000) {
    int n = s->available();
    if (n > 0) got += s->readBytes(buf + got, min((size_t)n, want - got));
    else delay(2);
  }
  http.end();
  return got == want;
}

int pullFromPC(String& message) {
  if (!config.server.length()) { message = "No PC address in config.json"; return -1; }
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(config.server + "/api/cards")) { message = "Bad server address"; return -1; }
  addAuth(http);
  int code = http.GET();
  if (code != 200) { message = code == 401 ? "PC wants an access token" : "PC not reachable (" + String(code) + ")"; http.end(); return -1; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { message = "Bad reply from PC"; return -1; }
  uint8_t* buf = bigAlloc(::cards::CARD_SIZE);
  if (!buf) { message = "Out of memory"; return -1; }
  int updated = 0, conflicts = 0, failed = 0;
  for (JsonObject o : doc["cards"].as<JsonArray>()) {
    String id = o["id"] | "", name = o["name"] | "", sha = o["sha1"] | "";
    if (!id.length()) continue;
    int idx = -1;
    for (size_t i = 0; i < cards.size(); i++) if (cards[i].id == id) idx = i;
    if (idx >= 0 && cards[idx].sha == sha) { if (cards[idx].name != name) cards[idx].name = name; continue; }
    if (idx >= 0 && cards[idx].dirty) { cards[idx].conflict = true; conflicts++; continue; }
    if (!httpGetBytes(config.server + "/api/cards/" + urlEncode(id) + "/data", buf, ::cards::CARD_SIZE)) { failed++; continue; }
    if (idx < 0) {
      // Same card already on the device as a local file? Link it instead of duplicating.
      uint32_t crc = ::cards::crc32(buf, ::cards::CARD_SIZE);
      uint8_t* tmp = bigAlloc(::cards::CARD_SIZE);
      for (size_t i = 0; tmp && i < cards.size() && idx < 0; i++)
        if (!cards[i].id.length() && loadCard(i, tmp) && ::cards::crc32(tmp, ::cards::CARD_SIZE) == crc) idx = i;
      if (tmp) free(tmp);
      if (idx >= 0) { cards[idx].id = id; cards[idx].sha = sha; cards[idx].name = name; continue; }
    }
    if (idx < 0) {
      CardInfo c; c.id = id; c.name = name;
      c.file = "pc-" + String(::cards::crc32((const uint8_t*)id.c_str(), id.length()), HEX) + ".mcd";
      cards.push_back(c); idx = cards.size() - 1;
    }
    cards[idx].sha = sha; cards[idx].name = name; cards[idx].dirty = false; cards[idx].conflict = false;
    saveCard(idx, buf);
    dropStates(idx);
    updated++;
  }
  free(buf);
  saveIndex();
  scan();
  message = String(updated) + " card(s) updated from PC";
  if (conflicts) message += ", " + String(conflicts) + " changed in both places";
  if (failed) message += ", " + String(failed) + " failed";
  return updated;
}

int pushToPC(String& message, bool force) {
  if (!config.server.length()) { message = "No PC address in config.json"; return -1; }
  uint8_t* buf = bigAlloc(::cards::CARD_SIZE);
  if (!buf) { message = "Out of memory"; return -1; }
  int sent = 0, conflicts = 0, failed = 0;
  for (size_t i = 0; i < cards.size(); i++) {
    CardInfo& c = cards[i];
    if (!c.id.length() || !c.dirty) continue;
    if (!loadCard(i, buf)) { failed++; continue; }
    HTTPClient http;
    http.setTimeout(15000);
    http.begin(config.server + "/api/cards/" + urlEncode(c.id) + "/data" + (force ? "?force=1" : ""));
    addAuth(http);
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("X-Base-Sha1", c.sha);
    int code = http.PUT(buf, ::cards::CARD_SIZE);
    String body = http.getString();
    http.end();
    if (code == 200) {
      JsonDocument d; deserializeJson(d, body);
      c.sha = d["sha1"] | c.sha; c.dirty = false; c.conflict = false; sent++;
    } else if (code == 409) { c.conflict = true; conflicts++; }
    else failed++;
  }
  free(buf);
  saveIndex();
  message = String(sent) + " card(s) sent to PC";
  if (conflicts) message += ", " + String(conflicts) + " also changed on PC";
  if (failed) message += ", " + String(failed) + " failed";
  return sent;
}

bool fetchBiosFromPC() {
  if (!config.server.length()) return false;
  uint8_t* buf = bigAlloc(0x4000);
  if (!buf) return false;
  bool ok = httpGetBytes(config.server + "/api/bios", buf, 0x4000);
  if (ok) { File f = LittleFS.open("/bios.bin", "w"); if (f) { f.write(buf, 0x4000); f.close(); } else ok = false; }
  free(buf);
  return ok;
}

}  // namespace store
