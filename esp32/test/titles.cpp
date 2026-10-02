#include "../src/cards.h"
#include <cstdio>
#include <vector>
#include <cstring>
int main(int argc, char** argv) {
  for (int a = 1; a < argc; a++) {
    FILE* f = fopen(argv[a], "rb"); std::vector<uint8_t> v(200000); size_t n = fread(v.data(), 1, v.size(), f); fclose(f);
    int h = cards::containerHeader(v.data(), n); const uint8_t* c = v.data() + (h < 0 ? 0 : h);
    for (auto& s : cards::parseDirectory(c)) { cards::parseHeader(s, c + s.firstBlock * cards::BLOCK); printf("%-28.28s dir=%d blocks=%d pocket=%d title='%s' partial=%d\n", argv[a] + 15, s.dirIndex, s.blocks, s.pocket, s.title.c_str(), s.titlePartial); }
  }
}
