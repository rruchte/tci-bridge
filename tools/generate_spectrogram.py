#!/usr/bin/env python3

import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import spectrogram

path = "/tmp/js8-tci-tx.raw"
sample_rate = 48000

pcm = np.fromfile(path, dtype="<i2").astype(np.float32)
pcm /= 32768.0

f, t, Sxx = spectrogram(
    pcm,
    fs=sample_rate,
    window="hann",
    nperseg=8192,
    noverlap=6144,
    scaling="spectrum",
    mode="magnitude",
)

Sdb = 20 * np.log10(Sxx + 1e-12)

lo = 1850
hi = 2100
mask = (f >= lo) & (f <= hi)

plt.figure(figsize=(14, 7))
plt.pcolormesh(t, f[mask], Sdb[mask], shading="auto")
plt.ylim(lo, hi)
plt.xlabel("Time (s)")
plt.ylabel("Frequency (Hz)")
plt.title("JS8 TCI TX narrow-band spectrogram")
plt.colorbar(label="dB")
plt.tight_layout()
plt.savefig("js8-tci-tx-narrow.png", dpi=150)