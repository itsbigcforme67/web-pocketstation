#!/bin/bash
# Desktop tests for the PocketStation module (no device or firmware source needed, just g++ and python3).
# Uses the BIOS and cards in esp32/data.      bash run.sh
set -e
cd "$(dirname "$0")"
MOD=../../PocketStation
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
mkdir -p "$B/s/concurrency" "$B/s/mesh" "$B/s/gps" "$B/s/graphics" "$B/s/input" "$B/s/freertos" "$B/data"
cp stubs/fake.h "$B/s/"
cp stubs/freertos/FreeRTOS.h "$B/s/freertos/"
echo '#include "FreeRTOS.h"' > "$B/s/freertos/semphr.h"
echo '#include "FreeRTOS.h"' > "$B/s/freertos/task.h"
# stand-ins for the firmware headers the module includes; they all lead to fake.h
for h in configuration.h DebugConfiguration.h FSCommon.h Observer.h PowerFSM.h SPILock.h SafeFile.h concurrency/LockGuard.h \
         concurrency/OSThread.h gps/RTC.h graphics/Screen.h graphics/ScreenFonts.h graphics/SharedUIDisplay.h input/InputBroker.h \
         main.h memGet.h mesh/MeshModule.h; do
  echo '#include "fake.h"' > "$B/s/$h"
done
python3 ../../pack.py "$B/data" > /dev/null
CXX="g++ -O2 -std=gnu++17 -Wno-unused-value -I$B/s -I$MOD"
$CXX -O1 -c "$B/data/psdata.cpp" -o "$B/psdata.o"
echo "1/3 emulator on the main loop, stepped clock"
$CXX test.cpp $MOD/pscore.cpp $MOD/cards.cpp "$B/psdata.o" -o "$B/coop" && "$B/coop" | tail -1
echo "2/3 emulator on its own thread, real time (about 30 s)"
$CXX -DTHREADED -DREAL_CLOCK test.cpp $MOD/pscore.cpp $MOD/cards.cpp "$B/psdata.o" -o "$B/thr" -lpthread && "$B/thr" | tail -1
echo "3/3 built without a data pack"
$CXX nopack.cpp $MOD/pscore.cpp $MOD/cards.cpp -o "$B/nopack" && "$B/nopack" | tail -1
