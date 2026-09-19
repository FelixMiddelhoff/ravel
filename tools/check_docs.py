#!/usr/bin/env python3
"""Keeps the code and output quoted in the docs identical to the real thing.

The docs quote source files and program output. Each quotation is a fenced
code block preceded by a marker comment:

    <!-- snippet: docs/snippets/deposit.hpp#deposit_system -->
    ```cpp
    ...the text of that region of the file...
    ```

    <!-- output: tutorial_deposit --trace -->
    ```text
    ...what that program prints...
    ```

    <!-- output@root: kv_sweep --replay docs/snippets/data/x.choices -->
    ```text
    ...the same, but run from the repository root, so file paths can be relative to it...
    ```

    <!-- tool: ravel_trace.py summary docs/snippets/data/x.trace.jsonl -->
    ```text
    ...what that script under tools/ prints (run from the repository root)...
    ```

A snippet names a file, and optionally a region of it between the lines
`// [name]` and `// [/name]`. An output names a program built under
docs/snippets in the build directory, plus its arguments; it is run in an empty
scratch directory (or, for output@root, in the repository root). A tool names a
script under tools/. Both must exit with status 0 or 1: 1 is a normal answer
for a sweep that finds a bug or a diff that differs; anything else is an error.

Usage:
    python3 tools/check_docs.py --build-dir build            # verify (used by CI)
    python3 tools/check_docs.py --build-dir build --update   # rewrite the docs
"""

import argparse
import difflib
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKED_BLOCK = re.compile(
    r"(<!-- (?P<kind>snippet|output@root|output|tool): (?P<arg>.+?) -->\n```(?P<lang>\w*)\n)(?P<body>.*?)(\n```)",
    re.DOTALL,
)


def region_of(path: Path, name: str) -> str:
    """The lines of `path` between `// [name]` and `// [/name]`."""
    lines = path.read_text(encoding="utf-8").splitlines()
    begin, end = f"// [{name}]", f"// [/{name}]"
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == begin)
        stop = next(i for i, line in enumerate(lines) if line.strip() == end)
    except StopIteration:
        sys.exit(f"error: region '{name}' not found in {path}")
    return "\n".join(line.rstrip() for line in lines[start + 1 : stop])


def snippet_text(arg: str) -> str:
    file_part, _, region = arg.partition("#")
    path = ROOT / file_part
    if not path.exists():
        sys.exit(f"error: snippet file {file_part} does not exist")
    if region:
        return region_of(path, region)
    return "\n".join(line.rstrip() for line in path.read_text(encoding="utf-8").splitlines())


def find_program(build_dir: Path, name: str) -> Path:
    for folder in (build_dir / "docs" / "snippets", build_dir / "docs" / "snippets" / "Release"):
        for suffix in ("", ".exe"):
            candidate = folder / (name + suffix)
            if candidate.is_file():
                return candidate
    sys.exit(f"error: program '{name}' not found under {build_dir}/docs/snippets (build it first)")


def output_text(build_dir: Path, kind: str, arg: str) -> str:
    words = shlex.split(arg)
    ok_statuses = (0, 1)  # 1 is a normal answer: a sweep that found a bug, a diff that differs.
    if kind == "tool":
        # A script under tools/.
        command = [sys.executable, str(ROOT / "tools" / words[0]), *words[1:]]
        ok_statuses = (0, 1)
    else:
        command = [str(find_program(build_dir, words[0])), *words[1:]]

    with tempfile.TemporaryDirectory() as scratch:
        cwd = scratch if kind == "output" else str(ROOT)
        # The docs show what a person sees at a terminal, so hide the environment
        # that changes the output: on GitHub Actions ravel adds error annotations.
        environment = {
            name: value
            for name, value in os.environ.items()
            if name != "GITHUB_ACTIONS" and not name.startswith("RAVEL_")
        }
        run = subprocess.run(
            command, cwd=cwd, capture_output=True, text=True, encoding="utf-8", env=environment
        )
    if run.returncode not in ok_statuses:
        sys.exit(f"error: '{arg}' exited with status {run.returncode}:\n{run.stdout}{run.stderr}")
    lines = run.stdout.replace("\r\n", "\n").split("\n")
    return "\n".join(line.rstrip() for line in lines).rstrip("\n")


def expected_body(kind: str, arg: str, build_dir: Path) -> str:
    return snippet_text(arg) if kind == "snippet" else output_text(build_dir, kind, arg)


def process(doc: Path, build_dir: Path, update: bool) -> int:
    with open(doc, encoding="utf-8", newline="") as file:  # Keep line endings as they are.
        original = file.read()
    newline = "\r\n" if "\r\n" in original else "\n"
    text = original.replace("\r\n", "\n")

    mismatches = 0

    def replace(match: re.Match) -> str:
        nonlocal mismatches
        expected = expected_body(match["kind"], match["arg"], build_dir)
        if match["body"].rstrip("\n") == expected:
            return match.group(0)
        mismatches += 1
        if not update:
            print(f"\n{doc.relative_to(ROOT)}: out of date: {match['kind']}: {match['arg']}")
            diff = difflib.unified_diff(
                match["body"].split("\n"), expected.split("\n"), "in the docs", "actual", lineterm=""
            )
            print("\n".join(list(diff)[:40]))
        return match.group(1) + expected + match.group(6)

    rewritten = MARKED_BLOCK.sub(replace, text)
    if update and mismatches:
        with open(doc, "w", encoding="utf-8", newline="") as file:
            file.write(rewritten.replace("\n", newline))
        print(f"{doc.relative_to(ROOT)}: updated {mismatches} block(s)")
    return mismatches


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--update", action="store_true", help="rewrite out-of-date blocks")
    args = parser.parse_args()

    docs = sorted((ROOT / "docs").glob("*.md")) + [ROOT / "README.md"]
    total = sum(process(doc, args.build_dir.resolve(), args.update) for doc in docs)

    if total and not args.update:
        print(f"\n{total} block(s) out of date. Run: python3 tools/check_docs.py --update")
        sys.exit(1)
    print("docs are up to date" if not total else f"{total} block(s) rewritten")


if __name__ == "__main__":
    main()
