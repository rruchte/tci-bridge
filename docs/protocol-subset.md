# TCI 2.1 hardware-radio profile

The bridge exposes one CAT-controlled radio and one PCM capture source over WebSocket. Receiver, transceiver and audio stream indices are zero. The profile targets applications that use CAT and audio for digital modes.

## Startup and control

A new connection receives `protocol:tci-bridge,2.1;`, `device:tci-bridge;`, `receive_only`, `trx_count:1;`, `channels_count:1;`, discovered capabilities when available, and `ready;`. Current state follows. The bridge announces `MODULATIONS_LIST` when it has a supported-mode list and `VFO_LIMITS` when the rig reports a contiguous RX tuning range. Capability discovery can finish after a client connects, in which case the bridge broadcasts the results.

| Command | Behavior |
| --- | --- |
| `VFO:0,0[,hz];` | Reads or sets RX frequency. |
| `VFO:0,1[,hz];` | Reads or sets Hamlib’s split TX frequency. A read falls back to the RX frequency when the split frequency is unknown. Channel 1 represents the split TX VFO. |
| `DDS:0[,hz];` | Compatibility alias for RX frequency. |
| `MODULATION:0[,mode];` | Reads or sets mode. Hamlib packet-sideband modes map to DIGU/DIGL. |
| `SPLIT_ENABLE:0[,true/false];` | Reads or controls Hamlib split using VFO B. Actual split behavior depends on the rig and Hamlib driver. |
| `TRX:0[,true/false[,tci]];` | Reads or controls PTT. The only explicit source accepted is `tci`, which selects WebSocket TX audio. With no source, the radio uses its configured input. Also accepts a receiverless query and the aliases `1/on/tx` and `0/off/rx` for the boolean argument. |
| `TX_FREQUENCY:<hz>;` | Sent on connection and when the known effective frequency changes. Uses RX frequency in simplex and the discovered TX VFO frequency in split. Omitted when CAT or split state is unavailable. Includes the XIT offset when enabled and confirmed by CAT. |
| `RIT_ENABLE:0[,true/false];`, `XIT_ENABLE:0[,true/false];` | Reads or sets the separate RX and TX clarifier switches. |
| `RIT_OFFSET:0[,hz];`, `XIT_OFFSET:0[,hz];` | Reads or sets a signed Hz offset. Zero clears the offset without changing the enable flag. Rigs with a shared clarifier may return the same offset for both. |
| `TX_SENSORS_ENABLE:<true/false>[,interval_ms];` | Subscribes the connection to `TX_SENSORS` readings during confirmed transmit. Defaults to 250 ms, with requested intervals from 30 to 1000 ms. Actual cadence is limited by CAT polling. |
| `RX_SENSORS_ENABLE:<true/false>[,interval_ms];` | Subscribes the connection to changed `RX_SENSORS` and channel A `RX_CHANNEL_SENSORS` readings during receive. Defaults to 250 ms, with requested intervals from 30 to 1000 ms. Actual cadence is limited by CAT polling. |
| `RX_FILTER_BAND:0[,low,high];` | Reads or sets signed receive-filter edges relative to the carrier in USB or LSB. Requires explicit opt-in and a driver exposing both Hamlib slope values in Hz. |
| `RX_NB_ENABLE:0[,true/false];`, `RX_NR_ENABLE:0[,true/false];` | Reads or sets the noise blanker and noise reduction switches independently through Hamlib. Writes run during receive and broadcasts use confirmed readback. |
| `AGC_MODE:0[,normal/fast/off];` | Reads or sets receiver AGC through Hamlib. Auto, medium and slow map to normal. Fast and superfast map to fast. User-defined or unknown modes are omitted. Writes are accepted only in RX. |
| `DRIVE:0[,percent];` | Reads or sets the rig’s configured RF power level as 0–100 via Hamlib RFPOWER. Confirmed readback can differ from the request. Queries request a fresh CAT reading in RX and use cached state during TX. Writes are accepted only in RX. |
| `TX_ENABLE:0;` | Reports permission to request TX based on bridge configuration, CAT availability and unkey recovery. Hamlib and the radio remain responsible for accepting PTT on the selected frequency. |
| `START;`, `STOP;` | START refreshes state. STOP has no hardware power action. |
| `CLOSE;` | Closes the requesting WebSocket. |
| `PTT` | Alias for `TRX`. |

Commands are case-insensitive. Unknown commands are ignored on the wire. CAT writes update the shared cache and broadcast only after rigctld acknowledges them. Physical VFO, mode and PTT changes are observed via polling. Clarifier settings are polled during RX and echoed after CAT readback. Unsupported settings are omitted. Clarifier writes are ignored during TX. DRIVE reports the configured level and can be updated by front-panel changes. TUNE_DRIVE requires a separate rig control and is unsupported. During RX, the bridge also checks split and the active split TX frequency about once a second. After a failed PTT command or lost CAT connection, the bridge can only confirm unkey once CAT succeeds again.

TX requires explicit enablement. The same connection must own PTT and select TCI audio to supply TX samples. The maximum-duration watchdog starts with the bridge's key request and continues independently of the audio traffic.

## Audio

- `AUDIO_START:0;` and `AUDIO_STOP:0;` control RX delivery for the sending connection. These commands have no text acknowledgement. The owner selects TX audio with `TRX:0,true,tci;`.
- Each of the audio settings below takes one value to set, or no arguments to query. Replies report the effective setting for that connection. The TX audio owner can query settings during its transmission and change them after it ends.
- `AUDIO_SAMPLERATE` supports 8, 12, 24 and 48 kHz. `AUDIO_STREAM_SAMPLE_TYPE` supports INT16, INT24, INT32 and FLOAT32. `AUDIO_STREAM_CHANNELS` supports one or two channels. RX stereo duplicates the same real PCM into both channels. TX stereo is mixed to mono.
- `AUDIO_STREAM_SAMPLES` accepts 100–2048 scalar values per packet. The implicit lengths at 8/12/24/48 kHz are 256/512/1024/2048. Explicit lengths survive sample-rate changes and are rounded down to an even value for stereo. The standard initial RX format is 48 kHz stereo FLOAT32.
- `TX_STREAM_AUDIO_BUFFERING` selects 50–500 ms of TX prebuffering for the connection. The configured appliance prebuffer applies until the client sets an override.
- `TX_CHRONO` starts after a CAT readback confirms key-down and the audio sink opens. Its rate, sample type, channels and scalar length reflect the TX owner's negotiated settings. Valid owner PCM frames without a matching chrono request remain accepted for push clients. Incoming TX frames must address receiver 0, use one of the supported rates and sample formats with one or two channels, and set codec and CRC to zero. The payload size must match the declared format and scalar length.
- The little-endian binary header is 64 bytes. Supported stream types are RX audio (1), TX audio (2), and TX chrono (3). Sample type codes are INT16 (0), INT24 (1), INT32 (2), and FLOAT32 (3). The channels field is at byte 28 and the packet counter at byte 32, followed by seven reserved words. Stream length counts scalar values across all channels. Chrono messages have a header with no PCM payload.

The bridge uses 48 kHz mono PCM internally. The current hardware path does not expose wideband IQ, a panorama IF, independent receiver audio, or complex DIGL/DIGU samples.


### Receive-filter edges

Set `radio.hamlib_filter_edges_hz: true` only after confirming that
`l SLOPE_LOW` and `l SLOPE_HIGH` report the rig's nonnegative audio
cutoffs in Hz in the applicable sideband. Some Hamlib drivers return
raw selectors through these commands. USB uses the audio values directly.
LSB reverses and negates them. Queries and writes run during receive.
A driver can reject either edge independently. The bridge reads back
both edges and broadcasts the resulting pair, which can include a partial
change. Rigs exposing only passband width leave this command unavailable.

### Receive meters and capability detection

The bridge attempts `l STRENGTH` only while a client subscribes. Hamlib defines
it as calibrated dB relative to S9. The bridge adds the IARU S9 reference
(−73 dBm below 30 MHz, −93 dBm at and above 30 MHz) to give an estimated
input level. Meter accuracy depends on the rig and its Hamlib calibration.
An unsupported or invalid response produces no sensor messages.

Use rigctld `\dump_caps` to inspect advertised get/set levels. A successful
live read is stronger evidence that a feature works in the current rig state.
The bridge probes STRENGTH directly and does not convert RAWSTR itself. Hamlib
can supply STRENGTH from RAWSTR when it has a calibration table. Filter slopes
still require verified units because some drivers return raw selector values
under the same level names.


### Transmit meters

TCI 2.1 defines `TX_SENSORS:<trx>,<mic_dbm>,<output_w>,<peak_w>,<swr>;`.
By default, the bridge sends a frame only when all four fields are available.
The generic rigctld backend currently supplies measured output watts
(`RFPOWER_METER_WATTS`) and SWR (`SWR`) where the driver supports those
levels. The power field uses the driver’s output meter reading. Its
averaging behavior depends on the rig.

For a client that accepts the bridge’s partial-frame extension, set
`server.allow_partial_tx_sensors: true`. Missing fields are written as
`nan`, which TCI 2.1 does not define. A rig with both generic meters may
send `tx_sensors:0,nan,25.5,nan,1.4;`. The bridge polls only while
subscribed and transmitting. ALC and configured power are excluded.
