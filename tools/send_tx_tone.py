#!/usr/bin/env python3

import math
import struct
import sys
import time
import websocket

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:40001"

SAMPLE_RATE = 48000
CHANNELS = 1
SAMPLE_TYPE_INT16 = 0
TX_AUDIO_STREAM = 2
RECEIVER = 0
SAMPLES_PER_FRAME = 512
FREQ = 1000.0
DURATION_SECONDS = 3.0


def make_frame(pcm: bytes) -> bytes:
    sample_count = len(pcm) // 2

    header = struct.pack(
        "<16I",
        RECEIVER,
        SAMPLE_RATE,
        SAMPLE_TYPE_INT16,
        0,  # codec
        0,  # crc
        sample_count,
        TX_AUDIO_STREAM,
        CHANNELS,
        0, 0, 0, 0, 0, 0, 0, 0
    )

    return header + pcm


def make_tone_frame(start_sample: int) -> bytes:
    samples = []

    for i in range(SAMPLES_PER_FRAME):
        n = start_sample + i
        value = int(12000 * math.sin(2.0 * math.pi * FREQ * n / SAMPLE_RATE))
        samples.append(struct.pack("<h", value))

    return b"".join(samples)


def main():
    ws = websocket.create_connection(URL)

    try:
        print("Connected:", URL)

        ws.send("tx_audio_start:0;")

        total_samples = int(SAMPLE_RATE * DURATION_SECONDS)
        sent = 0

        while sent < total_samples:
            pcm = make_tone_frame(sent)
            ws.send_binary(make_frame(pcm))
            sent += SAMPLES_PER_FRAME
            time.sleep(SAMPLES_PER_FRAME / SAMPLE_RATE)

        ws.send("tx_audio_stop:0;")
        print("Done")

    finally:
        ws.close()


if __name__ == "__main__":
    main()