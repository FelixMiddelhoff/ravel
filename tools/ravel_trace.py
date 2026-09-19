#!/usr/bin/env python3
"""Read and compare ravel trace files (ravel-seed-N.trace.jsonl).

A trace has one JSON object per line: a header, then one event per step (see
docs/formats.md). This tool turns that into something you can read at a glance.

    ravel_trace.py summary FILE             counts, times, and what went wrong
    ravel_trace.py show FILE [filters]      the events, one per line
    ravel_trace.py timeline FILE [filters]  one column per task/channel/disk
    ravel_trace.py diff A B                 where two traces part ways

Filters for `show` and `timeline`:
    --name NAME    only events about this task, channel or disk (repeatable)
    --kind KIND    only this kind of event, e.g. MessageDropped (repeatable)
    --from T       only events at virtual time T or later
    --to T         only events at virtual time T or earlier

Needs only Python 3.8+. Exit status: 0 on success (for `diff`: the traces are
identical), 1 if `diff` found a difference, 2 if a file is not a ravel trace.
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

SUPPORTED_TRACE_VERSION = 1

# A short word for each kind of event, for the timeline. Capitals mark what
# usually deserves a second look.
SHORT = {
    "TaskSpawned": "spawned",
    "TaskResumed": "runs",
    "TaskFinished": "finished",
    "TaskThrew": "THREW",
    "MessageSent": "send",
    "MessageDropped": "DROPPED",
    "MessageDelivered": "deliver",
    "DiskWritten": "write",
    "DiskSynced": "sync",
    "DiskFailed": "FAILED",
    "DiskCrashed": "CRASH",
}


class TraceError(Exception):
    pass


def load(path):
    """Returns (header, events) for a trace file, or raises TraceError."""
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise TraceError(f"{path}: {error.strerror or error}")
    if not lines:
        raise TraceError(f"{path}: the file is empty")

    try:
        header = json.loads(lines[0])
    except json.JSONDecodeError:
        raise TraceError(f"{path}: the first line is not JSON; is this a ravel trace?")
    if header.get("format") != "ravel-trace":
        raise TraceError(f"{path}: not a ravel trace (no \"format\":\"ravel-trace\" header)")
    if header.get("trace_version") != SUPPORTED_TRACE_VERSION:
        raise TraceError(
            f"{path}: trace_version {header.get('trace_version')} is not supported "
            f"(this tool reads version {SUPPORTED_TRACE_VERSION})"
        )

    events = []
    for number, line in enumerate(lines[1:], start=2):
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            raise TraceError(f"{path}:{number}: not valid JSON")
    return header, events


def matches(event, args):
    if args.name and event["name"] not in args.name:
        return False
    if args.kind and event["kind"] not in args.kind:
        return False
    if args.time_from is not None and event["time"] < args.time_from:
        return False
    if args.time_to is not None and event["time"] > args.time_to:
        return False
    return True


def table(rows, header):
    """Left-aligns text columns, right-aligns none: plain and copy-paste friendly."""
    widths = [max(len(str(row[i])) for row in [header] + rows) for i in range(len(header))]
    lines = []
    for row in [header] + rows:
        lines.append("  ".join(str(cell).ljust(width) for cell, width in zip(row, widths)).rstrip())
    return lines


def summary(args):
    header, events = load(args.file)
    kinds = Counter(event["kind"] for event in events)
    subjects = Counter(event["name"] for event in events)
    last_time = max((event["time"] for event in events), default=0)

    out = [
        f"{args.file}",
        f"seed {header.get('seed')}, ravel {header.get('ravel_version')}",
        f"{len(events)} events over {last_time} ticks of virtual time",
        "",
        "events by kind:",
    ]
    out += ["  " + line for line in table([[k, n] for k, n in sorted(kinds.items())], ["kind", "count"])]
    out += ["", "events by task, channel or disk:"]
    out += ["  " + line for line in table([[k, n] for k, n in sorted(subjects.items())], ["name", "count"])]

    notes = []
    for kind, what in (
        ("TaskThrew", "a task threw an exception (the run stops there)"),
        ("MessageDropped", "message(s) lost to the fault spec"),
        ("DiskFailed", "disk write(s) or sync(s) failed"),
        ("DiskCrashed", "simulated power cut(s)"),
    ):
        if kinds[kind]:
            notes.append(f"  {kinds[kind]} x {what}")
    if notes:
        out += ["", "worth a look:"] + notes
    print("\n".join(out))


def show(args):
    _, events = load(args.file)
    rows = [
        [event["step"], event["time"], event["kind"], event["name"]]
        for event in events
        if matches(event, args)
    ]
    if not rows:
        print("(no events match)")
        return
    print("\n".join(table(rows, ["step", "time", "kind", "name"])))


def timeline(args):
    _, events = load(args.file)
    shown = [event for event in events if matches(event, args)]
    if not shown:
        print("(no events match)")
        return

    lanes = []  # In order of first appearance.
    for event in shown:
        if event["name"] not in lanes:
            lanes.append(event["name"])

    header = ["time", "step"] + lanes
    rows = []
    for event in shown:
        row = [event["time"], event["step"]] + [""] * len(lanes)
        row[2 + lanes.index(event["name"])] = SHORT.get(event["kind"], event["kind"])
        rows.append(row)
    print("\n".join(table(rows, header)))


def describe(event):
    return f"t={event['time']} {event['kind']} {event['name']}"


def diff(args):
    _, first = load(args.a)
    _, second = load(args.b)

    def same(x, y):
        return (x["time"], x["kind"], x["id"], x["name"]) == (y["time"], y["kind"], y["id"], y["name"])

    common = min(len(first), len(second))
    split = next((i for i in range(common) if not same(first[i], second[i])), None)
    if split is None and len(first) == len(second):
        print(f"identical: {len(first)} events")
        return 0

    out = []
    if split is None:
        longer, name = (first, "A") if len(first) > len(second) else (second, "B")
        out.append(f"the traces agree for {common} events, then {name} goes on for {len(longer) - common} more")
        split = common
    else:
        out.append(f"the traces agree for {split} events, then part ways at step {split}:")

    context = range(max(0, split - 2), min(max(len(first), len(second)), split + 3))
    rows = []
    for i in context:
        a = describe(first[i]) if i < len(first) else "(ended)"
        b = describe(second[i]) if i < len(second) else "(ended)"
        marker = ">" if i == split else " "
        rows.append([marker, i, a, b])
    out += table(rows, ["", "step", "A: " + Path(args.a).name, "B: " + Path(args.b).name])

    kinds_a = Counter(event["kind"] for event in first)
    kinds_b = Counter(event["kind"] for event in second)
    changed = [
        [kind, kinds_a[kind], kinds_b[kind], f"{kinds_b[kind] - kinds_a[kind]:+d}"]
        for kind in sorted(set(kinds_a) | set(kinds_b))
        if kinds_a[kind] != kinds_b[kind]
    ]
    out += ["", f"A has {len(first)} events, B has {len(second)}."]
    if changed:
        out += ["", "what differs, by kind:"]
        out += ["  " + line for line in table(changed, ["kind", "A", "B", "change"])]
    print("\n".join(out))
    return 1


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def add_filters(command):
        command.add_argument("--name", action="append", help="only this task, channel or disk")
        command.add_argument("--kind", action="append", help="only this kind of event")
        command.add_argument("--from", dest="time_from", type=int, help="only events at or after this time")
        command.add_argument("--to", dest="time_to", type=int, help="only events at or before this time")

    commands.add_parser("summary").add_argument("file")
    for name in ("show", "timeline"):
        command = commands.add_parser(name)
        command.add_argument("file")
        add_filters(command)
    command = commands.add_parser("diff")
    command.add_argument("a")
    command.add_argument("b")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "diff":
            return diff(args)
        {"summary": summary, "show": show, "timeline": timeline}[args.command](args)
        return 0
    except TraceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
