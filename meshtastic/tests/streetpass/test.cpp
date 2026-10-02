// Logic test for StreetPass: encounter counting, saving, reboot, damaged file, eviction, clock rollover.
// Run with: bash run.sh
#define private public
#define protected public
#include "StreetPass.h"
#undef private
#undef protected
#include <cassert>
uint32_t fakeMillis = 1000, fakeEpoch = 0; int redraws = 0, spiLock = 0, fileWrites = 0;
NodeDB_ db; NodeDB_ *nodeDB = &db; std::map<std::string, std::string> fakeFiles; FakeFS FSCom;
static void hear(StreetPassModule &m, uint32_t from, int hops = 0, float snr = 5.4f) {
    meshtastic_MeshPacket p; p.from = from; p.hops = hops; p.rx_snr = snr;
    if (m.wantPacket(&p)) m.handleReceived(p);
}
static const StreetPassModule::Pass *get(StreetPassModule &m, uint32_t n) { int i = m.find(n); return i < 0 ? nullptr : &m.passLog[i]; }
int main() {
    const uint32_t MIN = 60000;
    auto *m = new StreetPassModule();
    assert(sizeof(StreetPassModule::Pass) == 22);
    assert(m->nodesMet() == 0);
    // ignored: ourselves, local (from 0), relayed, mqtt, unknown hops
    hear(*m, 0xAAAA0001); hear(*m, 0); hear(*m, 0x10, 1); hear(*m, 0x10, -1);
    { meshtastic_MeshPacket p; p.from = 0x10; p.via_mqtt = true; m->handleReceived(p); }
    { meshtastic_MeshPacket p; p.from = 0x10; p.transport_mechanism = 2; m->handleReceived(p); }
    assert(m->nodesMet() == 0 && redraws == 0);
    // first direct packet = new pass, no clock yet
    db.nodes[0x10] = {0x10, "ABCD"};
    hear(*m, 0x10, 0, -7.6f);
    assert(m->nodesMet() == 1 && get(*m, 0x10)->count == 1 && get(*m, 0x10)->snr == -8 && get(*m, 0x10)->rssi == -80);
    assert(!strcmp(get(*m, 0x10)->name, "ABCD") && get(*m, 0x10)->firstSeen == 0 && redraws == 1 && m->dirty);
    // chatter within the gap is the same encounter
    fakeMillis += 10 * MIN; hear(*m, 0x10); fakeMillis += 25 * MIN; hear(*m, 0x10);
    assert(get(*m, 0x10)->count == 1);
    // the gap is measured from the last packet, so 29 min later still the same; 31 min later is a new one
    fakeMillis += 29 * MIN; hear(*m, 0x10); assert(get(*m, 0x10)->count == 1);
    fakeMillis += 31 * MIN; hear(*m, 0x10); assert(get(*m, 0x10)->count == 2);
    // saving: tick writes once when dirty, then not again until stale for 30 min
    m->runOnce(); assert(fileWrites == 1 && !m->dirty);
    m->runOnce(); assert(fileWrites == 1);
    fakeMillis += 5 * MIN; hear(*m, 0x10); m->runOnce(); assert(fileWrites == 1);
    fakeMillis += 26 * MIN; m->runOnce(); assert(fileWrites == 2);
    // clock arrives; unnamed node on a channel we can't read
    fakeEpoch = 1790000000; hear(*m, 0x20);
    assert(get(*m, 0x20)->firstSeen == 1790000000 && get(*m, 0x20)->name[0] == 0 && m->totalPasses() == 3);
    m->runOnce(); assert(fileWrites == 3);
    // "reboot": log survives; a node heard 10 min ago by the clock is the same encounter, one heard 2 h ago is new
    auto *m2 = new StreetPassModule();
    assert(m2->nodesMet() == 2 && m2->totalPasses() == 3 && !strcmp(get(*m2, 0x10)->name, "ABCD"));
    fakeEpoch += 600; hear(*m2, 0x20); assert(get(*m2, 0x20)->count == 1);
    assert(get(*m2, 0x10)->lastSeen == 0);            // never seen with a valid clock...
    hear(*m2, 0x10); assert(get(*m2, 0x10)->count == 3); // ...so the first hearing after reboot counts
    fakeEpoch += 7200; fakeMillis += 120 * MIN; hear(*m2, 0x20); assert(get(*m2, 0x20)->count == 2);
    // corrupted file is rejected
    fakeFiles["/prefs/streetpass.bin"][20] ^= 0x55;
    auto *m3 = new StreetPassModule(); assert(m3->nodesMet() == 0);
    // eviction: fill the table, then the node heard longest ago is replaced
    for (uint32_t n = 1; n <= StreetPassModule::MAX_PASSES; n++) { fakeMillis += 1000; hear(*m3, 0x1000 + n); }
    assert(m3->nodesMet() == StreetPassModule::MAX_PASSES);
    fakeMillis += 1000; hear(*m3, 0x1000 + 1); // refresh the oldest, so node 2 is now the stalest
    fakeMillis += 1000; hear(*m3, 0x9999);
    assert(m3->nodesMet() == StreetPassModule::MAX_PASSES && get(*m3, 0x9999) && !get(*m3, 0x1002) && get(*m3, 0x1001));
    // millis rollover does not create a phantom encounter or lose one
    auto *m4 = new StreetPassModule(); fakeMillis = 0xFFFFFFF0u; hear(*m4, 0x77); uint16_t c = get(*m4, 0x77)->count;
    fakeMillis += 5 * MIN; hear(*m4, 0x77); assert(get(*m4, 0x77)->count == c);
    fakeMillis += 40 * MIN; fakeEpoch += 2700; hear(*m4, 0x77); assert(get(*m4, 0x77)->count == c + 1);
    printf("ALL TESTS PASSED\n");
}
