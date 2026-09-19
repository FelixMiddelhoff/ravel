#include "ravel/simulation.hpp"

#include <cstdio>
#include <fstream>
#include <ostream>
#include <system_error>

#include "ravel/version.hpp"

namespace ravel {

namespace {

constexpr int kTraceFormatVersion = 1;

// Escapes `text` as the inside of a JSON string literal.
std::string json_escape(const std::string& text) {
  std::string escaped;
  for (const char c : text) {
    switch (c) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char code[8];
          std::snprintf(code, sizeof code, "\\u%04x", static_cast<unsigned>(c));
          escaped += code;
        } else {
          escaped += c;
        }
    }
  }
  return escaped;
}

}  // namespace

Simulation::Simulation(std::uint64_t seed, SimulationOptions options)
    : seed_(seed),
      options_(std::move(options)),
      rng_(options_.replay_choices ? VirtualRng::replaying(*options_.replay_choices)
                                   : VirtualRng(seed)),
      scheduler_(clock_, rng_, trace_) {}

Channel& Simulation::add_channel(std::string from, std::string to, FaultSpec fault) {
  return channels_.emplace_back(channels_.size(), std::move(from), std::move(to), fault,
                                scheduler_, rng_, trace_);
}

Disk& Simulation::add_disk(std::string name, DiskFaultSpec fault) {
  return disks_.emplace_back(disks_.size(), std::move(name), fault, scheduler_, rng_, trace_);
}

void Simulation::add_invariant(std::string name, InvariantFn invariant) {
  invariants_.push_back({std::move(name), std::move(invariant)});
}

std::string Simulation::first_failed_invariant() const {
  for (const NamedInvariant& invariant : invariants_) {
    try {
      if (!invariant.check()) return "invariant '" + invariant.name + "' failed";
    } catch (...) {
      return "invariant '" + invariant.name + "' threw";
    }
  }
  return {};
}

std::string Simulation::subject_name(const TraceEvent& event) const {
  switch (subject_of(event.kind)) {
    case TraceSubject::Task:
      return scheduler_.task_name(event.subject);
    case TraceSubject::Channel: {
      const Channel& channel = channels_.at(event.subject);
      return channel.from() + "->" + channel.to();
    }
    case TraceSubject::Disk:
      return disks_.at(event.subject).name();
  }
  return {};
}

void Simulation::write_trace(std::ostream& out) const {
  out << R"({"format":"ravel-trace","trace_version":)" << kTraceFormatVersion
      << R"(,"ravel_version":")" << version_string() << R"(","seed":)" << seed_ << "}\n";

  std::size_t step = 0;
  for (const TraceEvent& event : trace_.events()) {
    out << R"({"step":)" << step++ << R"(,"time":)" << event.time << R"(,"kind":")"
        << to_string(event.kind) << R"(","id":)" << event.subject << R"(,"name":")"
        << json_escape(subject_name(event)) << "\"}\n";
  }
}

std::string Simulation::dump_trace() const {
  std::error_code error;
  std::filesystem::create_directories(options_.trace_dir, error);

  const std::string kind = options_.replay_choices ? ".replay" : "";
  const auto path =
      options_.trace_dir / ("ravel-seed-" + std::to_string(seed_) + kind + ".trace.jsonl");
  std::ofstream file(path);
  if (error || !file) return {};

  write_trace(file);
  file.flush();
  return file ? path.string() : std::string();
}

Result Simulation::run_until_quiescent() {
  const RunReport report = scheduler_.run_until_quiescent(options_.max_steps);

  Result result;
  result.seed = seed_;
  result.steps = report.steps;
  result.trace_digest = trace_.digest();
  result.failure =
      report.status == RunStatus::Completed ? first_failed_invariant() : report.failure;
  result.ok = result.failure.empty();

  if (!result.ok && !options_.trace_dir.empty()) {
    result.trace_path = dump_trace();
    if (result.trace_path.empty()) {
      result.failure += " (trace could not be written to " + options_.trace_dir.string() + ")";
    }
  }
  return result;
}

}  // namespace ravel
