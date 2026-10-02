#include "cards.h"
#include <string.h>

namespace cards {

std::vector<Save> parseDirectory(const uint8_t* d) {
  std::vector<Save> out;
  if (d[0] != 'M' || d[1] != 'C') return out;
  for (int i = 1; i < 16; i++) {
    const uint8_t* f = d + i * FRAME;
    if (f[0] != 0x51) continue;  // first block of a file
    Save s;
    s.dirIndex = i; s.firstBlock = i; s.blocks = 1; s.pocket = false; s.titlePartial = false;
    for (int k = 0; k < 20 && f[0x0A + k]; k++) s.filename += (char)f[0x0A + k];
    int next = f[8] | (f[9] << 8), guard = 0;
    while (next != 0xFFFF && next < 15 && guard++ < 15) { s.blocks++; const uint8_t* nf = d + (next + 1) * FRAME; next = nf[8] | (nf[9] << 8); }
    out.push_back(s);
  }
  return out;
}

void parseHeader(Save& s, const uint8_t* h) {
  bool sc = h[0] == 'S' && h[1] == 'C';
  s.pocket = sc && h[0x52] == 'M' && h[0x53] == 'C' && h[0x54] == 'X' && (h[0x55] == '0' || h[0x55] == '1');
  s.titlePartial = false;
  s.title = sc ? sjisTitle(h + 4, 64, &s.titlePartial) : "";
}

static inline uint16_t rgb555to565(uint16_t v) {
  uint16_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
  return (uint16_t)((r << 11) | (g << 6) | (g >> 4) | b);
}

void decodeIcon(const uint8_t* h, uint16_t out[256], bool swapBytes) {
  uint16_t pal[16];
  for (int i = 0; i < 16; i++) {
    uint16_t v = h[0x60 + i * 2] | (h[0x61 + i * 2] << 8);
    uint16_t c = v == 0 ? 0x0000 : rgb555to565(v);
    pal[i] = swapBytes ? (uint16_t)((c >> 8) | (c << 8)) : c;
  }
  for (int i = 0; i < 128; i++) {
    uint8_t b = h[0x80 + i];
    out[i * 2] = pal[b & 15];
    out[i * 2 + 1] = pal[b >> 4];
  }
}

// Shift-JIS -> ASCII for the characters that have ASCII equivalents.
static int sjisChar(const uint8_t* p, int len, char* outc) {
  uint8_t a = p[0];
  if (a < 0x80) { *outc = (a >= 0x20 && a < 0x7F) ? (char)a : 0; return 1; }
  if (len < 2) return 1;
  uint8_t b = p[1];
  *outc = 0;
  if (a == 0x81) {
    switch (b) {
      case 0x40: *outc = ' '; break;   case 0x46: *outc = ':'; break; case 0x44: *outc = '.'; break;
      case 0x43: *outc = ','; break;   case 0x5E: *outc = '/'; break; case 0x7C: *outc = '-'; break;
      case 0x5B: *outc = '-'; break;   case 0x69: *outc = '('; break; case 0x6A: *outc = ')'; break;
      case 0x49: *outc = '!'; break;   case 0x48: *outc = '?'; break; case 0x95: *outc = '&'; break;
      case 0x75: *outc = '\x01'; break;  // 「 marker
      case 0x76: *outc = '\x02'; break;  // 」 marker
    }
  } else if (a == 0x82) {
    if (b >= 0x4F && b <= 0x58) *outc = (char)('0' + b - 0x4F);
    else if (b >= 0x60 && b <= 0x79) *outc = (char)('A' + b - 0x60);
    else if (b >= 0x81 && b <= 0x9A) *outc = (char)('a' + b - 0x81);
    else *outc = '\x03';  // kana: non-ASCII
  } else if ((a >= 0x83 && a <= 0x9F) || (a >= 0xE0 && a <= 0xEF)) {
    *outc = '\x03';
  }
  return ((a >= 0x81 && a <= 0x9F) || (a >= 0xE0 && a <= 0xFC)) ? 2 : 1;
}

std::string sjisTitle(const uint8_t* p, int maxLen, bool* partial) {
  std::string all, quoted;
  bool kana = false, kanaInQuote = false;
  bool inQuote = false, sawQuote = false;
  for (int i = 0; i < maxLen && p[i];) {
    char c; int n = sjisChar(p + i, maxLen - i, &c); i += n;
    if (c == '\x01') { inQuote = true; sawQuote = true; quoted.clear(); continue; }
    if (c == '\x02') { inQuote = false; continue; }
    if (c == '\x03') { kana = true; if (inQuote) kanaInQuote = true; if (!all.empty() && all.back() != ' ') all += ' '; continue; }
    if (!c) continue;
    if (inQuote) quoted += c;
    all += c;
  }
  auto letters = [](const std::string& s) { int n = 0; for (char ch : s) if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) n++; return n; };
  auto trim = [](std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '/')) s.pop_back(); size_t k = 0; while (k < s.size() && (s[k] == ' ' || s[k] == '/')) k++; return s.substr(k); };
  if (sawQuote && letters(quoted) >= 2) { if (partial) *partial = kanaInQuote; return trim(quoted); }
  if (partial) *partial = kana;
  if (letters(all) >= 3) return trim(all);
  return "";
}

int containerHeader(const uint8_t* d, size_t len) {
  if (len == CARD_SIZE) return 0;
  if (len == CARD_SIZE + 3904 && memcmp(d, "123-456-STD", 11) == 0) return 3904;
  if (len == CARD_SIZE + 64 && memcmp(d, "VgsM", 4) == 0) return 64;
  return -1;
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
  crc = ~crc;
  while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}

}  // namespace cards
