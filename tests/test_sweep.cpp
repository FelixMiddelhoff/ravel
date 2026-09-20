#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ravel/sweep.hpp"
#include "testing.hpp"

namespace {

// Passes on every seed.
void healthy(ravel::Simulation& sim) {
  sim.scheduler().spawn("t", [&sim]() -> ravel::Task { co_await sim.scheduler().sleep(5); });
}

// Lost update: fails on some seeds.
void racy(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&counter] { return counter == 3; });
}

struct Outcome {
  int status;
  std::string text;
};

Outcome sweep(const ravel::SimulationSetup& setup, std::vector<std::string> args,
              ravel::SweepDefaults defaults = {}) {
  std::ostringstream out;
  const int status = ravel::run_sweep(out, args, setup, defaults, "my_test");
  return {status, out.str()};
}

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

// A scratch directory, removed afterwards.
class Scratch {
 public:
  Scratch() : path_(std::filesystem::temp_directory_path() / "ravel-sweep-test") {
    std::filesystem::remove_all(path_);
  }
  ~Scratch() { std::filesystem::remove_all(path_); }
  std::string dir(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace

namespace {

void set_environment(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);  // An empty value removes it.
#else
  if (*value == '\0') unsetenv(name); else setenv(name, value, 1);
#endif
}

}  // namespace

TEST(sweep_reports_success_and_exits_zero) {
  const Outcome outcome = sweep(healthy, {"--seeds", "50", "--first-seed", "10"});
  CHECK(outcome.status == 0);
  CHECK(outcome.text == "ok: 50 seeds passed (seeds 10..59)\n");
}

TEST(sweep_reports_a_failure_shrinks_it_and_says_how_to_replay) {
  const Scratch scratch;
  const Outcome outcome = sweep(racy, {"--seeds", "100", "--trace-dir", scratch.dir("out")});
  CHECK(outcome.status == 1);
  CHECK(contains(outcome.text, "FAILED: "));
  CHECK(contains(outcome.text, "of 100 seeds"));
  CHECK(contains(outcome.text, "first failure: seed "));
  CHECK(contains(outcome.text, "invariant 'no_lost_updates' failed"));
  CHECK(contains(outcome.text, "shrunk from "));
  CHECK(contains(outcome.text, "replay it:  my_test --replay "));
}

TEST(sweep_can_replay_a_saved_reproducer) {
  const Scratch scratch;
  const Outcome failing = sweep(racy, {"--seeds", "100", "--trace-dir", scratch.dir("out")});
  CHECK(failing.status == 1);

  // The reproducer path is on the "reproducer:" line.
  const std::string marker = "reproducer: ";
  const std::size_t at = failing.text.find(marker);
  CHECK(at != std::string::npos);
  if (at == std::string::npos) return;
  const std::size_t end = failing.text.find('\n', at);
  const std::string file = failing.text.substr(at + marker.size(), end - at - marker.size());

  const Outcome replayed = sweep(racy, {"--replay", file});
  CHECK(replayed.status == 1);
  CHECK(contains(replayed.text, "FAILED: invariant 'no_lost_updates' failed"));

  // The same reproducer against a system without the bug passes.
  const Outcome fixed = sweep(healthy, {"--replay", file});
  CHECK(fixed.status == 0);
  CHECK(contains(fixed.text, ": ok"));
}

TEST(sweep_no_shrink_skips_the_minimization) {
  const Scratch scratch;
  const Outcome outcome =
      sweep(racy, {"--seeds", "100", "--no-shrink", "--trace-dir", scratch.dir("out")});
  CHECK(outcome.status == 1);
  CHECK(!contains(outcome.text, "shrunk from"));
  CHECK(contains(outcome.text, "replay it:  my_test --seeds 1 --first-seed "));
}

TEST(sweep_checks_determinism) {
  const Outcome good = sweep(healthy, {"--check-determinism", "--seeds", "20", "--threads", "1"});
  CHECK(good.status == 0);
  CHECK(good.text == "deterministic: 20 seeds checked\n");

  const auto leaky = [](ravel::Simulation& sim) {
    static int runs = 0;
    const ravel::VirtualClock::Tick nap = (++runs % 2) ? 5 : 9;
    sim.scheduler().spawn("t", [&sim, nap]() -> ravel::Task { co_await sim.scheduler().sleep(nap); });
  };
  const Outcome bad = sweep(leaky, {"--check-determinism", "--seeds", "3", "--threads", "1"});
  CHECK(bad.status == 1);
  CHECK(contains(bad.text, "NOT deterministic: 3 of 3 seeds have problems"));
  CHECK(contains(bad.text, "seed 0: two runs of the same seed diverged"));
}

TEST(sweep_applies_the_time_limit_flag_and_the_defaults) {
  const auto heartbeat = [](ravel::Simulation& sim) {
    int& beats = sim.make_state<int>(0);
    sim.scheduler().spawn("beat", [&sim, &beats]() -> ravel::Task {
      while (true) {
        co_await sim.scheduler().sleep(10);
        ++beats;
      }
    });
    sim.add_invariant("beat_ten_times", [&beats] { return beats == 10; });
  };

  // Without a limit it would never stop; with one, it passes at exactly 100 ticks.
  ravel::SweepDefaults defaults;
  defaults.simulation.time_limit = 100;
  CHECK(sweep(heartbeat, {"--seeds", "5"}, defaults).status == 0);

  // A flag overrides the default.
  CHECK(sweep(heartbeat, {"--seeds", "5", "--time-limit", "50"}, defaults).status == 1);
}

TEST(sweep_rejects_a_bad_command_line_with_status_two) {
  for (const std::vector<std::string>& args :
       {std::vector<std::string>{"--bogus"}, {"--seeds"}, {"--seeds", "many"}, {"--replay", "no/such/file"}}) {
    const Outcome outcome = sweep(healthy, args);
    CHECK(outcome.status == 2);
    CHECK(contains(outcome.text, "Try 'my_test --help'."));
  }
}

TEST(sweep_help_lists_the_flags) {
  const Outcome outcome = sweep(healthy, {"--help"});
  CHECK(outcome.status == 0);
  for (const char* flag : {"--seeds", "--first-seed", "--threads", "--trace-dir", "--no-shrink",
                           "--time-limit", "--replay", "--check-determinism"}) {
    CHECK(contains(outcome.text, flag));
  }
}

TEST(sweep_rejects_a_number_that_is_too_large) {
  const Outcome outcome = sweep(healthy, {"--seeds", "99999999999999999999999"});
  CHECK(outcome.status == 2);
  CHECK(contains(outcome.text, "is too large"));
}

TEST(sweep_rejects_a_replay_file_that_is_not_a_choice_list) {
  Scratch scratch;
  std::filesystem::create_directories(scratch.dir(""));
  const std::string path = scratch.dir("junk.txt");
  std::ofstream(path) << "not a choice list";
  const Outcome outcome = sweep(healthy, {"--replay", path});
  CHECK(outcome.status == 2);
  CHECK(contains(outcome.text, "not a ravel choice list"));
}

TEST(sweep_accepts_a_shrink_budget) {
  const Outcome outcome = sweep(racy, {"--seeds", "50", "--max-shrink-attempts", "5"});
  CHECK(outcome.status == 1);
  CHECK(contains(outcome.text, "my_test"));
}

TEST(sweep_reads_seeds_from_the_environment) {
  set_environment("RAVEL_SEEDS", "7");
  const Outcome outcome = sweep(healthy, {});
  set_environment("RAVEL_SEEDS", "");
  CHECK(outcome.status == 0);
  CHECK(contains(outcome.text, "7 seeds"));
}

TEST(sweep_main_names_the_program_without_its_directory_or_exe_suffix) {
  std::ostringstream captured;
  std::streambuf* const previous = std::cout.rdbuf(captured.rdbuf());
  char program[] = "C:\\tools\\dir/my_prog.exe";
  char flag[] = "--help";
  char* argv[] = {program, flag};
  const int status = ravel::run_sweep_main(2, argv, healthy);
  std::cout.rdbuf(previous);

  CHECK(status == 0);
  CHECK(contains(captured.str(), "my_prog"));
  CHECK(!contains(captured.str(), "my_prog.exe"));
  CHECK(!contains(captured.str(), "tools"));
  CHECK(!contains(captured.str(), "/my_prog"));
}


TEST(sweep_singular_and_help_short_flag) {
  const Outcome one = sweep(healthy, {"--check-determinism", "--seeds", "1", "--threads", "1"});
  CHECK(one.text == "deterministic: 1 seed checked\n");
  CHECK(sweep(healthy, {"-h"}).status == 0);
}

TEST(sweep_rejects_an_empty_number) {
  const Outcome outcome = sweep(healthy, {"--seeds", ""});
  CHECK(outcome.status == 2);
  CHECK(contains(outcome.text, "needs a non-negative whole number"));
}

TEST(sweep_ignores_an_empty_environment_variable) {
  set_environment("RAVEL_SEEDS", "");
  CHECK(sweep(healthy, {}).status == 0);
}

TEST(sweep_says_when_the_shrink_budget_ran_out) {
  // Many random choices to shrink: ten messages over a lossy link.
  const auto lossy = [](ravel::Simulation& sim) {
    int& received = sim.make_state<int>(0);
    auto& link = sim.add_channel("a", "b", {.loss_probability = 0.5});
    sim.scheduler().spawn("sender", [&link]() -> ravel::Task {
      for (int i = 0; i < 10; ++i) link.send("m");
      co_return;
    });
    sim.scheduler().spawn("receiver", [&link, &received]() -> ravel::Task {
      while (true) {
        co_await link.receive();
        ++received;
      }
    });
    sim.add_invariant("all_delivered", [&received] { return received == 10; });
  };
  const Outcome outcome = sweep(lossy, {"--seeds", "20", "--max-shrink-attempts", "1"});
  CHECK(outcome.status == 1);
  CHECK(contains(outcome.text, "(budget ran out)"));
}

TEST(sweep_shows_only_the_first_five_determinism_problems) {
  const auto leaky = [](ravel::Simulation& sim) {
    static int runs = 0;
    const ravel::VirtualClock::Tick nap = (++runs % 2) ? 5 : 9;
    sim.scheduler().spawn("t", [&sim, nap]() -> ravel::Task { co_await sim.scheduler().sleep(nap); });
  };
  const Outcome outcome = sweep(leaky, {"--check-determinism", "--seeds", "8", "--threads", "1"});
  CHECK(outcome.status == 1);
  CHECK(contains(outcome.text, "... and 3 more"));
}

TEST(sweep_annotates_failures_on_github_actions) {
  set_environment("GITHUB_ACTIONS", "true");
  const Outcome failed = sweep(racy, {"--seeds", "100"});
  const auto leaky = [](ravel::Simulation& sim) {
    static int runs = 0;
    const ravel::VirtualClock::Tick nap = (++runs % 2) ? 5 : 9;
    sim.scheduler().spawn("t", [&sim, nap]() -> ravel::Task { co_await sim.scheduler().sleep(nap); });
  };
  const Outcome nondeterministic =
      sweep(leaky, {"--check-determinism", "--seeds", "2", "--threads", "1"});
  set_environment("GITHUB_ACTIONS", "");

  CHECK(contains(failed.text, "::error title=ravel found a failure::"));
  CHECK(contains(nondeterministic.text, "::error title=ravel found a failure::"));
  CHECK(!contains(sweep(racy, {"--seeds", "100"}).text, "::error"));
}

TEST(sweep_main_copes_with_an_empty_argument_list) {
  std::ostringstream captured;
  std::streambuf* const previous = std::cout.rdbuf(captured.rdbuf());
  const int status = ravel::run_sweep_main(0, nullptr, healthy);
  std::cout.rdbuf(previous);
  CHECK(status == 0);
  CHECK(contains(captured.str(), "passed"));
}
