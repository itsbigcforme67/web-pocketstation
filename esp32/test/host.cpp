// Desktop harness: ./host bios.bin card.mcd dir "script"  -> prints frame hashes/rows at checkpoints
#include "../src/pscore.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
using namespace ps;
static std::vector<uint8_t> readFile(const char* p) { FILE* f = fopen(p, "rb"); std::vector<uint8_t> v; if (!f) return v; fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(v.data(), 1, v.size(), f); fclose(f); return v; }
int main(int argc, char** argv) {
  auto bios = readFile(argv[1]); auto raw = readFile(argv[2]);
  static uint8_t flash[0x20000];
  size_t off = raw.size() == 0x20000 + 3904 ? 3904 : raw.size() == 0x20000 + 64 ? 64 : 0;
  memcpy(flash, raw.data() + off, 0x20000);
  static PocketStation ps;
  ps.init(bios.data(), flash);
  DateTime d{2026, 10, 1, 13, 30, 0, 4};
  auto t0 = std::chrono::steady_clock::now();
  ps.bootFile(atoi(argv[3]), d);
  std::string script = argc > 4 ? argv[4] : "w1000 p";
  char* s = strdup(script.c_str());
  for (char* tok = strtok(s, " "); tok; tok = strtok(nullptr, " ")) {
    if (tok[0] == 'w') ps.runMs(atoi(tok + 1));
    else if (tok[0] == 'b') { ps.setButtons(atoi(tok + 1)); }
    else if (tok[0] == 'p') {
      uint32_t fr[32]; ps.getFrame(fr);
      printf("F t=%llu", (unsigned long long)(ps.masterTime * 1000 / MAX_CLOCK));
      for (int y = 0; y < 32; y++) printf(" %08x", fr[y]);
      printf("\n");
    }
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  fprintf(stderr, "wall %.0f ms, emu %.1f s, pc %08x clk %u\n", ms, ps.masterTime / (double)MAX_CLOCK, ps.cpu.pc, ps.clkMode);
}
