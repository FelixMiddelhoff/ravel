#!/usr/bin/env python3
"""Feeds damaged trace files to ravel_trace.py and reports any crash.

The promise of ravel_trace.py is that a file that is not a good trace gives
exit status 2 and a one-line error, never a Python traceback. This script
starts from the real traces in docs/snippets/data, damages them in random ways
(flipped bytes, cut lines, wrong types, invalid UTF-8, ...) and runs every
command on the result.

    fuzz_trace.py [ITERATIONS] [SEED]      default: 2000 iterations, seed 0

Exit status: 0 if nothing crashed, 1 if something did (the damaged inputs are
printed). The same seed always does the same thing.
"""

import contextlib
import io
import json
import random
import sys
import tempfile
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ravel_trace  # noqa: E402

DATA = Path(__file__).resolve().parent.parent / "docs" / "snippets" / "data"

ODD_VALUES = [None, True, 0, -1, 1.5, "", "x", [], {}, [1, 2], {"a": 1}, 10**30]


def damage(rng, original):
    """Returns damaged bytes made from a good trace."""
    data = bytearray(original)
    lines = original.split(b"\n")
    for _ in range(rng.randint(1, 3)):
        kind = rng.randrange(9)
        if kind == 0 and data:  # Flip a byte.
            data[rng.randrange(len(data))] = rng.randrange(256)
        elif kind == 1:  # Cut the file short.
            del data[rng.randrange(len(data) + 1):]
        elif kind == 2:  # Drop a line.
            lines = bytes(data).split(b"\n")
            del lines[rng.randrange(len(lines))]
            data = bytearray(b"\n".join(lines))
        elif kind == 3:  # Repeat a line.
            lines = bytes(data).split(b"\n")
            i = rng.randrange(len(lines))
            lines.insert(i, lines[i])
            data = bytearray(b"\n".join(lines))
        elif kind == 4:  # Replace a whole line with some JSON value.
            lines = bytes(data).split(b"\n")
            lines[rng.randrange(len(lines))] = json.dumps(rng.choice(ODD_VALUES)).encode()
            data = bytearray(b"\n".join(lines))
        elif kind == 5:  # Give a field of one event another type.
            lines = bytes(data).split(b"\n")
            i = rng.randrange(len(lines))
            try:
                obj = json.loads(lines[i])
            except ValueError:
                continue
            if isinstance(obj, dict) and obj:
                obj[rng.choice(sorted(obj))] = rng.choice(ODD_VALUES)
                lines[i] = json.dumps(obj).encode()
                data = bytearray(b"\n".join(lines))
        elif kind == 6:  # Remove a field of one event.
            lines = bytes(data).split(b"\n")
            i = rng.randrange(len(lines))
            try:
                obj = json.loads(lines[i])
            except ValueError:
                continue
            if isinstance(obj, dict) and obj:
                del obj[rng.choice(sorted(obj))]
                lines[i] = json.dumps(obj).encode()
                data = bytearray(b"\n".join(lines))
        elif kind == 7:  # Invalid UTF-8.
            data[rng.randrange(len(data) + 1):0] = b"\xff\xfe"
        else:  # Pure noise.
            data = bytearray(rng.randbytes(rng.randint(0, 64)))
    return bytes(data)


def run_tool(*argv):
    """Runs the tool; returns None if it behaved, else the traceback text."""
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = ravel_trace.main(list(argv))
    except BaseException:  # A crash of any kind, including SystemExit from a bug.
        return traceback.format_exc()
    if status not in (0, 1, 2):
        return f"exit status {status}"
    return None


def fuzz(iterations=2000, seed=0):
    """Returns a list of (description, damaged bytes) for every crash found."""
    rng = random.Random(seed)
    samples = [path.read_bytes() for path in sorted(DATA.glob("*.trace.jsonl"))]
    problems = []
    with tempfile.TemporaryDirectory() as folder:
        bad, good = Path(folder) / "bad.jsonl", Path(folder) / "good.jsonl"
        good.write_bytes(samples[0])
        for _ in range(iterations):
            bad.write_bytes(damage(rng, rng.choice(samples)))
            commands = [
                ("summary", str(bad)),
                ("show", str(bad)),
                ("show", str(bad), "--kind", "TaskResumed", "--from", "1"),
                ("timeline", str(bad)),
                ("diff", str(bad), str(good)),
                ("diff", str(good), str(bad)),
            ]
            for argv in commands:
                failure = run_tool(*argv)
                if failure:
                    problems.append((" ".join(argv[:1]) + ": " + failure.strip().splitlines()[-1], bad.read_bytes()))
    return problems


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    problems = fuzz(iterations, seed)
    seen = set()
    for message, data in problems:
        if message in seen:
            continue
        seen.add(message)
        print(f"CRASH: {message}\n  input: {data[:300]!r}")
    print(f"{len(problems)} crashes in {iterations} iterations ({len(seen)} distinct)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
