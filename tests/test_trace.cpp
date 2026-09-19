#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ravel/simulation.hpp"
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
      "{\"format\":\"ravel-trace\",\"trace_version\":1,\"ravel_version\":\"0.1.0\",\"seed\":7}\n"
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
