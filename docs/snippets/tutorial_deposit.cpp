// Runs the deposit system with its bug, finds it, shrinks it, and reports.
//   tutorial_deposit                 the summary
//   tutorial_deposit --trace         only the minimal run's trace
//   tutorial_deposit --choices-file  only the saved reproducer
#include <cstdio>
#include <fstream>
#include <string>

#include "deposit.hpp"
#include "ravel/runner.hpp"
#include "ravel/version.hpp"

namespace {

std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

void print_file(const std::string& path) {
  std::ifstream file(path);
  const std::string version = ravel::version_string();
  for (std::string line; std::getline(file, line);) {
    const auto at = line.find(version);
    if (at != std::string::npos) line.replace(at, version.size(), "x.y.z");  // Keeps the docs stable.
    std::puts(line.c_str());
  }
}

// For the tutorial: with --trace or --choices-file, show that saved file
// instead of the summary. Returns whether it did.
bool print_saved_file(const std::string& mode, const ravel::ShrinkResult& shrunk) {
  if (mode == "--trace") {
    print_file(shrunk.minimal.trace_path);
  } else if (mode == "--choices-file") {
    print_file(shrunk.choices_path);
  } else {
    return false;
  }
  return true;
}

}  // namespace

// [deposit_main]
int main(int argc, char** argv) {
  ravel::RunnerOptions options;
  options.seed_count = 1000;
  options.shrink_first_failure = true;              // Minimize the first failure...
  options.simulation.trace_dir = "ravel-traces";    // ...and save what happened.

  const ravel::RunnerReport report = ravel::run_seeds(deposit_setup(/*dedupe=*/false), options);
  if (report.ok()) return 0;
  const ravel::ShrinkResult& shrunk = *report.shrunk;
  if (print_saved_file(argc > 1 ? argv[1] : "", shrunk)) return 0;

  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  const ravel::Result& first = report.failures.front();
  std::printf("first failure: seed %llu: %s\n", static_cast<unsigned long long>(first.seed),
              first.failure.c_str());
  std::printf("shrunk from %zu random choices (%llu steps) to %zu (%llu steps)\n",
              shrunk.original_choices.size(), static_cast<unsigned long long>(shrunk.original.steps),
              shrunk.choices.size(), static_cast<unsigned long long>(shrunk.minimal.steps));
  std::printf("minimal choices:");
  for (const auto choice : shrunk.choices) std::printf(" %llu", static_cast<unsigned long long>(choice));
  std::printf("\nsaved: %s\n", forward_slashes(shrunk.choices_path).c_str());
  std::printf("trace: %s\n", forward_slashes(shrunk.minimal.trace_path).c_str());
  return 0;
}
// [/deposit_main]
