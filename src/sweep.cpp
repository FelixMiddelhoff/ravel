#include "ravel/sweep.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "ravel/determinism.hpp"
#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"

namespace ravel {

namespace {

// What the command line asked for.
struct Request {
  std::uint64_t seeds = 0;
  std::uint64_t first_seed = 0;
  unsigned threads = 0;
  std::string trace_dir;
  bool shrink = true;
  std::uint64_t max_shrink_attempts = ShrinkOptions{}.max_attempts;
  std::optional<VirtualClock::Tick> time_limit;
  std::optional<std::string> replay_file;
  bool check_determinism = false;
  bool help = false;
};

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint64_t parse_number(const std::string& flag, const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    throw UsageError(flag + " needs a non-negative whole number, got '" + text + "'");
  }
  try {
    return std::stoull(text);
  } catch (const std::exception&) {
    throw UsageError(flag + ": '" + text + "' is too large");
  }
}

// The value of an environment variable, if it is set and not empty.
std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) return std::nullopt;
  std::string value(buffer);
  std::free(buffer);
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr) return std::nullopt;
  std::string value(raw);
#endif
  if (value.empty()) return std::nullopt;
  return value;
}

std::optional<std::uint64_t> number_from_environment(const char* name) {
  const std::optional<std::string> value = environment(name);
  if (!value) return std::nullopt;
  return parse_number(name, *value);
}

Request parse(const std::vector<std::string>& args, const SweepDefaults& defaults) {
  Request request;
  request.seeds = number_from_environment("RAVEL_SEEDS").value_or(defaults.seeds);
  request.first_seed = number_from_environment("RAVEL_FIRST_SEED").value_or(0);
  request.trace_dir = defaults.trace_dir;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& flag = args[i];
    const auto value = [&]() -> const std::string& {
      if (i + 1 >= args.size()) throw UsageError(flag + " needs a value");
      return args[++i];
    };

    if (flag == "--seeds") {
      request.seeds = parse_number(flag, value());
    } else if (flag == "--first-seed") {
      request.first_seed = parse_number(flag, value());
    } else if (flag == "--threads") {
      request.threads = static_cast<unsigned>(parse_number(flag, value()));
    } else if (flag == "--trace-dir") {
      request.trace_dir = value();
    } else if (flag == "--no-shrink") {
      request.shrink = false;
    } else if (flag == "--max-shrink-attempts") {
      request.max_shrink_attempts = parse_number(flag, value());
    } else if (flag == "--time-limit") {
      request.time_limit = parse_number(flag, value());
    } else if (flag == "--replay") {
      request.replay_file = value();
    } else if (flag == "--check-determinism") {
      request.check_determinism = true;
    } else if (flag == "--help" || flag == "-h") {
      request.help = true;
    } else {
      throw UsageError("unknown option '" + flag + "'");
    }
  }
  return request;
}

void print_help(std::ostream& out, const std::string& program, const SweepDefaults& defaults) {
  out << "usage: " << program << " [options]\n"
      << "\n"
      << "Runs the simulation under many seeds and reports any that fail.\n"
      << "\n"
      << "  --seeds N                how many seeds to run (default " << defaults.seeds
      << ", or $RAVEL_SEEDS)\n"
      << "  --first-seed S           the first seed (default 0, or $RAVEL_FIRST_SEED)\n"
      << "  --threads N              worker threads (default: one per hardware thread)\n"
      << "  --trace-dir DIR          where failures are saved (default " << defaults.trace_dir
      << ")\n"
      << "  --no-shrink              do not minimize the first failure\n"
      << "  --max-shrink-attempts N  budget for shrinking\n"
      << "  --time-limit TICKS       stop each run at this virtual time\n"
      << "  --replay FILE            replay a saved .choices file instead of sweeping\n"
      << "  --check-determinism      check the code is deterministic instead of sweeping\n"
      << "  --help                   this text\n"
      << "\n"
      << "Exit status: 0 all passed, 1 a failure was found, 2 bad command line.\n";
}

std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

std::string plural(std::uint64_t count, const char* singular, const char* plural_form) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural_form);
}

// One failure as a line for GitHub's annotations, when running there.
void annotate_for_github(std::ostream& out, const std::string& message) {
  if (environment("GITHUB_ACTIONS") != std::optional<std::string>("true")) return;
  out << "::error title=ravel found a failure::" << message << "\n";
}

int replay_file(std::ostream& out, const Request& request, const SimulationSetup& setup,
                const SimulationOptions& simulation) {
  std::ifstream file(*request.replay_file);
  if (!file) throw UsageError("cannot open '" + *request.replay_file + "'");

  Choices choices;
  try {
    choices = read_choices(file);
  } catch (const std::exception& e) {
    throw UsageError("'" + *request.replay_file + "': " + e.what());
  }

  const Result result = replay(setup, choices, simulation);
  out << "replay of " << forward_slashes(*request.replay_file) << " (" << choices.size()
      << " choices): ";
  if (result.ok) {
    out << "ok\n";
    return 0;
  }
  out << "FAILED: " << result.failure << "\n";
  annotate_for_github(out, "replay of " + forward_slashes(*request.replay_file) +
                               " failed: " + result.failure);
  return 1;
}

int check_determinism_of(std::ostream& out, const Request& request,
                         const SimulationSetup& setup, const SimulationOptions& simulation) {
  DeterminismOptions options;
  options.first_seed = request.first_seed;
  options.seed_count = request.seeds;
  options.threads = request.threads;
  options.simulation = simulation;

  const DeterminismReport report = check_determinism(setup, options);
  if (report.ok()) {
    out << "deterministic: " << plural(report.seeds_checked, "seed", "seeds") << " checked\n";
    return 0;
  }

  out << "NOT deterministic: " << report.problems.size() << " of " << report.seeds_checked
      << " seeds have problems\n";
  const std::size_t shown = std::min<std::size_t>(report.problems.size(), 5);
  for (std::size_t i = 0; i < shown; ++i) {
    out << "  seed " << report.problems[i].seed << ": " << report.problems[i].description << "\n";
  }
  if (report.problems.size() > shown) out << "  ... and " << report.problems.size() - shown << " more\n";
  annotate_for_github(out, "the code under test is not deterministic (first: seed " +
                               std::to_string(report.problems.front().seed) + ")");
  return 1;
}

int sweep(std::ostream& out, const Request& request, const SimulationSetup& setup,
          const SimulationOptions& simulation, const std::string& program) {
  RunnerOptions options;
  options.first_seed = request.first_seed;
  options.seed_count = request.seeds;
  options.threads = request.threads;
  options.shrink_first_failure = request.shrink;
  options.max_shrink_attempts = request.max_shrink_attempts;
  options.simulation = simulation;

  const RunnerReport report = run_seeds(setup, options);
  if (report.ok()) {
    out << "ok: " << plural(report.seeds_run, "seed", "seeds") << " passed (seeds "
        << request.first_seed << ".." << request.first_seed + request.seeds - 1 << ")\n";
    return 0;
  }

  const Result& first = report.failures.front();
  out << "FAILED: " << report.failures.size() << " of " << report.seeds_run << " seeds\n"
      << "first failure: seed " << first.seed << ": " << first.failure << "\n";

  std::string where;
  if (report.shrunk) {
    const ShrinkResult& shrunk = *report.shrunk;
    out << "shrunk from " << plural(shrunk.original_choices.size(), "random choice", "random choices")
        << " (" << shrunk.original.steps << " steps) to " << shrunk.choices.size() << " ("
        << shrunk.minimal.steps << " steps)" << (shrunk.budget_exhausted ? " (budget ran out)" : "")
        << "\n";
    if (!shrunk.choices_path.empty()) {
      where = forward_slashes(shrunk.choices_path);
      out << "reproducer: " << where << "\n"
          << "replay it:  " << program << " --replay " << where << "\n";
    }
    if (!shrunk.minimal.trace_path.empty()) {
      out << "trace:      " << forward_slashes(shrunk.minimal.trace_path) << "\n";
    }
  } else if (!first.trace_path.empty()) {
    out << "trace:      " << forward_slashes(first.trace_path) << "\n"
        << "replay it:  " << program << " --seeds 1 --first-seed " << first.seed << "\n";
  }

  annotate_for_github(out, "seed " + std::to_string(first.seed) + ": " + first.failure +
                               (where.empty() ? "" : " (reproducer: " + where + ")"));
  return 1;
}

}  // namespace

int run_sweep(std::ostream& out, const std::vector<std::string>& args,
              const SimulationSetup& setup, const SweepDefaults& defaults,
              const std::string& program) {
  try {
    const Request request = parse(args, defaults);
    if (request.help) {
      print_help(out, program, defaults);
      return 0;
    }

    SimulationOptions simulation = defaults.simulation;
    simulation.trace_dir = request.trace_dir;
    if (request.time_limit) simulation.time_limit = *request.time_limit;

    if (request.replay_file) return replay_file(out, request, setup, simulation);
    if (request.check_determinism) return check_determinism_of(out, request, setup, simulation);
    return sweep(out, request, setup, simulation, program);
  } catch (const UsageError& e) {
    out << program << ": " << e.what() << "\nTry '" << program << " --help'.\n";
    return 2;
  }
}

int run_sweep_main(int argc, char** argv, const SimulationSetup& setup,
                   const SweepDefaults& defaults) {
  std::string program = argc > 0 ? argv[0] : "test";
  const std::size_t slash = program.find_last_of("/\\");
  if (slash != std::string::npos) program = program.substr(slash + 1);
  if (program.ends_with(".exe") || program.ends_with(".EXE")) {
    program.resize(program.size() - 4);  // Shown in "replay it:" hints; keep them the same everywhere.
  }

  const std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
  return run_sweep(std::cout, args, setup, defaults, program);
}

}  // namespace ravel
