# Tests

`sh test/run_tests.sh` builds the plugin and the test hosts natively (needs `cc`,
`pkg-config` and lilv development files, e.g. `apt install liblilv-dev`) and runs:

| Host | Checks |
| --- | --- |
| host5 `record` / `reload` | recording from the bus input, reloading the slot |
| host6 | every message the PCR-M30 GM2 set sends |
| host7 | start / loop point parameters and controllers |
| host8 | portamento and legato |
| host9, host10 | oscillator slots 61–64 and the filter envelope |
| host11 | buttons B1–B4 |
| host12 | B5 Save to Menu / B6 Restore Menu (needs the fake sushi server) |

Rendered audio is written to `test/out/` for listening or analysis.

## Save to Menu test (host12)

`fake_sushi.py` imitates the gRPC control API of [Elk Audio's sushi](https://github.com/elk-audio/sushi),
the audio host on the DSP Card. It needs `pip install grpcio protobuf` and a compiled
descriptor of sushi's `sushi_rpc.proto` (from the sushi repository, matching the
card's sushi version):

```sh
protoc --include_imports -o sushi_rpc.desc sushi_rpc.proto
SUSHI_DESC=sushi_rpc.desc sh test/run_tests.sh
```

The log should show `SetParameterValue` calls only after B5.

## On-device testing

The hosts can also be cross-built for aarch64 and run under `qemu-aarch64` against the
DSP Card's own `liblilv` to reproduce the card's environment exactly. Firmware files are
not part of this repository.

Older hosts (`host.c`, `host3.c`, `host4.c`, `grpc_probe*.c`) are kept for reference.
