#include "../src/pscore.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace ps;
int main(int argc, char** argv) {
  static uint8_t bios[0x4000], f1[0x20000], f2[0x20000];
  FILE* f = fopen(argv[1], "rb"); fread(bios, 1, 0x4000, f); fclose(f);
  f = fopen(argv[2], "rb"); fread(f1, 1, 0x20000, f); fclose(f); memcpy(f2, f1, 0x20000);
  DateTime d{2026, 10, 1, 13, 30, 0, 4};
  static PocketStation a, b;
  a.init(bios, f1); a.bootFile(1, d);
  a.runMs(3000); a.setButtons(1); a.runMs(150); a.setButtons(0); a.runMs(1000);
  std::vector<uint8_t> st(a.stateSize()); a.saveState(st.data());
  memcpy(f2, f1, 0x20000);
  b.init(bios, f2); b.hardReset(d);
  bool ok = b.loadState(st.data(), st.size(), d);
  // align the RTC (loadState re-reads the clock, as on the real device)
  b.rtc = a.rtc; b.masterTime = a.masterTime;
  const int seq[] = {2, 0, 1, 0, 8, 0, 16, 0, 1, 0};
  int same = 0, total = 0;
  for (int k = 0; k < 10; k++) {
    a.setButtons(seq[k]); b.setButtons(seq[k]); a.runMs(400); b.runMs(400);
    uint32_t fa[32], fb[32]; a.getFrame(fa); b.getFrame(fb); same += !memcmp(fa, fb, sizeof fa); total++;
  }
  printf("%s: load=%d frames identical %d/%d, flash identical %d\n", argv[2] + 14, ok, same, total, !memcmp(f1, f2, 0x20000));
}
