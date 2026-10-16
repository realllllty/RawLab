#!/usr/bin/env python3
"""Check Nikon HE/HE* fixtures through the patched LibRaw; never download or bundle photos.

Each manifest entry pins the original file by SHA-256 and the unpacked CFA by an
FNV-1a 64 hash, so a decoder change that alters a single sample fails. The
lossless entries confirm that same-body lossless files keep their decoder.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from urllib.parse import urlparse


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("nikon-he-raw-samples.json"))
    parser.add_argument("--samples", type=Path, required=True,
                        help="Directory containing original filenames from the manifest URLs")
    parser.add_argument("--tool", type=Path, required=True, help="Built nikon_he_tests executable")
    parser.add_argument("--only", nargs="*", help="Check only these manifest IDs")
    args = parser.parse_args()
    entries = json.loads(args.manifest.read_text())["samples"]
    if args.only:
        unknown = set(args.only) - {entry["id"] for entry in entries}
        if unknown:
            parser.error("Unknown sample IDs: " + ", ".join(sorted(unknown)))
        entries = [entry for entry in entries if entry["id"] in args.only]
    failures = 0
    for entry in entries:
        path = args.samples / Path(urlparse(entry["url"]).path).name
        if not path.is_file():
            print(f"FAIL {entry['id']}: missing {path}")
            failures += 1
            continue
        if sha256(path) != entry["sha256"]:
            print(f"FAIL {entry['id']}: {path.name} is not the pinned file")
            failures += 1
            continue
        result = subprocess.run([str(args.tool), "--decode", str(path)],
                                capture_output=True, text=True, timeout=300)
        facts = dict(field.split("=", 1) for field in result.stdout.split())
        width, height = entry["expected_dimensions"]
        expected = {"decoder": entry["decoder"], "width": str(width), "height": str(height),
                    "errors": "0", "fnv1a64": entry["raw_fnv1a64"]}
        wrong = {key: facts.get(key) for key, value in expected.items() if facts.get(key) != value}
        if result.returncode or wrong:
            print(f"FAIL {entry['id']}: {wrong or result.stdout.strip()} {result.stderr.strip()}")
            failures += 1
        else:
            print(f"PASS {entry['id']}: {entry['decoder']} {width}x{height} {entry['raw_fnv1a64']}")
    print(f"{len(entries) - failures} of {len(entries)} samples passed")
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
