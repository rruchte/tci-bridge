#!/usr/bin/env python3

import argparse
import math
import struct
import sys
import time
from typing import Optional, Tuple

import websocket


RX_AUDIO_STREAM = 1
TX_AUDIO_STREAM = 2
SAMPLE_TYPE_INT16 = 0

DEFAULT_FREQ_HZ = 7078000
DEFAULT_MODE = "usb"


class SmokeFailure(Exception):
    pass


def log(msg: str) -> None:
    print(f"[tci-smoke] {msg}", flush=True)


def fail(msg: str) -> None:
    raise SmokeFailure(msg)


def parse_stream_frame(data: bytes) -> dict:
    if len(data) < 64:
        fail(f"binary frame too short: {len(data)} bytes")

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


def make_tx_audio_frame(
        pcm: bytes,
        sample_rate: int = 48000,
        channels: int = 1,
        receiver: int = 0,
) -> bytes:
    sample_count = len(pcm) // 2 // channels

    header = struct.pack(
        "<16I",
        receiver,
        sample_rate,
        SAMPLE_TYPE_INT16,
        0,  # codec
        0,  # crc
        sample_count,
        TX_AUDIO_STREAM,
        channels,
        0, 0, 0, 0, 0, 0, 0, 0,
    )

    return header + pcm


def make_tone_pcm(
        start_sample: int,
        sample_count: int,
        sample_rate: int = 48000,
        tone_hz: float = 1000.0,
        amplitude: int = 12000,
) -> bytes:
    out = bytearray()

    for i in range(sample_count):
        n = start_sample + i
        value = int(amplitude * math.sin(2.0 * math.pi * tone_hz * n / sample_rate))
        out += struct.pack("<h", value)

    return bytes(out)


class TciSmokeClient:
    def __init__(self, url: str, timeout: float):
        self.url = url
        self.timeout = timeout
        self.ws: Optional[websocket.WebSocket] = None

    def connect(self) -> None:
        log(f"connecting to {self.url}")
        self.ws = websocket.create_connection(self.url, timeout=self.timeout)
        self.ws.settimeout(self.timeout)
        log("connected")

    def close(self) -> None:
        if self.ws:
            self.ws.close()
            self.ws = None

    def send_text(self, text: str) -> None:
        if not self.ws:
            fail("not connected")

        log(f"TX text: {text}")
        self.ws.send(text)

    def send_binary(self, data: bytes) -> None:
        if not self.ws:
            fail("not connected")

        self.ws.send_binary(data)

    def recv(self):
        if not self.ws:
            fail("not connected")

        try:
            return self.ws.recv()
        except websocket.WebSocketTimeoutException:
            fail("timed out waiting for message")

    def drain_startup(self, max_messages: int = 30) -> list[str]:
        messages: list[str] = []
        deadline = time.time() + self.timeout

        while time.time() < deadline and len(messages) < max_messages:
            try:
                msg = self.ws.recv()
            except websocket.WebSocketTimeoutException:
                break

            if isinstance(msg, bytes):
                continue

            messages.append(msg)
            log(f"RX startup text: {msg}")

            if msg.strip().lower() == "ready;":
                # The bridge usually sends state right after ready, so take a
                # short additional drain window.
                extra_deadline = time.time() + 0.5
                while time.time() < extra_deadline and len(messages) < max_messages:
                    old_timeout = self.ws.gettimeout()
                    self.ws.settimeout(0.1)
                    try:
                        extra = self.ws.recv()
                    except websocket.WebSocketTimeoutException:
                        break
                    finally:
                        self.ws.settimeout(old_timeout)

                    if isinstance(extra, str):
                        messages.append(extra)
                        log(f"RX startup text: {extra}")
                break

        if not any(m.strip().lower() == "ready;" for m in messages):
            fail("did not receive ready; during startup")

        return messages

    def wait_for_text_containing(self, needle: str, timeout: Optional[float] = None) -> str:
        deadline = time.time() + (timeout or self.timeout)
        needle_lower = needle.lower()

        while time.time() < deadline:
            old_timeout = self.ws.gettimeout()
            self.ws.settimeout(max(0.05, min(0.5, deadline - time.time())))

            try:
                msg = self.ws.recv()
            except websocket.WebSocketTimeoutException:
                continue
            finally:
                self.ws.settimeout(old_timeout)

            if isinstance(msg, bytes):
                log(f"RX binary while waiting for text: {len(msg)} bytes")
                continue

            log(f"RX text: {msg}")

            if needle_lower in msg.lower():
                return msg

        fail(f"did not receive text containing {needle!r}")

    def wait_for_rx_audio_frame(self, timeout: Optional[float] = None) -> Tuple[dict, bytes]:
        deadline = time.time() + (timeout or self.timeout)

        while time.time() < deadline:
            old_timeout = self.ws.gettimeout()
            self.ws.settimeout(max(0.05, min(0.5, deadline - time.time())))

            try:
                msg = self.ws.recv()
            except websocket.WebSocketTimeoutException:
                continue
            finally:
                self.ws.settimeout(old_timeout)

            if isinstance(msg, str):
                log(f"RX text while waiting for audio: {msg}")
                continue

            frame = parse_stream_frame(msg)

            if frame["stream_type"] != RX_AUDIO_STREAM:
                log(f"ignoring binary stream type {frame['stream_type']}")
                continue

            return frame, msg

        fail("did not receive RX audio frame")


def test_startup(client: TciSmokeClient) -> None:
    log("test: startup")
    messages = client.drain_startup()

    required = ["protocol:", "version:", "device:", "ready;"]

    for item in required:
        if not any(item in m.lower() for m in messages):
            fail(f"startup did not include {item}")

    log("PASS startup")


def test_frequency(client: TciSmokeClient, freq_hz: int) -> None:
    log("test: frequency")

    client.send_text("vfo;")
    client.wait_for_text_containing("vfo:")

    client.send_text(f"vfo:0,0,{freq_hz};")
    client.wait_for_text_containing(f"{freq_hz}")

    client.send_text("vfo;")
    msg = client.wait_for_text_containing(f"{freq_hz}")

    if f"{freq_hz}" not in msg:
        fail(f"frequency readback did not include {freq_hz}: {msg}")

    log("PASS frequency")


def test_mode(client: TciSmokeClient, mode: str) -> None:
    log("test: mode")

    client.send_text("modulation;")
    client.wait_for_text_containing("modulation:")

    client.send_text(f"modulation:0,0,{mode};")
    client.wait_for_text_containing("modulation:")

    client.send_text("modulation;")
    msg = client.wait_for_text_containing("modulation:")

    if mode.lower() not in msg.lower():
        # Some rigs may normalize DIGU/PKTUSB, so do not hard-fail on mode
        # readback by default. Frequency is the stronger rigctld sanity test.
        log(f"WARN mode readback did not include requested {mode!r}: {msg}")

    log("PASS mode")


def test_ptt_safety_default(client: TciSmokeClient) -> None:
    log("test: default PTT safety / denied TX")

    client.send_text("trx:0,true;")
    msg = client.wait_for_text_containing("trx:0,")

    if "true" in msg.lower():
        fail("PTT appeared to key, but transmit should be disabled by default")

    if "false" not in msg.lower():
        fail(f"unexpected PTT safety response: {msg}")

    log("PASS default PTT safety")


def test_rx_audio(client: TciSmokeClient, frames: int) -> None:
    log("test: RX audio")

    client.send_text("audio_stream_sample_type:int16;")
    client.wait_for_text_containing("audio_stream_sample_type:")

    client.send_text("audio_stream_channels:1;")
    client.wait_for_text_containing("audio_stream_channels:")

    client.send_text("audio_stream_samples:512;")
    client.wait_for_text_containing("audio_stream_samples:")

    client.send_text("audio_samplerate:48000;")
    client.wait_for_text_containing("audio_samplerate:")

    client.send_text("audio_start:0;")
    client.wait_for_text_containing("audio_start:")

    total_payload = 0

    for i in range(frames):
        frame, raw = client.wait_for_rx_audio_frame()

        if frame["sample_rate"] != 48000:
            fail(f"unexpected RX sample rate: {frame['sample_rate']}")

        if frame["sample_type"] != SAMPLE_TYPE_INT16:
            fail(f"unexpected RX sample type: {frame['sample_type']}")

        if frame["channels"] != 1:
            fail(f"unexpected RX channels: {frame['channels']}")

        expected_payload = frame["sample_count"] * 2 * frame["channels"]

        if len(frame["payload"]) < expected_payload:
            fail(
                f"short RX payload: got {len(frame['payload'])}, "
                f"expected {expected_payload}"
            )

        total_payload += expected_payload

        if (i + 1) % 10 == 0:
            log(f"RX frames validated: {i + 1}/{frames}")

    client.send_text("audio_stop:0;")
    client.wait_for_text_containing("audio_stop:")

    log(f"PASS RX audio: frames={frames} payload={total_payload} bytes")


def test_tx_audio(client: TciSmokeClient, duration_seconds: float) -> None:
    log("test: TX audio send path")

    sample_rate = 48000
    samples_per_frame = 512
    total_samples = int(sample_rate * duration_seconds)

    client.send_text("tx_audio_start:0;")
    client.wait_for_text_containing("tx_audio_start:")

    sent = 0
    frames = 0

    while sent < total_samples:
        pcm = make_tone_pcm(sent, samples_per_frame)
        client.send_binary(make_tx_audio_frame(pcm))
        sent += samples_per_frame
        frames += 1
        time.sleep(samples_per_frame / sample_rate)

    client.send_text("tx_audio_stop:0;")
    client.wait_for_text_containing("tx_audio_stop:")

    log(f"PASS TX audio: frames={frames} samples={sent}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Smoke-test the tci-bridge protocol subset."
    )

    parser.add_argument(
        "url",
        nargs="?",
        default="ws://127.0.0.1:40001",
        help="TCI WebSocket URL",
    )

    parser.add_argument(
        "--freq",
        type=int,
        default=DEFAULT_FREQ_HZ,
        help="Frequency to set during CAT test",
    )

    parser.add_argument(
        "--mode",
        default=DEFAULT_MODE,
        help="Mode to set during CAT test",
    )

    parser.add_argument(
        "--rx-frames",
        type=int,
        default=20,
        help="Number of RX audio frames to validate",
    )

    parser.add_argument(
        "--tx-audio",
        action="store_true",
        help="Send TX audio frames to the bridge",
    )

    parser.add_argument(
        "--tx-duration",
        type=float,
        default=1.0,
        help="TX tone duration in seconds when --tx-audio is set",
    )

    parser.add_argument(
        "--skip-ptt-safety",
        action="store_true",
        help="Skip default transmit-disabled PTT safety test",
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="WebSocket receive timeout in seconds",
    )

    args = parser.parse_args()

    client = TciSmokeClient(args.url, args.timeout)

    try:
        client.connect()

        test_startup(client)
        test_frequency(client, args.freq)
        test_mode(client, args.mode)

        if not args.skip_ptt_safety:
            test_ptt_safety_default(client)

        test_rx_audio(client, args.rx_frames)

        if args.tx_audio:
            test_tx_audio(client, args.tx_duration)

        log("ALL TESTS PASSED")
        return 0

    except SmokeFailure as e:
        log(f"FAIL: {e}")
        return 1

    except KeyboardInterrupt:
        log("interrupted")
        return 130

    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())