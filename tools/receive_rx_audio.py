#!/usr/bin/env python3

import struct
import sys
import time
import wave
import websocket

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:40001"
OUT = sys.argv[2] if len(sys.argv) > 2 else "rx_capture.wav"
DURATION_SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

RX_AUDIO_STREAM = 1
SAMPLE_TYPE_INT16 = 0


def parse_frame(data: bytes):
    if len(data) < 64:
        raise ValueError(f"Frame too short: {len(data)} bytes")

    header = struct.unpack("<16I", data[:64])

    return {
        "receiver": header[0],
        "sample_rate": header[1],
        "sample_type": header[2],
        "codec": header[3],
        "crc": header[4],
        "sample_count": header[5],
        "stream_type": header[6],
        "channels": header[7],
        "payload": data[64:],
    }


def main():
    ws = websocket.create_connection(URL)
    pcm_chunks = []

    try:
        print("Connected:", URL)

        ws.send("audio_stream_sample_type:int16;")
        ws.send("audio_stream_channels:1;")
        ws.send("audio_stream_samples:512;")
        ws.send("audio_samplerate:48000;")
        ws.send("audio_start:0;")

        deadline = time.time() + DURATION_SECONDS
        frames = 0
        total_payload = 0
        sample_rate = 48000
        channels = 1

        while time.time() < deadline:
            msg = ws.recv()

            if isinstance(msg, str):
                print("TEXT:", msg)
                continue

            frame = parse_frame(msg)

            if frame["stream_type"] != RX_AUDIO_STREAM:
                print("Ignoring non-RX stream:", frame["stream_type"])
                continue

            if frame["sample_type"] != SAMPLE_TYPE_INT16:
                raise ValueError(f"Unsupported sample type: {frame['sample_type']}")

            if frame["channels"] != 1:
                raise ValueError(f"Expected mono, got channels={frame['channels']}")

            expected_bytes = frame["sample_count"] * 2 * frame["channels"]

            if len(frame["payload"]) < expected_bytes:
                raise ValueError(
                    f"Short payload: got {len(frame['payload'])}, "
                    f"expected {expected_bytes}"
                )

            sample_rate = frame["sample_rate"]
            channels = frame["channels"]

            pcm = frame["payload"][:expected_bytes]
            pcm_chunks.append(pcm)

            frames += 1
            total_payload += len(pcm)

            if frames % 50 == 0:
                print(
                    f"frames={frames} "
                    f"rate={sample_rate} "
                    f"channels={channels} "
                    f"payload={total_payload}"
                )

        ws.send("audio_stop:0;")

    finally:
        ws.close()

    pcm = b"".join(pcm_chunks)

    with wave.open(OUT, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)

    print(f"Wrote {OUT}: {len(pcm)} bytes, {frames} frames")


if __name__ == "__main__":
    main()