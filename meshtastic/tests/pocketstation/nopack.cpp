// Without psdata.cpp the module must still build and say what to do.
#include "fake.h"
#define private public
#define protected public
#include "PocketStation.cpp"
#include <cassert>
uint32_t fakeMs = 1000, fakeEpoch = 0; bool quietLog = false; int spiLock = 0;
Screen theScreen; Screen *screen = &theScreen; InputBroker theBroker; InputBroker *inputBroker = &theBroker;
FakeFsm powerFSM; MemGet memGet; std::map<std::string, std::string> fakeFiles; FakeFS FSCom;
int main() {
    auto *m = new PocketStationModule(); OLEDDisplay d;
    assert(m->packProblem && m->games.empty());
    InputEvent ev{"t", INPUT_BROKER_SELECT, 0, 0, 0};
    assert(inputBroker->notifyObservers(&ev) == 0 && m->state == LIST);
    m->runOnce(); m->drawFrame(&d, nullptr, 0, 0);
    assert(d.text.size() == 3 && d.text[1] == "No games built in.");
    printf("NO-PACK TEST PASSED\n");
}
