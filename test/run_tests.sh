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
# four LFOs; B4 pitch, B5 filter, B6 pan select which one R3-R5 set (same button again = volume)
lfo() { n=$1; st=$2; ex=$3; shift 3; (export "$@" SAMPLE=3 FILTER=1 CUTOFF=60; run host17 test/out/$n.wav) >/dev/null
  python3 test/lfo_check.py test/out/$n.wav $st $ex >$W/$n.chk && echo "ok    $n moves $ex ($(head -1 $W/$n.chk))" || { echo "FAIL  $n: expected $ex, $(cat $W/$n.chk)"; fail=1; }; }
lfo lfo_none 0.1 none LFO_NONE=1
lfo lfo_menu_vol 0.1 vol LFO=vol
lfo lfo_menu_flt 0.1 flt LFO=flt
lfo lfo_menu_pit 0.1 pit LFO=pit
lfo lfo_menu_pan 0.1 pan LFO=pan
lfo lfo_knobs_vol 0.4 vol PRESS=-
lfo lfo_b4_pitch 0.4 pit PRESS=4
lfo lfo_b5_filter 0.4 flt PRESS=5
lfo lfo_b6_pan 0.4 pan PRESS=6
lfo lfo_b4_twice 0.65 vol PRESS=44
lfo lfo_b4_then_b6 0.65 pan PRESS=46
lfo lfo_cc87_pan 0.4 pan PRESS=p
lfo lfo_menu_mod_pitch 0.4 pit PRESS=- MODU=2
lfo lfo_menu_mod_filter 0.4 flt PRESS=- MODU=1
lfo lfo_menu_mod_pan 0.4 pan PRESS=- MODU=3
lfo lfo_menu_pan_then_b6 0.4 vol PRESS=6 MODU=3
lfo lfo_ui_vol 0.1 vol UI=1 MODU=0
lfo lfo_ui_pan 0.1 pan UI=1 MODU=3
lfo lfo_ui_pitch 0.1 pit UI=1 MODU=2
lfo lfo_ui_filter 0.1 flt UI=1 MODU=1
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
# MIDI Channel: Omni plays everything; 2 ignores channel 1 and plays channel 2
peak() { python3 -c "
import wave, numpy as np
w = wave.open('$1'); x = np.frombuffer(w.readframes(w.getnframes()), np.int16)
print(int(abs(x[int(0.25*96000):int(0.55*96000)]).max()))"; }
SAMPLE=0 CMDS="" SET="0:midi_ch:2" run host18 test/out/ch_other.wav
SAMPLE=0 CMDS="" SET="0:midi_ch:2" MIDI="0.21:91 40 7f;0.58:81 40 00" run host18 test/out/ch_match.wav
SAMPLE=0 CMDS="" run host18 test/out/ch_omni.wav
[ "$(peak test/out/ch_other.wav)" -lt 100 ] && [ "$(peak test/out/ch_match.wav)" -gt 3000 ] && [ "$(peak test/out/ch_omni.wav)" -gt 3000 ] \
  && echo "ok    MIDI Channel 2 ignores channel 1, plays channel 2; Omni plays all" || { echo "FAIL  MIDI Channel"; fail=1; }
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
  # new PCR layout, checked by what Save writes to the menu: B5 + S1 = filter attack, R6 = Glide ms,
  # B4 + R4 = pitch LFO depth, B4 again + R4 = volume LFO depth, L1 = nothing
  python3 test/fake_sushi.py "$SUSHI_DESC" src/keysampler.ttl 51074 $W/sushi2.log & SP=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do grep -q listening $W/sushi2.log 2>/dev/null && break; sleep 0.5; done
  rm -f $R/sample_*.wav
  KEYSAMPLER_GRPC_PORT=51074 SAMPLE=0 CMDS="2.0:1" MIDI="0.70:b0 7e 01;0.72:b0 05 7f;0.90:b0 65 00;0.91:b0 64 02;0.92:b0 06 64;1.10:b0 7f 00;1.12:b0 4d 7f;1.30:b0 7f 00;1.32:b0 4d 40;1.40:b0 50 7f;1.45:b0 50 00" run host18 test/out/layout.wav
  kill $SP
  L=$W/sushi2.log; has() { grep -q "Set proc=7 $1 " $L; }
  # menu LFO items: an edit goes to the LFO Modulation selects; switching Modulation shows the other LFO
  python3 test/fake_sushi.py "$SUSHI_DESC" src/keysampler.ttl 51075 $W/sushi3.log & SP=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do grep -q listening $W/sushi3.log 2>/dev/null && break; sleep 0.5; done
  KEYSAMPLER_GRPC_PORT=51075 SAMPLE=0 CMDS="" SET="0:modulation:3,0.8:lfo_depth:60,1.6:modulation:2" run host18 test/out/ui.wav
  kill $SP
  grep -q "lfo_pan_depth norm=[0-9.]* domain=60" $W/sushi3.log && grep -q "lfo_depth norm=[0-9.]* domain=0.000" $W/sushi3.log && ! grep -q "lfo_vol_depth\|lfo_pit_depth" $W/sushi3.log \
    && echo "ok    menu LFO Depth on Pan sets the pan LFO; switching to Pitch shows the pitch LFO" || { echo "FAIL  menu LFO items"; cat $W/sushi3.log; fail=1; }
  has fenv_attack && has glide && has lfo_pit_depth && has lfo_vol_depth && ! has attack && ! has record && ! ls $R/sample_*.wav >/dev/null 2>&1 \
    && echo "ok    B5+S1 filter attack, R6 glide, B4+R4 pitch LFO, B4 again volume LFO, L1 unused" || { echo "FAIL  PCR layout"; cat $L; fail=1; }
  python3 test/fake_sushi.py "$SUSHI_DESC" src/keysampler.ttl 51071 $W/sushi.log & SP=$!; sleep 2
  KEYSAMPLER_GRPC_PORT=51071 run host13 test/out/save_restore.wav
  kill $SP; echo "--- sushi log"; cat $W/sushi.log
fi
exit $fail
