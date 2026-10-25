#!/bin/sh
# Builds the test hosts natively and runs them against build/native/keysampler.so.
# Needs: cc, lilv (libjack not required), pkg-config. Output WAVs go to test/out/.
# host12 also needs python3 with grpcio + protobuf and a sushi_rpc descriptor
# (see test/README.md); it is skipped when SUSHI_DESC is not set.
set -e
cd "$(dirname "$0")/.."
sh build.sh >/dev/null
W=$(mktemp -d); U=$W/user; B=$U/plugins/lv2/keysampler.lv2
mkdir -p $B/samples test/out
cp build/native/keysampler.so src/keysampler.ttl src/manifest.ttl $B/
cp test/samples/A440.wav $B/samples/00test.wav
export LV2_PATH=$U/plugins/lv2 KEYSAMPLER_GRPC_PORT=${KEYSAMPLER_GRPC_PORT:-1}
URI=https://github.com/keysampler/KeySampler
CF="-Ithird_party/lv2/include $(pkg-config --cflags lilv-0)"; LF="$(pkg-config --libs lilv-0) -lm"
fail=0
run() { n=$1; shift
  cc $CF -o $W/$n test/$n.c $LF
  if "$W/$n" $URI "$@" >$W/$n.log 2>&1; then echo "ok    $n $*"; else echo "FAIL  $n $* (see $W/$n.log)"; fail=1; fi; }
run host5 test/out/record.wav record
run host5 test/out/reload.wav reload
run host6 test/out/pcr.wav
run host7 test/out/points.wav
run host8 test/out/glide.wav
run host9 test/out/osc.wav
run host10 test/out/slot64.wav
run host11 test/out/buttons.wav
if [ -n "$SUSHI_DESC" ]; then
  python3 test/fake_sushi.py "$SUSHI_DESC" src/keysampler.ttl 51071 $W/sushi.log & SP=$!; sleep 2
  KEYSAMPLER_GRPC_PORT=51071 run host12 test/out/save_restore.wav
  kill $SP; echo "--- sushi log"; cat $W/sushi.log
fi
exit $fail
