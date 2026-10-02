#!/bin/bash
# Desktop logic test for the StreetPass module (no device or firmware source needed, just g++).
#   bash run.sh
cd "$(dirname "$0")"
B=$(mktemp -d)
mkdir -p "$B/concurrency" "$B/mesh" "$B/gps"
cp stubs/fake.h "$B/"
# stand-ins for the firmware headers the module includes; they all lead to fake.h
for h in configuration.h Observer.h concurrency/OSThread.h concurrency/LockGuard.h mesh/MeshModule.h mesh/NodeDB.h \
         mesh/Throttle.h gps/RTC.h DebugConfiguration.h FSCommon.h SPILock.h SafeFile.h main.h ErriezCRC32.h; do
  echo '#include "fake.h"' > "$B/$h"
done
g++ -std=gnu++17 -Wall -I"$B" -I../../StreetPass test.cpp ../../StreetPass/StreetPass.cpp -o "$B/t" && "$B/t" | tail -1
rm -rf "$B"
