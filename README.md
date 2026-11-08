# tci-bridge

`tci-bridge` is a standalone bridge that exposes a practical subset of the ExpertSDR-style TCI WebSocket protocol and connects it to:

- Hamlib `rigctld` for CAT/PTT/radio control
- QtMultimedia audio devices for RX and TX audio

It was developed to allow JS8Call TCI operation with conventional radios, external audio devices, and Hamlib-supported rigs.

## Protocol support

The bridge implements a hardware-radio subset of TCI 2.1 over WebSocket.
It supports CAT control, RX and TX audio, and optional radio controls and
meters where the Hamlib driver supplies the required data. The
[hardware-radio profile](docs/protocol-subset.md) lists the commands,
audio formats, driver-dependent behavior, and extensions.

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

The bridge starts chrono after rigctld confirms key-down and the audio
sink opens. The TX owner's negotiated rate, format, channel count and scalar
length determine chrono requests. The bridge accepts uncompressed PCM at 8,
12, 24 or 48 kHz in INT16, INT24, INT32 or FLOAT32 with one or two channels,
converting to 48 kHz mono INT16 for the Qt backend. The supported commands and
compatibility behavior are documented in the [hardware-radio profile](docs/protocol-subset.md).

## TX safety

Transmit is disabled by default.

To allow transmit:

```yaml
ptt:
  enable_transmit: true
```

Safety features:

- TX watchdog with a configurable maximum duration measured from the key request
- Optional unkey on client disconnect
- TX ownership enforcement
- Separate TX audio ownership
- Radio polling suspension during TX audio
- Clean SIGINT/SIGTERM shutdown handling
- Forced unkey on shutdown/watchdog where appropriate

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
### Audio format and resampling

The bridge uses 48 kHz mono PCM internally. The per-client audio settings
control RX frames and the format requested by `TX_CHRONO`. Valid incoming
TX frames may use any supported wire format. RX decimates from 48 kHz for
lower-rate subscriptions, and TX interpolates lower-rate input before
playback. See the [audio command reference](docs/protocol-subset.md#audio).

### Startup capability declarations

The startup burst reports the TCI version, bridge device identifier,
effective `receive_only` permission, and one receiver/one channel topology.
When TX permission changes at runtime, the bridge broadcasts the updated
`receive_only` state.

The rigctld backend now queries Hamlib's documented `M ?` operation
once after the radio becomes usable. It advertises `modulations_list`
only if a complete recognizable mode list is returned, including a successful
`RPRT 0` status. Discovery is asynchronous. Already-connected TCI clients receive the mode list when
it arrives, while later clients receive it in their startup burst.
No vendor-specific TCI negotiation has been introduced. VFO limits are discovered from the RX ranges in Hamlib's `dump_state`
response when they form a single continuous tuning interval. Discontinuous
or malformed ranges are not advertised. IF limits remain omitted because
the bridge does not implement an SDR IF/panorama tuning stage.


### TCI modulation names and Hamlib modes

TCI 2.1 describes `MODULATIONS_LIST` as a list of modulation **names**;
it does not define a closed enumeration of permitted values. The bridge
therefore preserves separate radio-specific mode names (including
`FM-D`, `FMN`, `AMN`, and `PKTFMN`) rather than projecting them
onto a narrower guessed TCI enum. It maps only Hamlib `PKTUSB` to
`DIGU` and `PKTLSB` to `DIGL`, in both command and notification
directions. In particular, `FMN` is not silently rewritten as
`NFM`: although `NFM` occurs in the TCI examples, the specification
does not establish an alias relationship with Hamlib's `FMN`.

The supported-mode parser checks the complete successful rigctld
`M ?` response and accepts well-formed future mode identifiers without
maintaining a fixed hard-coded allowlist. Mode selection is still
ultimately accepted or rejected by rigctld/the radio.

### Automatic VFO tuning limits

A separate, short-lived rigctld TCP connection queries `dump_state` once
after the radio becomes usable. The parser checks the zero-terminated RX
range list and the subsequent TX range delimiter, and only advertises
`vfo_limits:<minimum>,<maximum>;` for contiguous RX coverage. Discovery is read-only and does not
require a radio model ID, CLI option, or appliance configuration. An
unavailable or unsupported dump leaves the limits unadvertised. The
bridge deliberately does not claim that the RX tuning envelope is a
continuous legal TX range.

### TX output stream lifecycle

The bridge selects the TX audio device at startup, but opens a fresh
`QAudioSink` only when TCI audio TX begins and closes it after TX ends.
This prevents a long-idle Qt/PipeWire output stream from being reused on
subsequent transmissions. The existing prebuffer/jitter buffering remains
unchanged. Each TX start logs the requested/actual sink buffer size,
`bytesFree`, and initial Qt audio state. Each TX end logs underrun and
overflow counts and verifies that the sink was closed. If a new output
stream cannot open, the bridge queues a forced PTT unkey for safety.

The initial `TX_CHRONO` is sent after the sink opens and CAT confirms PTT.
