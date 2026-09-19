#pragma once

// A tiny key-value store that keeps its data in a write-ahead log on a disk,
// and what ravel finds when the power fails. See docs/example-kv.md.

#include <cstdint>
#include <map>
#include <string>

#include "ravel/simulation.hpp"

namespace kv {

enum class Bug {
  None,
  AckBeforeSync,     // Tells the client "stored" before the log is synced.
  TrustPartialTail,  // Recovery believes a record that was cut off mid-write.
};

constexpr int kPuts = 12;
constexpr int kKeys = 3;

// Big enough that a record spans two disk sectors, so a torn write can cut
// one in the middle.
inline std::string value_for(int version) {
  return "v" + std::to_string(version) + std::string(600, '.');
}

inline int version_of(const std::string& value) {
  if (value.size() < 2 || value[0] != 'v') return -1;
  const std::size_t digits = value.find_first_not_of("0123456789", 1);
  if (digits == 1) return -1;
  const int version = std::stoi(value.substr(1, digits - 1));
  return value == value_for(version) ? version : -1;  // -1: not a value anyone wrote.
}

struct State {
  std::map<std::string, int> acknowledged;  // key -> newest version the client was told is stored
  std::map<std::string, std::string> recovered;  // key -> value found in the log after the crash
  bool recovery_done = false;
};

// [kv_replay]
// Rebuilds the key-value map from the log: replay every record, in order.
inline std::map<std::string, std::string> replay_log(const std::string& log, bool trust_partial_tail) {
  std::map<std::string, std::string> store;
  std::size_t start = 0;
  while (start < log.size()) {
    const std::size_t end = log.find('\n', start);
    const bool complete = end != std::string::npos;
    if (!complete && !trust_partial_tail) break;  // A record cut off by the crash: ignore it.

    const std::string line = log.substr(start, (complete ? end : log.size()) - start);
    const std::size_t key_end = line.find(' ', 4);  // "PUT <key> <value>"
    if (line.rfind("PUT ", 0) == 0 && key_end != std::string::npos) {
      store[line.substr(4, key_end - 4)] = line.substr(key_end + 1);
    }
    if (!complete) break;
    start = end + 1;
  }
  return store;
}
// [/kv_replay]

inline ravel::SimulationSetup setup(Bug bug) {
  return [bug](ravel::Simulation& sim) {
    State& state = sim.make_state<State>();
    ravel::Disk& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 5});

    // [kv_server]
    // The server appends each put to the log and tells the client it is stored.
    sim.scheduler().spawn("server", [&sim, &state, &disk, bug]() -> ravel::Task {
      const std::uint64_t boot = disk.crash_count();  // If the power goes, this process is gone.
      const auto alive = [&disk, boot] { return disk.crash_count() == boot; };
      const auto ok = [&alive](ravel::DiskStatus status) {
        return status == ravel::DiskStatus::Ok && alive();
      };

      std::uint64_t log_size = 0;
      for (int version = 1; version <= kPuts; ++version) {
        const std::string key = "k" + std::to_string(version % kKeys);
        const std::string record = "PUT " + key + " " + value_for(version) + "\n";

        if (!ok(co_await disk.write("wal", log_size, record))) co_return;
        log_size += record.size();

        if (bug != Bug::AckBeforeSync && !ok(co_await disk.sync("wal"))) co_return;
        state.acknowledged[key] = version;  // "Stored": the client may rely on this.
        if (bug == Bug::AckBeforeSync && !ok(co_await disk.sync("wal"))) co_return;
      }
    });
    // [/kv_server]

    // [kv_recovery]
    // The power fails at some point; a moment later the machine boots and
    // rebuilds the store from whatever is in the log.
    sim.scheduler().spawn("power_cut", [&sim, &state, &disk, bug]() -> ravel::Task {
      co_await sim.scheduler().sleep(sim.rng().next_between(20, 120));
      disk.crash();
      co_await sim.scheduler().sleep(10);
      const ravel::ReadResult log = co_await disk.read("wal", 0, 1 << 20);
      state.recovered = replay_log(log.data, /*trust_partial_tail=*/bug == Bug::TrustPartialTail);
      state.recovery_done = true;
    });
    // [/kv_recovery]

    // [kv_invariants]
    // Recovery must not invent data: every value it finds is one that was written.
    sim.add_invariant("recovery_invents_nothing", [&state] {
      for (const auto& [key, value] : state.recovered) {
        const int version = version_of(value);
        if (version < 1 || version > kPuts || "k" + std::to_string(version % kKeys) != key) return false;
      }
      return true;
    });

    // Once the client was told "stored", the data must survive the crash.
    sim.add_invariant("acknowledged_puts_survive", [&state] {
      if (!state.recovery_done) return false;
      for (const auto& [key, version] : state.acknowledged) {
        const auto found = state.recovered.find(key);
        if (found == state.recovered.end() || version_of(found->second) < version) return false;
      }
      return true;
    });
    // [/kv_invariants]
  };
}

}  // namespace kv
