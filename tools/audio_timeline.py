"""Prints a per-second timeline of a WAV: loudness, and how different each second sounds from the
previous few (spectral distance), to spot music track changes and silences.

Usage: python tools/audio_timeline.py file.wav [--window 1.0]
"""
import argparse
import wave

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--window", type=float, default=1.0)
    a = ap.parse_args()
    with wave.open(a.wav) as w:
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels()).mean(axis=1) / 32768
    n = int(rate * a.window)
    specs = []
    for i in range(len(x) // n):
        seg = x[i * n:(i + 1) * n]
        spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
        # Log-spaced bands, 50 Hz to 8 kHz.
        edges = np.geomspace(50, 8000, 25) * len(seg) / rate
        bands = np.array([spec[int(lo):int(hi) + 1].mean() for lo, hi in zip(edges[:-1], edges[1:])])
        specs.append(np.log10(bands + 1e-6))
        rms = np.sqrt(np.mean(seg ** 2))
        prev = np.mean(specs[max(0, i - 4):i], axis=0) if i else specs[0]
        dist = np.linalg.norm(specs[-1] - prev)
        print(f"{i * a.window:6.1f}s rms {20 * np.log10(rms + 1e-9):6.1f} dB  change {dist:5.2f} {'#' * int(dist * 4)}")


if __name__ == "__main__":
    main()
