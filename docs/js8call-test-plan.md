## Compatibility target for JS8Call

A JS8Call TCI client should initially target this minimal sequence:

```text
connect WebSocket
wait for startup burst / ready
query or accept current vfo/modulation/trx state
send:
  audio_stream_sample_type:int16;
  audio_stream_channels:1;
  audio_stream_samples:512;
  audio_samplerate:48000;
  audio_start:0;
receive RX_AUDIO_STREAM frames
send vfo/modulation/trx commands as needed
send TX_AUDIO_STREAM frames for transmit audio
optionally bracket TX audio with:
  tx_audio_start:0;
  tx_audio_stop:0;
```

Recommended JS8Call-side assumptions for the first implementation:

```text
one radio
one receiver
one VFO
48000 Hz mono int16 audio
explicit PTT using trx:0,true/false
do not depend on tx_audio_start automatically keying PTT
```
