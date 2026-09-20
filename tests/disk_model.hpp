#pragma once

// A reference model of ravel::Disk, and a differential test that runs random
// operation sequences on both and compares everything observable.
//
// The model is deliberately built differently from src/disk.cpp so that a slip
// in one is unlikely to be repeated in the other:
//
//   * Disk keeps a "visible" and a "durable" name table and updates both. The
//     model keeps only the durable table plus the log of directory changes
//     that are not durable yet, and derives the visible names by replaying
//     that log whenever it needs them.
//   * Disk keeps per-file pending writes and a cached logical size. The model
//     rebuilds a file's contents from its stable bytes plus its unsynced
//     writes every time, and takes the size from that.
//   * Disk splits a sync into "covered" and "uncovered" writes by sequence
//     numbers. The model runs one operation at a time, so everything issued
//     earlier has completed and no sequence numbers exist at all.
//
// What the model does NOT re-derive is the order in which random decisions are
// drawn (error checks, then the crash prefix, then each unsynced write in file
// creation order). Both sides must draw identically to see the same crash, so
// the model replays the choices the real run recorded.
//
// Because operations run one at a time, the test does not cover operations that
// overlap in time (a sync racing a write); tests/test_disk.cpp does, by hand.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ravel/simulation.hpp"

namespace ravel::testing {

class DiskModel {
 public:
  DiskModel(DiskFaultSpec spec, VirtualRng& rng) : spec_(spec), rng_(rng) {}

  DiskStatus write(const std::string& path, std::uint64_t offset, const std::string& data) {
    if (rng_.chance(spec_.write_error_probability)) return DiskStatus::IoError;

    const auto id = lookup(path);
    const std::uint64_t old_size = id ? contents(files_.at(*id)).size() : 0;
    const std::uint64_t end = offset + data.size();
    if (end < offset) return DiskStatus::NoSpace;  // The end wrapped around.
    if (used() - old_size + std::max(old_size, end) > spec_.capacity_bytes) {
      return DiskStatus::NoSpace;
    }

    int target;
    if (id) {
      target = *id;
    } else {
      target = next_file_++;
      files_[target];
      log_.push_back({Change::Create, path, "", target});
    }
    files_[target].unsynced.push_back({offset, data});
    return DiskStatus::Ok;
  }

  ReadResult read(const std::string& path, std::uint64_t offset, std::uint64_t length) {
    const auto id = lookup(path);
    if (!id) return {DiskStatus::NotFound, {}};
    const std::string bytes = contents(files_.at(*id));
    if (offset >= bytes.size()) return {DiskStatus::Ok, {}};
    const auto count = std::min<std::uint64_t>(length, std::numeric_limits<std::size_t>::max());
    return {DiskStatus::Ok,
            bytes.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(count))};  // Clamps.
  }

  DiskStatus sync(const std::string& path) {
    const auto id = lookup(path);
    if (!id) return DiskStatus::NotFound;

    File& file = files_.at(*id);
    const std::vector<Patch> flushed = std::move(file.unsynced);
    file.unsynced.clear();
    if (rng_.chance(spec_.sync_error_probability)) return DiskStatus::IoError;  // Data dropped.

    for (const Patch& patch : flushed) overlay(file.stable, patch);

    // The file's creation becomes durable, and every change before it.
    std::size_t upto = 0;
    for (std::size_t i = 0; i < log_.size(); ++i) {
      if (log_[i].kind == Change::Create && log_[i].file == *id) upto = i + 1;
    }
    commit(upto);
    return DiskStatus::Ok;
  }

  DiskStatus rename(const std::string& from, const std::string& to) {
    if (rng_.chance(spec_.write_error_probability)) return DiskStatus::IoError;
    const auto id = lookup(from);
    if (!id) return DiskStatus::NotFound;
    if (from != to) log_.push_back({Change::Rename, from, to, *id});
    return DiskStatus::Ok;
  }

  DiskStatus remove(const std::string& path) {
    if (rng_.chance(spec_.write_error_probability)) return DiskStatus::IoError;
    const auto id = lookup(path);
    if (!id) return DiskStatus::NotFound;
    log_.push_back({Change::Remove, path, "", *id});
    return DiskStatus::Ok;
  }

  DiskStatus sync_dir(const std::string& dir) {
    if (rng_.chance(spec_.sync_error_probability)) return DiskStatus::IoError;
    std::size_t upto = 0;
    for (std::size_t i = 0; i < log_.size(); ++i) {
      const bool touches = parent(log_[i].path) == dir ||
                           (log_[i].kind == Change::Rename && parent(log_[i].target) == dir);
      if (touches) upto = i + 1;
    }
    commit(upto);
    return DiskStatus::Ok;
  }

  ListResult list(const std::string& dir) {
    std::set<std::string> entries;
    for (const auto& [path, id] : visible()) {
      (void)id;
      std::string rest;
      if (dir.empty()) {
        rest = path;
      } else if (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                 path[dir.size()] == '/') {
        rest = path.substr(dir.size() + 1);
      } else {
        continue;
      }
      const std::size_t slash = rest.find('/');
      entries.insert(slash == std::string::npos ? rest : rest.substr(0, slash + 1));
    }
    if (entries.empty() && !dir.empty()) return {DiskStatus::NotFound, {}};
    return {DiskStatus::Ok, {entries.begin(), entries.end()}};
  }

  void crash() {
    commit(static_cast<std::size_t>(rng_.next_below(log_.size() + 1)));
    log_.clear();

    std::set<int> alive;
    for (const auto& [path, id] : durable_) {
      (void)path;
      alive.insert(id);
    }
    for (auto it = files_.begin(); it != files_.end();) {
      it = alive.count(it->first) ? std::next(it) : files_.erase(it);
    }

    for (auto& [id, file] : files_) {  // Oldest file first, writes in completion order.
      (void)id;
      for (const Patch& patch : file.unsynced) {
        if (patch.data.empty()) continue;
        switch (rng_.next_below(3)) {
          case 0:
            break;
          case 1: {
            const std::uint64_t sectors = (patch.data.size() + 511) / 512;
            const std::uint64_t keep = rng_.next_below(sectors) * 512;
            if (keep > 0) overlay(file.stable, {patch.offset, patch.data.substr(0, static_cast<std::size_t>(keep))});
            break;
          }
          default:
            overlay(file.stable, patch);
        }
      }
      file.unsynced.clear();
    }
  }

  std::uint64_t file_size(const std::string& path) const {
    const auto id = lookup(path);
    return id ? contents(files_.at(*id)).size() : 0;
  }

  bool durable_exists(const std::string& path) const { return durable_.count(path) != 0; }

  std::string durable_contents(const std::string& path) const {
    const auto found = durable_.find(path);
    return found == durable_.end() ? std::string() : files_.at(found->second).stable;
  }

  std::uint64_t used_bytes() const { return used(); }

 private:
  struct Patch {
    std::uint64_t offset;
    std::string data;
  };
  struct File {
    std::string stable;           // What survives a crash.
    std::vector<Patch> unsynced;  // Completed writes, oldest first.
  };
  enum class Change { Create, Rename, Remove };
  struct Entry {
    Change kind;
    std::string path;    // Created, old name, or removed.
    std::string target;  // New name of a rename.
    int file;
  };
  using Names = std::map<std::string, int>;

  static void overlay(std::string& bytes, const Patch& patch) {
    const auto offset = static_cast<std::size_t>(patch.offset);
    if (bytes.size() < offset + patch.data.size()) bytes.resize(offset + patch.data.size(), '\0');
    bytes.replace(offset, patch.data.size(), patch.data);
  }

  static std::string contents(const File& file) {
    std::string bytes = file.stable;
    for (const Patch& patch : file.unsynced) overlay(bytes, patch);
    return bytes;
  }

  static std::string parent(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
  }

  static void replay(Names& names, const Entry& entry) {
    switch (entry.kind) {
      case Change::Create: names[entry.path] = entry.file; break;
      case Change::Rename:
        names.erase(entry.path);
        names[entry.target] = entry.file;
        break;
      case Change::Remove: names.erase(entry.path); break;
    }
  }

  // The durable names with every pending change laid over them.
  Names visible() const {
    Names names = durable_;
    for (const Entry& entry : log_) replay(names, entry);
    return names;
  }

  std::optional<int> lookup(const std::string& path) const {
    const Names names = visible();
    const auto found = names.find(path);
    if (found == names.end()) return std::nullopt;
    return found->second;
  }

  std::uint64_t used() const {
    std::uint64_t total = 0;
    for (const auto& [path, id] : visible()) {
      (void)path;
      total += contents(files_.at(id)).size();
    }
    return total;
  }

  // Makes the first `count` pending changes durable.
  void commit(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) replay(durable_, log_[i]);
    log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(count));
  }

  DiskFaultSpec spec_;
  VirtualRng& rng_;
  std::map<int, File> files_;
  Names durable_;
  std::vector<Entry> log_;
  int next_file_ = 1;
};

// ---- The differential test ----------------------------------------------

struct DiskOp {
  enum Kind { Write, Read, Sync, Rename, Remove, SyncDir, List, Crash } kind = Crash;
  std::string path;
  std::string path2;  // Rename target. For SyncDir and List the directory is `path`.
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::string data;
};

inline const std::vector<std::string>& disk_test_paths() {
  static const std::vector<std::string> paths = {"a", "b", "c", "d/x", "d/y", "e/f/g"};
  return paths;
}

inline const std::vector<std::string>& disk_test_dirs() {
  static const std::vector<std::string> dirs = {"", "d", "e", "e/f", "zz"};
  return dirs;
}

struct DiskCase {
  DiskFaultSpec spec;
  std::vector<DiskOp> ops;
};

// A random fault setting and operation list, all derived from `seed`.
inline DiskCase generate_disk_case(std::uint64_t seed, int op_count) {
  VirtualRng rng(seed ^ 0x9e3779b97f4a7c15ULL);
  const auto pick = [&rng](const std::vector<std::string>& from) {
    return from[static_cast<std::size_t>(rng.next_below(from.size()))];
  };
  const std::uint64_t offsets[] = {0, 0, 1, 3, 510, 512, 600, 1500};
  const std::uint64_t sizes[] = {0, 1, 5, 300, 700, 1100};

  DiskCase disk_case;
  const double probabilities[] = {0.0, 0.0, 0.25};
  disk_case.spec.write_error_probability = probabilities[rng.next_below(3)];
  disk_case.spec.sync_error_probability = probabilities[rng.next_below(3)];
  if (rng.next_below(4) == 0) disk_case.spec.capacity_bytes = 1500 + rng.next_below(3000);

  for (int i = 0; i < op_count; ++i) {
    DiskOp op;
    switch (rng.next_below(16)) {
      case 0: case 1: case 2: case 3: case 4: op.kind = DiskOp::Write; break;
      case 5: case 6: op.kind = DiskOp::Read; break;
      case 7: case 8: op.kind = DiskOp::Sync; break;
      case 9: case 10: op.kind = DiskOp::Rename; break;
      case 11: op.kind = DiskOp::Remove; break;
      case 12: case 13: op.kind = DiskOp::SyncDir; break;
      case 14: op.kind = DiskOp::List; break;
      default: op.kind = DiskOp::Crash; break;
    }
    op.path = (op.kind == DiskOp::SyncDir || op.kind == DiskOp::List) ? pick(disk_test_dirs())
                                                                        : pick(disk_test_paths());
    op.path2 = pick(disk_test_paths());
    op.offset = offsets[rng.next_below(8)];
    op.length = 1 + rng.next_below(2000);
    op.data.resize(static_cast<std::size_t>(sizes[rng.next_below(6)]));
    for (char& c : op.data) c = static_cast<char>('A' + rng.next_below(26));
    disk_case.ops.push_back(std::move(op));
  }
  return disk_case;
}

inline std::string describe_op(const DiskOp& op) {
  switch (op.kind) {
    case DiskOp::Write:
      return "write(" + op.path + ", " + std::to_string(op.offset) + ", " +
             std::to_string(op.data.size()) + " bytes)";
    case DiskOp::Read:
      return "read(" + op.path + ", " + std::to_string(op.offset) + ", " +
             std::to_string(op.length) + ")";
    case DiskOp::Sync: return "sync(" + op.path + ")";
    case DiskOp::Rename: return "rename(" + op.path + ", " + op.path2 + ")";
    case DiskOp::Remove: return "remove(" + op.path + ")";
    case DiskOp::SyncDir: return "sync_dir(" + op.path + ")";
    case DiskOp::List: return "list(" + op.path + ")";
    case DiskOp::Crash: return "crash()";
  }
  return "?";
}

// Everything observable about a disk between operations.
template <typename D>
std::string disk_state_line(const D& disk) {
  std::string line = " | used=" + std::to_string(disk.used_bytes());
  for (const std::string& path : disk_test_paths()) {
    line += " " + path + ":" + std::to_string(disk.file_size(path)) +
            (disk.durable_exists(path) ? "D'" : "-'") + disk.durable_contents(path) + "'";
  }
  return line;
}

inline std::string describe_read(const ReadResult& result) {
  return std::string(to_string(result.status)) + " '" + result.data + "'";
}

inline std::string describe_list(const ListResult& result) {
  std::string text = to_string(result.status);
  for (const std::string& name : result.names) text += " " + name;
  return text;
}

// Runs one random case on Disk and on the model. Empty string if they agree,
// otherwise a description of the first difference.
inline std::string differential_disk_test(std::uint64_t seed, int op_count) {
  const DiskCase disk_case = generate_disk_case(seed, op_count);

  std::vector<std::string> real;
  Simulation sim(seed);
  Disk& disk = sim.add_disk("d", disk_case.spec);
  sim.scheduler().spawn("ops", [&]() -> Task {
    for (const DiskOp& op : disk_case.ops) {
      std::string line = describe_op(op) + " -> ";
      switch (op.kind) {
        case DiskOp::Write:
          line += to_string(co_await disk.write(op.path, op.offset, op.data));
          break;
        case DiskOp::Read:
          line += describe_read(co_await disk.read(op.path, op.offset, op.length));
          break;
        case DiskOp::Sync: line += to_string(co_await disk.sync(op.path)); break;
        case DiskOp::Rename: line += to_string(co_await disk.rename(op.path, op.path2)); break;
        case DiskOp::Remove: line += to_string(co_await disk.remove(op.path)); break;
        case DiskOp::SyncDir: line += to_string(co_await disk.sync_dir(op.path)); break;
        case DiskOp::List: line += describe_list(co_await disk.list(op.path)); break;
        case DiskOp::Crash:
          disk.crash();
          line += "done";
          break;
      }
      real.push_back(line + disk_state_line(disk));
    }
  });
  sim.run_until_quiescent();

  VirtualRng replayed = VirtualRng::replaying(sim.choices());
  DiskModel model(disk_case.spec, replayed);
  std::vector<std::string> expected;
  for (const DiskOp& op : disk_case.ops) {
    std::string line = describe_op(op) + " -> ";
    switch (op.kind) {
      case DiskOp::Write: line += to_string(model.write(op.path, op.offset, op.data)); break;
      case DiskOp::Read: line += describe_read(model.read(op.path, op.offset, op.length)); break;
      case DiskOp::Sync: line += to_string(model.sync(op.path)); break;
      case DiskOp::Rename: line += to_string(model.rename(op.path, op.path2)); break;
      case DiskOp::Remove: line += to_string(model.remove(op.path)); break;
      case DiskOp::SyncDir: line += to_string(model.sync_dir(op.path)); break;
      case DiskOp::List: line += describe_list(model.list(op.path)); break;
      case DiskOp::Crash:
        model.crash();
        line += "done";
        break;
    }
    expected.push_back(line + disk_state_line(model));
  }

  if (real.size() != expected.size()) {
    return "seed " + std::to_string(seed) + ": the real run stopped after " +
           std::to_string(real.size()) + " of " + std::to_string(expected.size()) + " operations";
  }
  for (std::size_t i = 0; i < real.size(); ++i) {
    if (real[i] != expected[i]) {
      return "seed " + std::to_string(seed) + ", operation " + std::to_string(i) +
             ":\n  Disk:  " + real[i] + "\n  model: " + expected[i];
    }
  }
  return {};
}

}  // namespace ravel::testing
