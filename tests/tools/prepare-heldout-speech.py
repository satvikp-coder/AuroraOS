"""Validate a local, consented command corpus and emit its evaluation labels as TSV."""

import argparse
import csv
import hashlib
from pathlib import Path
import struct
import sys
import wave


FIELDS = ["clip", "speaker", "split", "expected", "consent_ref", "sha256"]
LABELS = {"open home", "open notes", "cancel timer", "none"}


def pcm_digest(path):
    with wave.open(str(path), "rb") as stream:
        if (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) != (1, 2, 16000):
            raise ValueError(f"{path}: expected mono S16 at 16 kHz")
        frames = stream.getnframes()
        if not 0 < frames <= 10 * 16000:
            raise ValueError(f"{path}: expected a nonempty utterance of at most 10 seconds")
        samples = stream.readframes(frames)
        if len(samples) != 2 * frames:
            raise ValueError(f"{path}: truncated PCM data")
    return hashlib.sha256(samples).hexdigest()


def evaluation_rows(manifest):
    root = manifest.resolve().parent
    speakers, labels = {}, {}
    seen_paths, seen_audio = set(), set()
    result = []
    with manifest.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames != FIELDS:
            raise ValueError(f"expected TSV header {FIELDS}")
        for row in reader:
            if None in row or any(not value or not value.strip() for value in row.values()):
                raise ValueError(f"row {reader.line_num}: missing or extra field")
            split, speaker, label = row["split"], row["speaker"], row["expected"]
            if split not in {"development", "evaluation"} or label not in LABELS:
                raise ValueError(f"row {reader.line_num}: invalid split or expected label")
            if speakers.setdefault(speaker, split) != split:
                raise ValueError(f"{speaker}: speaker appears in both splits")
            relative = Path(row["clip"])
            path = (root / relative).resolve()
            if relative.is_absolute() or root not in path.parents or path in seen_paths:
                raise ValueError(f"row {reader.line_num}: duplicate or nonlocal clip path")
            digest = hashlib.sha256()
            with path.open("rb") as audio_file:
                for chunk in iter(lambda: audio_file.read(65536), b""):
                    digest.update(chunk)
            if digest.hexdigest() != row["sha256"]:
                raise ValueError(f"{relative}: file SHA-256 mismatch")
            audio = pcm_digest(path)
            if audio in seen_audio:
                raise ValueError(f"{relative}: duplicate PCM, possibly under different WAV metadata")
            seen_paths.add(path)
            seen_audio.add(audio)
            labels.setdefault(speaker, set()).add(label)
            if split == "evaluation":
                result.append((row["clip"], label))
    if set(speakers.values()) != {"development", "evaluation"}:
        raise ValueError("both development and evaluation speakers are required")
    for speaker, split in speakers.items():
        if split == "evaluation" and labels[speaker] != LABELS:
            raise ValueError(f"{speaker}: evaluation requires each command and a negative control")
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        rows = evaluation_rows(args.manifest)
    except (OSError, ValueError, csv.Error, wave.Error, EOFError, struct.error) as exc:
        parser.exit(2, f"{exc}\n")
    writer = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    writer.writerow(["clip", "expected"])
    writer.writerows(rows)


if __name__ == "__main__":
    main()
