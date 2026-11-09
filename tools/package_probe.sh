#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export PS3DEV="${PS3DEV:-$(cd ../build-tools/ps3dev && pwd)}"
export PSL1GHT="$PS3DEV"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin:$PATH"
make -j4 probe
mkdir -p build/probe-pkg/USRDIR/licenses
ppu-strip build/audio-probe.elf -o build/audio-probe-stripped.elf
sprxlinker build/audio-probe-stripped.elf
make_self_npdrm build/audio-probe-stripped.elf build/probe-pkg/USRDIR/EBOOT.BIN UP0001-ORB000002_00-0000000000000000
sfo --title "Orbit Audio Test" --appid ORB000002 -f assets/audio-sfo.xml build/probe-pkg/PARAM.SFO
cp build/icon.png build/probe-pkg/ICON0.PNG
cp assets/OrbitSans.ttf build/probe-pkg/USRDIR/OrbitSans.ttf
cp README-DE.md build/probe-pkg/USRDIR/README-DE.txt
cp THIRD-PARTY.md build/probe-pkg/USRDIR/THIRD-PARTY.txt
cp LICENSE build/probe-pkg/USRDIR/LICENSE.txt
cp vendor/*LICENSE* build/probe-pkg/USRDIR/licenses/
pkg --contentid UP0001-ORB000002_00-0000000000000000 build/probe-pkg build/Orbit-Audio-Test-0.2.pkg
package_finalize build/Orbit-Audio-Test-0.2.pkg
python3 tools/verify_package.py --probe
