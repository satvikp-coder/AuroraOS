"""Score offline decoder transcripts against a fixed, independently labelled clip manifest."""

import argparse
import csv
import sys

COMMANDS = {"open home", "open notes", "cancel timer"}


def read_rows(path, fields, allow_group=False):
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames != fields and not (allow_group and reader.fieldnames == fields + ["group"]):
            raise ValueError(f"{path}: expected tab-separated header {fields}")
        rows = {}
        for row in reader:
            if None in row or None in row.values():
                raise ValueError(f"{path}:{reader.line_num}: wrong field count")
            clip = row["clip"]
            if not clip.strip() or clip in rows:
                raise ValueError(f"{path}:{reader.line_num}: missing or duplicate clip ID")
            if "group" in row and not row["group"].strip():
                raise ValueError(f"{path}:{reader.line_num}: empty group")
            rows[clip] = row
        if not rows:
            raise ValueError(f"{path}: no clips")
        return rows


def score(labels, transcripts):
    if labels.keys() != transcripts.keys():
        raise ValueError("clip IDs differ between labels and transcripts")
    outcomes = {"correct_command": 0, "missed_command": 0, "wrong_command": 0,
                "correct_reject": 0, "false_accept": 0}
    for clip, row in labels.items():
        expected = row["expected"]
        if expected not in COMMANDS and expected != "none":
            raise ValueError(f"{clip}: unsupported expected command {expected!r}")
        text = " ".join(transcripts[clip]["transcript"].lower().split())
        predicted = text if text in COMMANDS else "none"
        if expected == "none":
            result = "correct_reject" if predicted == "none" else "false_accept"
        elif predicted == "none":
            result = "missed_command"
        else:
            result = "correct_command" if predicted == expected else "wrong_command"
        outcomes[result] += 1
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("labels", help="TSV with clip and expected columns")
    parser.add_argument("transcripts", help="TSV with clip and transcript columns")
    parser.add_argument("--breakdown", action="store_true",
                        help="show outcomes by expected command and optional label group")
    args = parser.parse_args()
    try:
        labels = read_rows(args.labels, ["clip", "expected"], allow_group=True)
        transcripts = read_rows(args.transcripts, ["clip", "transcript"])
        outcomes = score(labels, transcripts)
    except (ValueError, OSError, csv.Error) as exc:
        parser.exit(2, f"{exc}\n")
    for name, count in outcomes.items():
        print(f"{name}\t{count}")
    if args.breakdown:
        print("group\texpected\tclips\t" + "\t".join(outcomes))
        buckets = {}
        for clip, row in labels.items():
            key = (row.get("group", "all"), row["expected"])
            buckets.setdefault(key, {})[clip] = row
        for (group, expected), subset in sorted(buckets.items()):
            counts = score(subset, {clip: transcripts[clip] for clip in subset})
            print(group, expected, len(subset), *counts.values(), sep="\t")
    return 0


if __name__ == "__main__":
    sys.exit(main())
