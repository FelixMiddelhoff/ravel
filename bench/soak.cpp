// Soak test: runs many seeds across several systems and checks ravel's own
// promises rather than the systems' invariants:
//
//   * a seed run twice gives the identical result and trace digest;
//   * replaying a run's recorded choices reproduces it exactly;
//   * so does replaying them with trailing zeros trimmed (a replay supplies 0
//     past the end of the list);
//   * nothing throws or crashes.
//
// It also reports throughput. Usage: ravel_soak [seeds-per-system] [threads]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quorum_register.hpp"
#include "ravel/shrink.hpp"

namespace {

using System = std::pair<const char*, ravel::SimulationSetup>;

// Tasks read a counter, yield, and write back read+1; the task count is drawn.
void lost_update(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  const int tasks = 2 + static_cast<int>(sim.rng().next_below(8));
  for (int i = 0; i < tasks; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&counter, tasks] { return counter == tasks; });
}

// A lossy, reordering link with timers on both ends.
void lossy_link(ravel::Simulation& sim) {
  int& received = sim.make_state<int>(0);
  auto& link = sim.add_channel(
      "a", "b", {.loss_probability = 0.3, .latency_min = 1, .latency_max = 30, .allow_reorder = true});
  sim.scheduler().spawn("receiver", [&received, &link]() -> ravel::Task {
    while (true) {
      co_await link.receive();
      ++received;
    }
  });
  sim.scheduler().spawn("sender", [&sim, &link]() -> ravel::Task {
    for (int i = 0; i < 30; ++i) {
      link.send("m");
      co_await sim.scheduler().sleep(1 + sim.rng().next_below(5));
    }
  });
  sim.add_invariant("all_delivered", [&received] { return received == 30; });
}

// A disk with latency and both kinds of I/O error, a worker that writes,
// syncs, renames and lists, and a power cut at a random time.
void faulty_disk(ravel::Simulation& sim) {
  ravel::Disk& disk = sim.add_disk("d", {.latency_min = 1,
                                         .latency_max = 8,
                                         .write_error_probability = 0.1,
                                         .sync_error_probability = 0.1});
  sim.scheduler().spawn("worker", [&sim, &disk]() -> ravel::Task {
    for (int i = 0; i < 12; ++i) {
      co_await disk.write("log", static_cast<std::uint64_t>(i) * 600, std::string(600, 'x'));
      if (i % 3 == 0) co_await disk.sync("log");
      co_await disk.read("log", 0, 4096);
      co_await disk.write("meta/tmp", 0, std::string(50, 'm'));
      co_await disk.rename("meta/tmp", "meta/current");
      if (i % 2 == 0) co_await disk.sync_dir("meta");
      co_await disk.list("meta");
    }
  });
  sim.scheduler().spawn("power_cut", [&sim, &disk]() -> ravel::Task {
    co_await sim.scheduler().sleep(1 + sim.rng().next_below(60));
    disk.crash();
  });
  sim.add_invariant("log_fits", [&disk] { return disk.file_size("log") <= 12 * 600; });
}

struct Findings {
  std::atomic<std::uint64_t> problems{0};
  std::atomic<std::uint64_t> runs{0};
  std::atomic<std::uint64_t> steps{0};
};

void report_problem(Findings& findings, const char* system, std::uint64_t seed, const char* what) {
  std::printf("PROBLEM: %s seed %llu: %s\n", system, static_cast<unsigned long long>(seed), what);
  ++findings.problems;
}

// One complete run of `setup`, with the choices it made.
struct Run {
  ravel::Result result;
  ravel::Choices choices;
};

Run run(const ravel::SimulationSetup& setup, std::uint64_t seed) {
  ravel::Simulation sim(seed);
  setup(sim);
  ravel::Result result = sim.run_until_quiescent();
  return {std::move(result), sim.choices()};
}

bool same(const ravel::Result& a, const ravel::Result& b) {
  return a.ok == b.ok && a.failure == b.failure && a.steps == b.steps &&
         a.trace_digest == b.trace_digest;
}

void soak_seed(const System& system, std::uint64_t seed, Findings& findings) {
  const auto& [name, setup] = system;
  try {
    const Run first = run(setup, seed);
    ++findings.runs;
    findings.steps += first.result.steps;

    if (!same(first.result, run(setup, seed).result)) {
      report_problem(findings, name, seed, "running the seed twice gave different results");
    }
    if (!same(first.result, ravel::replay(setup, first.choices))) {
      report_problem(findings, name, seed, "replaying the recorded choices diverged");
    }

    ravel::Choices trimmed = first.choices;
    while (!trimmed.empty() && trimmed.back() == 0) trimmed.pop_back();
    if (!same(first.result, ravel::replay(setup, trimmed))) {
      report_problem(findings, name, seed, "replaying with trailing zeros trimmed diverged");
    }
  } catch (const std::exception& e) {
    report_problem(findings, name, seed, e.what());
  } catch (...) {
    report_problem(findings, name, seed, "unknown exception");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t seeds_per_system = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20'000;
  const unsigned threads = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10))
                                    : std::max(1u, std::thread::hardware_concurrency());

  const std::vector<System> systems = {
      {"lost_update", lost_update},
      {"lossy_link", lossy_link},
      {"faulty_disk", faulty_disk},
      {"quorum_register_buggy", quorum_register::setup(1)},
      {"quorum_register_fixed", quorum_register::setup(2)},
  };

  Findings findings;
  const auto started = std::chrono::steady_clock::now();
  for (const System& system : systems) {
    std::atomic<std::uint64_t> next_seed{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < threads; ++i) {
      workers.emplace_back([&] {
        for (std::uint64_t seed; (seed = next_seed++) < seeds_per_system;) {
          soak_seed(system, seed, findings);
        }
      });
    }
    for (std::thread& worker : workers) worker.join();
    std::printf("%-24s %llu seeds done\n", system.first,
                static_cast<unsigned long long>(seeds_per_system));
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  // Each seed runs four times (original, repeat, two replays); steps count the
  // original run only.
  std::printf("\n%llu seeds, %llu scheduler steps, %.1fs\n",
              static_cast<unsigned long long>(findings.runs.load()),
              static_cast<unsigned long long>(findings.steps.load()), seconds);
  std::printf("%llu problems\n", static_cast<unsigned long long>(findings.problems.load()));
  return findings.problems == 0 ? 0 : 1;
}
