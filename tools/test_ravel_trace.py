"""Tests for ravel_trace.py, run against the traces in docs/snippets/data.

    python3 -m unittest discover -s tools
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ravel_trace  # noqa: E402

DATA = Path(__file__).resolve().parent.parent / "docs" / "snippets" / "data"
ORIGINAL = str(DATA / "deposit_original.trace.jsonl")
MINIMAL = str(DATA / "deposit_minimal.trace.jsonl")


def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = ravel_trace.main(list(argv))
    return status, out.getvalue(), err.getvalue()


class TraceToolTest(unittest.TestCase):
    def test_summary_counts_events_and_flags_lost_messages(self):
        status, out, _ = run("summary", ORIGINAL)
        self.assertEqual(status, 0)
        self.assertIn("26 events over 208 ticks", out)
        self.assertIn("MessageDropped    4", out)
        self.assertIn("4 x message(s) lost", out)

    def test_show_filters_by_kind_name_and_time(self):
        _, out, _ = run("show", ORIGINAL, "--kind", "MessageDropped")
        self.assertEqual(len(out.strip().splitlines()), 1 + 4)  # header + 4 events

        _, out, _ = run("show", ORIGINAL, "--name", "server", "--to", "1")
        self.assertNotIn("client", out)
        self.assertNotIn("t=50", out)

        _, out, _ = run("show", ORIGINAL, "--kind", "DiskCrashed")
        self.assertEqual(out.strip(), "(no events match)")

    def test_timeline_has_one_column_per_subject(self):
        _, out, _ = run("timeline", MINIMAL)
        header = out.splitlines()[0].split()
        self.assertEqual(header, ["time", "step", "server", "client", "client->server", "server->client"])
        self.assertIn("DROPPED", out)

    def test_diff_of_a_trace_with_itself_is_identical(self):
        status, out, _ = run("diff", MINIMAL, MINIMAL)
        self.assertEqual(status, 0)
        self.assertEqual(out.strip(), "identical: 17 events")

    def test_diff_finds_the_first_divergence_and_what_changed(self):
        status, out, _ = run("diff", ORIGINAL, MINIMAL)
        self.assertEqual(status, 1)
        self.assertIn("part ways at step 2", out)
        self.assertIn("MessageDropped  4  1  -3", out)

    def test_diff_reports_a_trace_that_is_a_prefix_of_another(self):
        with tempfile.TemporaryDirectory() as folder:
            shorter = Path(folder) / "shorter.jsonl"
            shorter.write_text("".join(Path(MINIMAL).read_text().splitlines(True)[:6]))
            status, out, _ = run("diff", str(shorter), MINIMAL)
        self.assertEqual(status, 1)
        self.assertIn("then B goes on for 12 more", out)

    def test_rejects_files_that_are_not_traces(self):
        with tempfile.TemporaryDirectory() as folder:
            bad = Path(folder) / "bad.jsonl"

            bad.write_text("")
            self.assertEqual(run("show", str(bad))[0], 2)

            bad.write_text("not json\n")
            status, _, err = run("show", str(bad))
            self.assertEqual(status, 2)
            self.assertIn("is this a ravel trace", err)

            bad.write_text('{"format":"something-else"}\n')
            self.assertIn("not a ravel trace", run("show", str(bad))[2])

            bad.write_text('{"format":"ravel-trace","trace_version":99}\n')
            self.assertIn("trace_version 99 is not supported", run("show", str(bad))[2])

        self.assertEqual(run("show", "no/such/file")[0], 2)

    def test_reports_damaged_traces_instead_of_crashing(self):
        header = '{"format":"ravel-trace","trace_version":1}\n'
        good = '{"step":0,"time":0,"kind":"TaskSpawned","id":0,"name":"t"}\n'
        cases = {
            "not UTF-8": (b"\xff\xfe", "not UTF-8"),
            "header is a list": (b"[1]\n", "not a JSON object"),
            "event is a list": ((header + "[1]\n").encode(), "must be a JSON object"),
            "event is null": ((header + "null\n").encode(), "must be a JSON object"),
            "event is not JSON": ((header + "{oops\n").encode(), "must be a JSON object"),
            "missing field": ((header + '{"step":0}\n').encode(), 'field "time"'),
            "wrong type": ((header + good.replace('"t"', "5")).encode(), 'field "name"'),
            "bool as int": ((header + good.replace('"step":0', '"step":true')).encode(), 'field "step"'),
            "deep nesting": ((header + "[" * 100000).encode(), "must be a JSON object"),
        }
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "damaged.jsonl"
            for name, (content, message) in cases.items():
                path.write_bytes(content)
                for command in ("summary", "show", "timeline"):
                    status, _, err = run(command, str(path))
                    self.assertEqual(status, 2, f"{name}: {command}")
                    self.assertIn(message, err, f"{name}: {command}")

    def test_random_damage_never_crashes_the_tool(self):
        import fuzz_trace

        self.assertEqual(fuzz_trace.fuzz(iterations=150, seed=1), [])


if __name__ == "__main__":
    unittest.main()
