#!/bin/sh
# Builds the test hosts natively and runs them against build/native/keysampler.so.
# Needs: cc, lilv (libjack not required), pkg-config. Output WAVs go to test/out/.
# host13 also needs python3 with grpcio + protobuf and a sushi_rpc descriptor
# (see test/README.md); it is skipped when SUSHI_DESC is not set.
set -e
cd "$(dirname "$0")/.."
sh build.sh >/dev/null
W=$(mktemp -d); U=$W/user; B=$U/plugins/lv2/keysampler.lv2
mkdir -p $B/samples test/out
cp build/native/keysampler.so src/keysampler.ttl src/manifest.ttl $B/
export LV2_PATH=$U/plugins/lv2 KEYSAMPLER_GRPC_PORT=${KEYSAMPLER_GRPC_PORT:-1}
URI=https://github.com/keysampler/KeySampler
CF="-Ithird_party/lv2/include $(pkg-config --cflags lilv-0)"; LF="$(pkg-config --libs lilv-0) -lm"
fail=0
run() { n=$1; shift
  cc $CF -o $W/$n test/$n.c $LF
  if "$W/$n" $URI "$@" >$W/$n.log 2>&1; then echo "ok    $n $*"; else echo "FAIL  $n $* (see $W/$n.log)"; fail=1; fi; }
run host5 test/out/record.wav record
echo 26 > $U/keysampler_recordings/last_number.txt   # next take = 27 (shown as 24), which comes back as 26.99999
ROUNDTRIP=1 RECDIR=$U/keysampler_recordings run host5 test/out/record_roundtrip.wav record
python3 -c "
import wave, numpy as np, sys
w = wave.open('test/out/record_roundtrip.wav'); x = np.frombuffer(w.readframes(w.getnframes()), np.int16)[::2] / 32768
sys.exit(0 if np.sqrt((x[int(2.2*48000):int(2.8*48000)]**2).mean()) > 0.05 else 1)" && echo "ok    recording plays after the S2400-style round trip" || { echo "FAIL  recording silent after the round trip"; fail=1; }
SAMPLE=4 SAMPLE_ID=1 run host5 test/out/reload.wav reload
# the S2400 may not echo Sample ID back: Sound = Sample must still play the take just recorded
NOECHO=1 run host5 test/out/record_noecho.wav record
python3 -c "
import wave, numpy as np, sys
w = wave.open('test/out/record_noecho.wav'); x = np.frombuffer(w.readframes(w.getnframes()), np.int16)[::2] / 32768
sys.exit(0 if np.sqrt((x[int(2.2*48000):int(2.8*48000)]**2).mean()) > 0.05 else 1)" && echo "ok    new take plays even if Sample ID comes back as 0" || { echo "FAIL  silent when Sample ID is not echoed"; fail=1; }
SAMPLE=5 run host5 test/out/reload_by_number.wav reload
run host6 test/out/pcr.wav
run host7 test/out/points.wav
run host8 test/out/glide.wav
run host9 test/out/osc.wav
run host10 test/out/slot64.wav
run host14 test/out/buttons.wav
python3 -c "
import numpy as np, wave
w = wave.open('$U/keysampler_recordings/sample_0001.wav', 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes((np.linspace(-0.8, 0.8, 24000) * 32767).astype(np.int16).tobytes())" 2>/dev/null || true
MODE=3 LSMS=100 LEMS=400 run host15 test/out/pingpong.wav
SAMPLE=3 FILTER=1 CUTOFF=40 RESO=0.9 run host16 test/out/ladder.wav
B4=1 SAMPLE=3 FILTER=1 CUTOFF=50 run host17 test/out/lfo_filter.wav
B4=2 SAMPLE=3 run host17 test/out/lfo_volume.wav
SAMPLE=5 CMDS="0.8:3,0.9:4,1.0:0" run host18 test/out/cmd_scroll.wav
[ -f $U/keysampler_recordings/sample_0001.wav ] && echo "ok    scrolling past the commands does nothing" || { echo "FAIL  scrolling deleted the file"; fail=1; }
SAMPLE=4 SAMPLE_ID=1 CMDS="0.8:4" run host18 test/out/cmd_delete_own.wav
[ -f $U/keysampler_recordings/sample_0001.wav ] && echo "ok    Changes > Delete does nothing while Sound = Sample" || { echo "FAIL  Delete removed the instance's own sample"; fail=1; }
SAMPLE=5 CMDS="0.8:4" run host18 test/out/cmd_delete.wav
[ ! -f $U/keysampler_recordings/sample_0001.wav ] && echo "ok    Sound = 1, Changes > Delete removes sample_0001.wav" || { echo "FAIL  Delete kept the file"; fail=1; }
# 0.20-0.31 named files after the Sample value; once, they are renamed to the number shown
R=$U/keysampler_recordings; rm -f $R/numbered_from_1 $R/sample_*.wav; cp test/out/reload.wav $R/sample_0030.wav
run host9 test/out/migrate.wav
[ -f $R/sample_0027.wav ] && [ ! -f $R/sample_0030.wav ] && [ -f $R/numbered_from_1 ] && echo "ok    old sample_0030.wav renamed to sample_0027.wav (menu shows 27)" || { echo "FAIL  renumbering"; ls $R; fail=1; }
# Sample follows the last number picked; deleting that one falls back to the one before
python3 -c "
import numpy as np, wave
for n, hz in ((1, 300), (2, 900)):
    w = wave.open('$R/sample_%04d.wav' % n, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes((0.5 * np.sin(2 * np.pi * hz * np.arange(96000) / 48000) * 32767).astype(np.int16).tobytes())"
hz() { python3 -c "
import wave, numpy as np
w = wave.open('$1'); x = np.frombuffer(w.readframes(w.getnframes()), np.int16)[::2].astype(float)
s = x[int(3.65*48000):int(3.85*48000)]; print(int(round(np.sum(np.abs(np.diff(np.sign(s)))) / 2 / 0.2 / 2)) if np.abs(s).max() > 1000 else 0)"; }
SAMPLE_ID=1 SEQ="0:6,1.5:4" CMDS="" run host18 test/out/adopt.wav
[ "$(hz test/out/adopt.wav)" -gt 800 ] && echo "ok    pick 2, back to Sample: Sample plays 2" || { echo "FAIL  Sample did not follow the picked number ($(hz test/out/adopt.wav) Hz)"; fail=1; }
SAMPLE_ID=1 SEQ="0:6" CMDS="1.0:4" run host18 test/out/adopt_delete.wav
F=$(hz test/out/adopt_delete.wav); [ "$F" -gt 200 ] && [ "$F" -lt 400 ] && [ ! -f $R/sample_0002.wav ] && echo "ok    delete 2: Sample goes back to 1" || { echo "FAIL  after deleting the picked number ($F Hz)"; fail=1; }
if [ -n "$SUSHI_DESC" ]; then
  python3 test/fake_sushi.py "$SUSHI_DESC" src/keysampler.ttl 51071 $W/sushi.log & SP=$!; sleep 2
  KEYSAMPLER_GRPC_PORT=51071 run host13 test/out/save_restore.wav
  kill $SP; echo "--- sushi log"; cat $W/sushi.log
fi
exit $fail
