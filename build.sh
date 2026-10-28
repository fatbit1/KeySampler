#!/bin/sh
# Builds keysampler.so for the S2400 DSP Card (aarch64, glibc 2.37) and for the
# local machine (used only for testing).
# Cross compiler: zig (pip install ziglang) or aarch64-linux-gnu-gcc.
set -e
cd "$(dirname "$0")"
LV2INC=${LV2INC:-third_party/lv2/include}
CFLAGS="-O2 -s -fPIC -shared -fvisibility=hidden -Wall -Wextra -I$LV2INC"

mkdir -p build/aarch64 build/native
# zig is preferred: it links against glibc 2.17 symbols, so the .so loads on the
# DSP Card (glibc 2.37). Distro cross-gccs often link newer symbols that won't load.
if python3 -m ziglang version >/dev/null 2>&1; then
  python3 -m ziglang cc -target aarch64-linux-gnu.2.17 -mcpu=cortex_a53 $CFLAGS \
    -o build/aarch64/keysampler.so src/keysampler.c -lm -lpthread
else
  echo "zig not found (pip install ziglang); falling back to aarch64-linux-gnu-gcc" >&2
  aarch64-linux-gnu-gcc $CFLAGS -mcpu=cortex-a53 -o build/aarch64/keysampler.so src/keysampler.c -lm -lpthread
fi
if readelf -V build/aarch64/keysampler.so | grep -qE "GLIBC_2\.(3[89]|[4-9][0-9])"; then
  echo "WARNING: build needs glibc newer than 2.37 and will NOT load on the DSP Card" >&2
fi
cc $CFLAGS -o build/native/keysampler.so src/keysampler.c -lm -lpthread
echo "built build/aarch64/keysampler.so and build/native/keysampler.so"

# assemble the ready-to-copy bundle
B=build/dspcard/plugins/lv2/keysampler.lv2
rm -rf build/dspcard && mkdir -p $B/samples
cp build/aarch64/keysampler.so $B/keysampler.so
cp src/keysampler.ttl $B/keysampler.ttl
cp src/manifest.ttl $B/
echo "bundle: $B"

# the same files, kept in the repository: sdcard/ is ready to copy to the S2400's SD card
S=sdcard/dspcard/plugins/lv2/keysampler.lv2
mkdir -p $S
cp $B/keysampler.so $B/keysampler.ttl $B/manifest.ttl $S/
echo "sd card copy: $S"
