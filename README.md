# tci-bridge

`tci-bridge` is a standalone bridge that exposes a practical subset of the ExpertSDR-style TCI WebSocket protocol and connects it to:

- Hamlib `rigctld` for CAT/PTT/radio control
- QtMultimedia audio devices for RX and TX audio

It was developed to allow JS8Call TCI operation with conventional radios, external audio devices, and Hamlib-supported rigs.

## Status

Currently supported:

- TCI WebSocket server
- RX audio stream to TCI clients
- TX audio stream from TCI clients
- TCI 2.1 TX_CHRONO server-side pacing, with permissive receipt of unsolicited valid TX_AUDIO_STREAM frames from the active TX owner
- VFO frequency
- TX VFO frequency shadowing / best-effort rigctld support
- Split enable/disable
- Mode
- PTT
- TX watchdog
- TX audio ownership
- Optional TX-audio-keys-PTT mode
- Rigctld worker thread so slow CAT operations do not block audio/WebSocket timing
- TX audio jitter buffering
- Clean TX audio lifecycle events
- SIGINT/SIGTERM shutdown handling

The bridge intentionally implements only the TCI protocol subset needed for JS8Call-style operation.

## Requirements

Runtime:

- Qt 6 Core
- Qt 6 Network
- Qt 6 WebSockets
- Qt 6 Multimedia
- yaml-cpp
- Hamlib `rigctld`, unless using the `null` radio backend

Build:

- CMake
- C++17 compiler
- Qt 6 development packages
- yaml-cpp development package

## Build

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j
```

The binary will be built as:

```bash
cmake-build-release/tci-bridge
```

## List audio devices

```bash
./cmake-build-release/tci-bridge --list-audio-devices
```

Use the printed device descriptions in the config file. Partial matches are supported.

## Configuration

Example config:

```yaml
server:
  bind: 127.0.0.1
  port: 40001
  debug: false

radio:
  backend: rigctld
  rigctld_host: 127.0.0.1
  rigctld_port: 4532
  poll_ms: 250
  debug: false

audio:
  rx_device: ""
  tx_device: ""
  debug: false
  tx_sink_buffer_ms: 300
  tx_prebuffer_ms: 200
  tx_jitter_buffer_ms: 5000
  tx_drain_interval_ms: 2

ptt:
  enable_transmit: false
  max_tx_ms: 30000
  unkey_on_disconnect: true

logging:
  quiet: true
  startup_config: true
  tx_timing: false
```

Recommended system config location:

```bash
/etc/tci-bridge.yml
```

## Running manually

```bash
./cmake-build-release/tci-bridge --config examples/tci-bridge.yml
```

Transmit control is disabled by default. To allow the bridge to assert PTT:

```yaml
ptt:
  enable_transmit: true
```

or:

```bash
./cmake-build-release/tci-bridge --config examples/tci-bridge.yml --enable-transmit
```

## rigctld setup

Start `rigctld` separately. Example:

```bash
rigctld -m <radio_model_id> -r <device> -s <baud> -t 4532
```

Then configure:

```yaml
radio:
  backend: rigctld
  rigctld_host: 127.0.0.1
  rigctld_port: 4532
```

The rigctld backend runs radio I/O and polling in a worker thread so slow CAT calls do not block WebSocket or audio timing.

## JS8Call setup

Configure JS8Call to connect to the bridge TCI server:

```text
Host: 127.0.0.1
Port: 40001
```

Use TCI for CAT and audio according to the JS8Call TCI integration settings.

## Audio setup

List available devices:

```bash
tci-bridge --list-audio-devices
```

Then configure:

```yaml
audio:
  rx_device: "partial or full RX device name"
  tx_device: "partial or full TX device name"
```

If left blank, Qt’s default audio input/output devices are used.

TX buffering defaults:

```yaml
audio:
  tx_sink_buffer_ms: 300
  tx_prebuffer_ms: 200
  tx_jitter_buffer_ms: 5000
  tx_drain_interval_ms: 2
```

If TX audio glitches, try increasing:

```yaml
audio:
  tx_sink_buffer_ms: 500
  tx_prebuffer_ms: 300
  tx_jitter_buffer_ms: 8000
```

## Running as a systemd service

Example files are provided under:

```text
examples/
├── tci-bridge.yml
└── systemd/
    └── tci-bridge.service
```

Manual install example:

```bash
sudo cp cmake-build-release/tci-bridge /usr/local/bin/tci-bridge
sudo cp examples/tci-bridge.yml /etc/tci-bridge.yml
sudo cp examples/systemd/tci-bridge.service /etc/systemd/system/tci-bridge.service

sudo systemctl daemon-reload
sudo systemctl enable --now tci-bridge.service
```

View logs:

```bash
journalctl -u tci-bridge.service -f
```

Stop service:

```bash
sudo systemctl stop tci-bridge.service
```

## TCI TX timing compatibility

The server sends TCI 2.1 `TX_CHRONO` pacing frames when the active
transmitting client selects TCI audio (`trx:0,true,tci;`). It also accepts
valid `TX_AUDIO_STREAM` frames from that same TX owner even when they
were not preceded by a chrono request. No proprietary capability negotiation
or global TX timing flag is required.

This permissive handling helps legacy push-style TCI clients, but does not
guarantee compatibility with every older TCI implementation. The server
accepts uncompressed 48 kHz TX PCM in int16, int24, int32, or float32,
with one or two channels, converting to mono int16 before passing audio
to the Qt output backend. Stereo input is downmixed; other sample rates
and compressed codecs are not supported yet. The stream length must match
the payload's number of sample values across all channels. PTT ownership,
TX-audio source selection and the TX watchdog remain enforced.

## TX safety

Transmit is disabled by default.

To allow transmit:

```yaml
ptt:
  enable_transmit: true
```

Safety features:

- TX watchdog with configurable maximum TX duration
- Optional unkey on client disconnect
- TX ownership enforcement
- Separate TX audio ownership
- Radio polling suspension during TX audio
- Clean SIGINT/SIGTERM shutdown handling
- Forced unkey on shutdown/watchdog where appropriate

Recommended first RF test:

- dummy load
- low RF power
- conservative radio audio input gain
- no or minimal ALC movement
- conservative JS8Call TX audio level

## Troubleshooting

### No RX waterfall or decodes

Check:

- selected RX audio device
- JS8Call TCI host/port
- bridge logs show `audio_start`
- bridge logs show RX audio device started

Run:

```bash
tci-bridge --list-audio-devices
```

### TX audio is choppy or distorted

Check logs for:

```text
TX push buffer overflow
TX audio underrun
slow call
```

If using `rigctld`, confirm the worker-thread backend is active and that polling is suspended during TX audio.

Try increasing:

```yaml
audio:
  tx_sink_buffer_ms: 500
  tx_prebuffer_ms: 300
  tx_jitter_buffer_ms: 8000
```

### Transmit does not key the radio

Check:

```yaml
ptt:
  enable_transmit: true
```

Also verify `rigctld` accepts PTT commands:

```bash
rigctl -m 2 -r localhost:4532
T 1
T 0
```

### rigctld returns `RPRT -20`

That usually means the rig/backend does not support the requested command in the current mode or VFO state. The bridge treats PTT as logical bridge state immediately, but hardware failures are still logged.

### Audio device not found

Device names are matched exactly first, then by case-insensitive partial match.

```bash
tci-bridge --list-audio-devices
```

## Install targets

If built with install rules enabled by the project CMake file:

```bash
cmake --install cmake-build-release
```

Typical installed paths:

```text
/usr/local/bin/tci-bridge
/usr/local/share/tci-bridge/examples/tci-bridge.yml
/usr/local/share/tci-bridge/examples/systemd/tci-bridge.service
```
TCI 2.1 RX audio defaults to 48 kHz, float32, two channels, and 2048
scalar sample values per frame (`Stream.length`), as documented for
48 kHz in the protocol. Clients may change the RX sample format,
channel count, and packet size using the standard audio-stream commands.
When requesting two RX channels, the bridge currently duplicates its
mono audio input into left and right channels. This is not equivalent
to true complex I/Q audio in DIGL/DIGU modes, for which an appropriate
radio backend would be needed.

TX_CHRONO continues to request 48 kHz mono float32, 512 samples per
request, independently of the RX packet-size default. In addition to
48-kHz TX, the bridge accepts 8-, 12-, and 24-kHz uncompressed TX packets
from the active TX owner. Those packets are converted to 48-kHz mono
int16 with a stateful, filtered interpolator before reaching Qt audio.
The native 48-kHz TX path remains byte-for-byte unchanged. Complex RX
audio is not implemented.

TCI RX sample-rate requests now support 8, 12, 24, and 48 kHz per
connection using a stateful, filtered decimator from the 48-kHz capture
audio. The resampler preserves phase/history across incoming audio chunks
and resets when an RX stream is stopped/restarted or its sample rate changes.
The radio-side TX audio output remains 48 kHz, with lower-rate TX
PCM upsampled before playback. TX resampler history resets when TX audio
ownership begins or ends, and on a change in incoming sample rate.

### Startup capability declarations

The startup burst reports the TCI version, bridge device identifier,
effective `receive_only` permission, and one receiver/one channel topology.
When TX permission changes at runtime, the bridge broadcasts the updated
`receive_only` state.

The rigctld radio backend currently exposes live frequency, mode, and PTT
state but **not** authoritative supported frequency limits, IF/panorama
limits, or the rig's full supported-mode set. Accordingly, the bridge does
not send `vfo_limits`, `if_limits`, or `modulations_list` placeholders.
It would be misleading to claim those values on behalf of an arbitrary
Hamlib radio. A future radio capability API can provide them, but they
must be obtained or configured from verified per-rig information.

The rigctld backend now queries Hamlib's documented `M ?` operation
once after the radio becomes usable. It advertises `modulations_list`
only if a complete recognizable mode list is returned, including a successful
`RPRT 0` status. The Yaesu FTDX-10's FM-D, FMN, AMN, and PKTFMN tokens
are recognized and retained as distinct modes rather than folded into FM. Discovery is
asynchronous; already-connected TCI clients receive the mode list when
it arrives, while later clients receive it in their startup burst.
No vendor-specific TCI negotiation has been introduced. Frequency and
IF limits remain omitted because they are not yet available through a
verified structured capability API.
