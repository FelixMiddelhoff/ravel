#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ravel/simulation.hpp"
#include "ravel/version.hpp"
#include "testing.hpp"

namespace {

// A fresh, empty directory that is removed again when the test ends.
class ScratchDir {
 public:
  ScratchDir() : path_(std::filesystem::temp_directory_path() / "ravel-trace-test") {
    std::filesystem::remove_all(path_);
  }
  ~ScratchDir() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::stringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

}  // namespace

TEST(trace_digest_is_fnv1a_over_the_little_endian_bytes_of_each_field) {
  // Written out separately from the library; the big values use every byte.
  const auto fnv = [](std::uint64_t digest, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      digest ^= (value >> (8 * byte)) & 0xFF;
      digest *= 0x100000001B3ULL;
    }
    return digest;
  };
  ravel::Trace trace;
  CHECK(trace.digest() == 0xCBF29CE484222325ULL);
  const ravel::TraceEvent event{0x0123456789ABCDEFULL, static_cast<std::size_t>(0xFEDCBA9876543210ULL),
                                ravel::TraceEventKind::DiskSynced};
  trace.record(event);
  std::uint64_t expected = 0xCBF29CE484222325ULL;
  expected = fnv(expected, event.time);
  expected = fnv(expected, event.subject);
  expected = fnv(expected, static_cast<std::uint64_t>(event.kind));
  CHECK(trace.digest() == expected);
}

TEST(trace_is_written_as_json_lines) {
  ravel::Simulation sim(7);
  auto& channel = sim.add_channel("a", "b", {});
  sim.scheduler().spawn("say \"hi\"", [&]() -> ravel::Task {
    channel.send("x");
    co_return;
  });
  sim.run_until_quiescent();

  std::ostringstream out;
  sim.write_trace(out);
  const std::string expected =
      std::string("{\"format\":\"ravel-trace\",\"trace_version\":1,\"ravel_version\":\"") +
      ravel::version_string() + "\",\"seed\":7}\n"
      "{\"step\":0,\"time\":0,\"kind\":\"TaskSpawned\",\"id\":0,\"name\":\"say \\\"hi\\\"\"}\n"
      "{\"step\":1,\"time\":0,\"kind\":\"TaskResumed\",\"id\":0,\"name\":\"say \\\"hi\\\"\"}\n"
      "{\"step\":2,\"time\":0,\"kind\":\"MessageSent\",\"id\":0,\"name\":\"a->b\"}\n"
      "{\"step\":3,\"time\":0,\"kind\":\"TaskFinished\",\"id\":0,\"name\":\"say \\\"hi\\\"\"}\n"
      "{\"step\":4,\"time\":0,\"kind\":\"MessageDelivered\",\"id\":0,\"name\":\"a->b\"}\n";
  CHECK(out.str() == expected);
}

TEST(failed_run_dumps_its_trace_when_a_directory_is_set) {
  const ScratchDir dir;
  ravel::Simulation sim(42, ravel::SimulationOptions{.trace_dir = dir.path() / "nested"});
  sim.scheduler().spawn("task", []() -> ravel::Task { co_return; });
  sim.add_invariant("always_false", [] { return false; });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(std::filesystem::path(result.trace_path).filename() == "ravel-seed-42.trace.jsonl");

  std::ostringstream expected;
  sim.write_trace(expected);
  CHECK(read_file(result.trace_path) == expected.str());
}

TEST(passing_run_writes_no_trace) {
  const ScratchDir dir;
  ravel::Simulation sim(1, ravel::SimulationOptions{.trace_dir = dir.path()});
  sim.scheduler().spawn("task", []() -> ravel::Task { co_return; });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(result.ok);
  CHECK(result.trace_path.empty());
  CHECK(!std::filesystem::exists(dir.path()));
}

TEST(failed_run_writes_no_trace_unless_asked) {
  ravel::Simulation sim(1);
  sim.add_invariant("always_false", [] { return false; });
  CHECK(sim.run_until_quiescent().trace_path.empty());
}

TEST(failed_run_reports_when_the_trace_cannot_be_written) {
  const ScratchDir dir;
  std::filesystem::create_directories(dir.path());
  std::ofstream(dir.path() / "blocker") << "a file, not a directory";

  ravel::Simulation sim(1, ravel::SimulationOptions{.trace_dir = dir.path() / "blocker"});
  sim.add_invariant("always_false", [] { return false; });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.trace_path.empty());
  CHECK(result.failure.find("trace could not be written") != std::string::npos);
}

TEST(trace_escapes_control_characters_in_names) {
  ravel::Simulation sim(1);
  sim.scheduler().spawn(std::string("a\\b\n\r\t\x01z"), []() -> ravel::Task { co_return; });
  sim.run_until_quiescent();

  std::ostringstream out;
  sim.write_trace(out);
  CHECK(out.str().find("\"name\":\"a\\\\b\\n\\r\\t\\u0001z\"") != std::string::npos);
}

TEST(trace_event_kinds_have_names_and_subjects) {
  using ravel::TraceEventKind;
  using ravel::TraceSubject;
  struct Row {
    TraceEventKind kind;
    const char* name;
    TraceSubject subject;
  };
  const Row rows[] = {
      {TraceEventKind::TaskSpawned, "TaskSpawned", TraceSubject::Task},
      {TraceEventKind::TaskResumed, "TaskResumed", TraceSubject::Task},
      {TraceEventKind::TaskFinished, "TaskFinished", TraceSubject::Task},
      {TraceEventKind::TaskThrew, "TaskThrew", TraceSubject::Task},
      {TraceEventKind::MessageSent, "MessageSent", TraceSubject::Channel},
      {TraceEventKind::MessageDropped, "MessageDropped", TraceSubject::Channel},
      {TraceEventKind::MessageDelivered, "MessageDelivered", TraceSubject::Channel},
      {TraceEventKind::DiskWritten, "DiskWritten", TraceSubject::Disk},
      {TraceEventKind::DiskSynced, "DiskSynced", TraceSubject::Disk},
      {TraceEventKind::DiskFailed, "DiskFailed", TraceSubject::Disk},
      {TraceEventKind::DiskCrashed, "DiskCrashed", TraceSubject::Disk},
  };
  for (const Row& row : rows) {
    CHECK(std::string(ravel::to_string(row.kind)) == row.name);
    CHECK(ravel::subject_of(row.kind) == row.subject);
  }
}

TEST(trace_describes_disk_events_by_the_disks_name) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("wal");
  sim.scheduler().spawn("t", [&]() -> ravel::Task { co_await disk.write("f", 0, "x"); });
  sim.run_until_quiescent();
  bool named = false;
  for (const ravel::TraceEvent& event : sim.trace().events()) {
    if (event.kind == ravel::TraceEventKind::DiskWritten) {
      named = sim.describe(event).find("wal") != std::string::npos;
    }
  }
  CHECK(named);
}
