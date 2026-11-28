"""Replay local 8 kHz mono S16 negative controls against a selected Aurora source tree."""

import argparse
from array import array
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import wave


def pcm16k(path):
    with wave.open(str(path), "rb") as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getframerate()) != (1, 2, 8000):
            raise ValueError(f"{path}: expected mono S16 at 8 kHz")
        frames = reader.getnframes()
        data = reader.readframes(frames)
    if not data or len(data) != frames * 2:
        raise ValueError(f"{path}: empty or incomplete PCM data")
    samples = [x for (x,) in struct.iter_unpack("<h", data)]
    output = array("h")
    for i, sample in enumerate(samples):
        following = samples[i + 1] if i + 1 < len(samples) else sample
        output.extend((sample, (sample + following) // 2))
    return output.tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="checkout containing the matcher to evaluate")
    parser.add_argument("clips", nargs="+", type=Path)
    args = parser.parse_args()
    kernel = args.source / "kernel"
    harness = Path(__file__).with_name("replay-speech-frames.c")
    with TemporaryDirectory() as temp:
        exe = Path(temp) / "replay"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-I", kernel / "include", harness, kernel / "proc/audio_vad.c",
                        kernel / "ui/aura_cmd.c", kernel / "ui/broker_policy.c", "-o", exe],
                       check=True)
        print("clip\tmax_distance\tframes\thome\tnotes\tcancel")
        for clip in args.clips:
            result = subprocess.run([exe], input=pcm16k(clip), capture_output=True, check=True)
            print(f"{clip.name}\t{result.stdout.decode().strip()}")


if __name__ == "__main__":
    main()
