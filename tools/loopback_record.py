"""Records what the default speaker plays (WASAPI loopback) to a 16-bit stereo WAV.

Used to compare kig.exe's audio with ares, which has no audio dump of its own.
Usage: python tools/loopback_record.py out.wav SECONDS   (needs: pip install soundcard)
"""
import sys
import wave

import numpy as np
import soundcard as sc

RATE = 48000


def main():
    out, seconds = sys.argv[1], float(sys.argv[2])
    speaker = sc.default_speaker()
    mic = sc.get_microphone(speaker.name, include_loopback=True)
    chunks = []
    with mic.recorder(samplerate=RATE, channels=2) as rec:
        for _ in range(int(seconds)):
            chunks.append(rec.record(numframes=RATE))
    data = np.clip(np.concatenate(chunks), -1, 1)
    with wave.open(out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((data * 32767).astype("<i2").tobytes())
    print(f"{out}: {len(data) / RATE:.1f}s, peak {np.abs(data).max():.3f}")


if __name__ == "__main__":
    main()
