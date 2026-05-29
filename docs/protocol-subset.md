# tci-bridge Protocol Subset

`tci-bridge` implements a deliberately small TCI-compatible subset intended for traditional CAT + soundcard radios.

The goal is not to emulate a full SDR application. The goal is to expose enough TCI behavior for digital-mode applications such as JS8Call to use:

- frequency control
- mode control
- PTT
- RX audio streaming
- TX audio streaming

## Transport

TCI uses a WebSocket connection.

`tci-bridge` listens as the server. Clients connect to the configured address and port.

Example:

```text
ws://127.0.0.1:40001
```

Text commands are sent as WebSocket text messages.

Binary audio frames are sent as WebSocket binary messages.

## Text command format

Text commands use this general form:

```text
command:arg0,arg1,arg2;
```

The trailing semicolon is accepted but not strictly required by the parser. Responses and broadcasts always include the semicolon.

Command names are parsed case-insensitively.

## Startup messages

When a client connects, `tci-bridge` sends a small startup burst:

```text
protocol:ExpertSDR3 TCI;
version:1.9;
device:0,tci-bridge;
receive_only:false;
trx_count:1;
channels_count:1;
vfo_limits:0,0,100000,60000000;
if_limits:0,0,-48000,48000;
ready;
```

Then it sends current state:

```text
vfo:0,0,<frequency_hz>;
dds:0,0,<frequency_hz>;
modulation:0,0,<mode>;
trx:0,<true|false>;
```

## Radio model

Current model:

```text
transceiver index: 0
receiver index:    0
VFO index:         0
audio channel:     0
```

`tci-bridge` currently exposes one radio, one receiver, one VFO, and one audio stream.

Multi-VFO, split operation, subreceivers, IQ streams, panadapters, and waterfall streams are intentionally not implemented yet.

## Frequency commands

### Query frequency

Client:

```text
vfo;
```

or:

```text
vfo:0,0;
```

Server:

```text
vfo:0,0,<frequency_hz>;
```

### Set frequency

Client:

```text
vfo:0,0,<frequency_hz>;
```

Example:

```text
vfo:0,0,14074000;
```

Server echoes the command, then broadcasts state:

```text
vfo:0,0,14074000;
dds:0,0,14074000;
```

### `dds` alias

`tci-bridge` treats `dds` similarly to `vfo`.

Client:

```text
dds:0,0,7078000;
```

Server may broadcast both:

```text
vfo:0,0,7078000;
dds:0,0,7078000;
```

## Mode commands

### Query mode

Client:

```text
modulation;
```

Server:

```text
modulation:0,0,<mode>;
```

Example:

```text
modulation:0,0,usb;
```

### Set mode

Client:

```text
modulation:0,0,<mode>;
```

Example:

```text
modulation:0,0,digu;
```

Server echoes the command, then broadcasts:

```text
modulation:0,0,digu;
```

### Supported modes

The bridge passes modes through to the radio backend after normalization.

Known useful values:

```text
usb
lsb
digu
digl
cw
cwr
am
fm
rtty
rttyr
```

For the `rigctld` backend:

```text
digu -> PKTUSB
digl -> PKTLSB
```

and on readback:

```text
PKTUSB -> DIGU
PKTLSB -> DIGL
```

## PTT commands

### Query PTT

Client:

```text
trx;
```

Server:

```text
trx:0,<true|false>;
```

### Set PTT

Client:

```text
trx:0,true;
```

or:

```text
trx:0,false;
```

Accepted true-ish values:

```text
true
1
on
tx
```

All other values are treated as false.

Server responds with the actual resulting PTT state:

```text
trx:0,true;
```

or:

```text
trx:0,false;
```

The alias `ptt` is also accepted.

## Transmit safety

By default, `tci-bridge` refuses to assert PTT.

PTT control must be explicitly enabled in configuration:

```yaml
ptt:
  enable_transmit: true
```

or via command line:

```bash
./build/tci-bridge --config examples/tci-bridge.yml --enable-transmit
```

When transmit is disabled, commands such as:

```text
trx:0,true;
```

are rejected and the server responds with the actual state:

```text
trx:0,false;
```

The bridge supports a maximum TX watchdog:

```yaml
ptt:
  max_tx_ms: 30000
```

If PTT remains on longer than this limit, the bridge forces PTT off.

The bridge can also force PTT off when a client disconnects:

```yaml
ptt:
  unkey_on_disconnect: true
```

This is enabled by default.

Recommended live-radio configuration while testing:

```yaml
ptt:
  enable_transmit: false
  tx_audio_keys_ptt: false
  max_tx_ms: 30000
  unkey_on_disconnect: true
```

Recommended live-radio configuration once PTT testing is intentional:

```yaml
ptt:
  enable_transmit: true
  tx_audio_keys_ptt: false
  max_tx_ms: 30000
  unkey_on_disconnect: true
```

`tx_audio_keys_ptt` should normally remain disabled until TX audio is well tested.

## RX audio control

RX audio is not sent to a client until the client enables it.

### Set sample type

Client:

```text
audio_stream_sample_type:int16;
```

Server:

```text
audio_stream_sample_type:int16;
```

Only `int16` is currently supported at the TCI boundary.

### Set channel count

Client:

```text
audio_stream_channels:1;
```

Server:

```text
audio_stream_channels:1;
```

Only mono is currently supported at the TCI boundary.

### Set samples per frame

Client:

```text
audio_stream_samples:512;
```

Server:

```text
audio_stream_samples:512;
```

Currently accepted range:

```text
100..2048
```

Recommended value:

```text
512
```

### Set sample rate

Client:

```text
audio_samplerate:48000;
```

Server:

```text
audio_samplerate:48000;
```

Only `48000` Hz is currently expected.

### Start RX audio

Client:

```text
audio_start:0;
```

Server echoes:

```text
audio_start:0;
```

After this, the server sends WebSocket binary messages containing `RX_AUDIO_STREAM` frames.

### Stop RX audio

Client:

```text
audio_stop:0;
```

Server echoes:

```text
audio_stop:0;
```

After this, the server stops sending RX audio frames to that client.

## TX audio control

### Start TX audio

Client:

```text
tx_audio_start:0;
```

Server echoes:

```text
tx_audio_start:0;
```

If `ptt.tx_audio_keys_ptt` is enabled in configuration and transmit is enabled, this also asserts PTT.

### Stop TX audio

Client:

```text
tx_audio_stop:0;
```

Server echoes:

```text
tx_audio_stop:0;
```

If `ptt.tx_audio_keys_ptt` is enabled in configuration, this also clears PTT.

## Binary stream frame layout

`tci-bridge` uses a 64-byte stream header followed by raw PCM payload.

Header fields are 16 little-endian unsigned 32-bit integers:

```text
uint32 receiver
uint32 sample_rate
uint32 sample_type
uint32 codec
uint32 crc
uint32 sample_count
uint32 stream_type
uint32 channels
uint32 reserved_0
uint32 reserved_1
uint32 reserved_2
uint32 reserved_3
uint32 reserved_4
uint32 reserved_5
uint32 reserved_6
uint32 reserved_7
```

### Stream types

```text
0 = IQ_STREAM
1 = RX_AUDIO_STREAM
2 = TX_AUDIO_STREAM
3 = TX_CHRONO
4 = LINEOUT_STREAM
```

Currently implemented:

```text
1 = RX_AUDIO_STREAM
2 = TX_AUDIO_STREAM
```

### Sample types

```text
0 = INT16
1 = INT24
2 = INT32
3 = FLOAT32
```

Currently implemented at the TCI boundary:

```text
0 = INT16
```

### RX audio binary frame

Server to client:

```text
receiver:     0
sample_rate:  48000
sample_type:  0
codec:        0
crc:          0
sample_count: usually 512
stream_type:  1
channels:     1
reserved:     all 0
payload:      signed little-endian int16 mono PCM
```

For 512 samples:

```text
64-byte header + 1024-byte PCM payload = 1088-byte WebSocket binary message
```

### TX audio binary frame

Client to server:

```text
receiver:     0
sample_rate:  48000
sample_type:  0
codec:        0
crc:          0
sample_count: number of mono int16 samples
stream_type:  2
channels:     1
reserved:     all 0
payload:      signed little-endian int16 mono PCM
```

Unsupported TX frames are ignored with a warning.

Currently rejected:

```text
sample_rate != 48000
sample_type != INT16
channels != 1
stream_type != TX_AUDIO_STREAM
```

## Audio format policy

The TCI-side audio format is fixed for now:

```text
48000 Hz
mono
signed little-endian int16
```

The local Qt audio device may use another supported PCM sample format. The bridge converts between the local device format and TCI mono int16.

Supported local Qt sample formats:

```text
UInt8
Int16
Int32
Float
```

Supported local channel counts:

```text
1
2
```

Current limitation:

```text
No resampling yet.
```

If the selected audio device opens at something other than 48000 Hz, the current bridge may behave incorrectly. Prefer devices that support 48000 Hz.

## Backend behavior

### null backend

The `null` backend is a fake radio useful for testing the TCI server without Hamlib.

Default state:

```text
frequency: 14074000
mode:      USB
ptt:       false
```

### rigctld backend

The `rigctld` backend talks to Hamlib through `rigctld` TCP.

Used commands:

```text
f       get frequency
F <hz>  set frequency

m       get mode/passband
M <mode> 0
        set mode using default passband

t       get PTT
T 0|1   set PTT
```

The backend polls radio state and broadcasts changes to connected TCI clients.

## Unknown commands

Unknown text commands are logged and echoed.

This is intentional. Some TCI clients serialize command flow by waiting for a confirmation before sending the next command. Echoing unknown commands keeps discovery sessions alive while the bridge gains protocol coverage.

## Intentional omissions

Not implemented yet:

```text
IQ stream
spectrum stream
waterfall/panadapter
spots
CW macros
memory channels
filters
AGC
RF/AF gain
SQL
split
multiple VFOs
multiple receivers
transverter offsets
amplifier/tuner control
native rig discovery
authentication/TLS
```