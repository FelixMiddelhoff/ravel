#include "ravel/disk.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>

namespace ravel {

const char* to_string(DiskStatus status) noexcept {
  switch (status) {
    case DiskStatus::Ok: return "Ok";
    case DiskStatus::NoSpace: return "NoSpace";
    case DiskStatus::IoError: return "IoError";
    case DiskStatus::Crashed: return "Crashed";
    case DiskStatus::NotFound: return "NotFound";
  }
  return "Unknown";
}

Disk::Disk(DiskId id, std::string name, DiskFaultSpec fault, Scheduler& scheduler,
           VirtualRng& rng, Trace& trace)
    : id_(id),
      name_(std::move(name)),
      fault_(fault),
      scheduler_(scheduler),
      rng_(rng),
      trace_(trace) {
  const auto is_probability = [](double p) { return p >= 0.0 && p <= 1.0; };
  if (!is_probability(fault_.write_error_probability) ||
      !is_probability(fault_.sync_error_probability)) {
    throw std::invalid_argument("DiskFaultSpec: error probabilities must be in [0, 1]");
  }
  if (fault_.latency_min > fault_.latency_max) {
    throw std::invalid_argument("DiskFaultSpec: latency_min must not exceed latency_max");
  }
}

// ---- Issuing operations -------------------------------------------------
//
// Each call captures what it needs (and the crash epoch) now and does its work
// when its latency has passed, so an operation sees the disk as it is then.

Disk::Operation<DiskStatus> Disk::write(std::string path, std::uint64_t offset,
                                        std::string data) {
  return Operation<DiskStatus>(
      *this, [this, path = std::move(path), offset, data = std::move(data), epoch = epoch_] {
        return finish_write(path, offset, data, epoch);
      });
}

Disk::Operation<ReadResult> Disk::read(std::string path, std::uint64_t offset,
                                       std::uint64_t length) {
  return Operation<ReadResult>(*this,
                               [this, path = std::move(path), offset, length, epoch = epoch_] {
                                 return finish_read(path, offset, length, epoch);
                               });
}

// Only changes that completed before the call are covered, however long the
// sync takes: one that lands during it is not guaranteed durable.
Disk::Operation<DiskStatus> Disk::sync(std::string path) {
  return Operation<DiskStatus>(
      *this, [this, path = std::move(path), horizon = next_sequence_, epoch = epoch_] {
        return finish_sync(path, horizon, epoch);
      });
}

Disk::Operation<DiskStatus> Disk::rename(std::string from, std::string to) {
  return Operation<DiskStatus>(
      *this, [this, from = std::move(from), to = std::move(to), epoch = epoch_] {
        return finish_rename(from, to, epoch);
      });
}

Disk::Operation<DiskStatus> Disk::remove(std::string path) {
  return Operation<DiskStatus>(*this, [this, path = std::move(path), epoch = epoch_] {
    return finish_remove(path, epoch);
  });
}

Disk::Operation<DiskStatus> Disk::sync_dir(std::string dir) {
  return Operation<DiskStatus>(
      *this, [this, dir = std::move(dir), horizon = next_sequence_, epoch = epoch_] {
        return finish_sync_dir(dir, horizon, epoch);
      });
}

Disk::Operation<ListResult> Disk::list(std::string dir) {
  return Operation<ListResult>(*this, [this, dir = std::move(dir), epoch = epoch_] {
    return finish_list(dir, epoch);
  });
}

// ---- Helpers ------------------------------------------------------------

void Disk::apply(std::string& bytes, std::uint64_t offset, const std::string& data) {
  const auto start = static_cast<std::size_t>(offset);
  if (bytes.size() < start + data.size()) bytes.resize(start + data.size(), '\0');
  bytes.replace(start, data.size(), data);
}

void Disk::apply(Names& names, const PendingNameChange& change) {
  switch (change.kind) {
    case NameChange::Create:
      names[change.path] = change.inode;
      break;
    case NameChange::Rename:
      names.erase(change.path);
      names[change.target] = change.inode;
      break;
    case NameChange::Remove:
      names.erase(change.path);
      break;
  }
}

void Disk::refresh_logical_size(Inode& inode) {
  inode.logical_size = inode.durable.size();
  for (const PendingWrite& write : inode.pending) {
    inode.logical_size =
        std::max<std::uint64_t>(inode.logical_size, write.offset + write.data.size());
  }
}

std::string Disk::parent_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

void Disk::make_durable(std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) apply(durable_names_, pending_names_[i]);
  pending_names_.erase(pending_names_.begin(),
                       pending_names_.begin() + static_cast<std::ptrdiff_t>(count));
}

// ---- Inspection ---------------------------------------------------------

std::uint64_t Disk::used_bytes() const {
  std::uint64_t used = 0;
  for (const auto& name : visible_names_) used += inodes_.at(name.second).logical_size;
  return used;
}

std::uint64_t Disk::file_size(const std::string& path) const {
  const auto name = visible_names_.find(path);
  return name == visible_names_.end() ? 0 : inodes_.at(name->second).logical_size;
}

bool Disk::durable_exists(const std::string& path) const {
  return durable_names_.count(path) != 0;
}

std::string Disk::durable_contents(const std::string& path) const {
  const auto name = durable_names_.find(path);
  return name == durable_names_.end() ? std::string() : inodes_.at(name->second).durable;
}

// ---- Completing operations ---------------------------------------------

DiskStatus Disk::finish_write(const std::string& path, std::uint64_t offset,
                              const std::string& data, std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  if (rng_.chance(fault_.write_error_probability)) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  const auto existing = visible_names_.find(path);
  const std::uint64_t old_size =
      existing == visible_names_.end() ? 0 : inodes_.at(existing->second).logical_size;
  const std::uint64_t end = offset + data.size();
  const std::uint64_t new_size = std::max(old_size, end);
  const bool overflowed = end < offset;
  if (overflowed || used_bytes() - old_size + new_size > fault_.capacity_bytes) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::NoSpace;
  }

  InodeId inode_id = 0;
  if (existing == visible_names_.end()) {
    inode_id = next_inode_++;
    visible_names_[path] = inode_id;
    pending_names_.push_back({next_sequence_++, NameChange::Create, path, {}, inode_id});
  } else {
    inode_id = existing->second;
  }

  Inode& inode = inodes_[inode_id];
  inode.pending.push_back({next_sequence_++, offset, data});
  inode.logical_size = new_size;
  record(TraceEventKind::DiskWritten);
  return DiskStatus::Ok;
}

ReadResult Disk::finish_read(const std::string& path, std::uint64_t offset, std::uint64_t length,
                             std::uint64_t epoch) {
  if (epoch != epoch_) return {DiskStatus::Crashed, {}};

  const auto name = visible_names_.find(path);
  if (name == visible_names_.end()) return {DiskStatus::NotFound, {}};
  const Inode& inode = inodes_.at(name->second);
  if (offset >= inode.logical_size) return {};

  // What a read sees: the durable bytes with every pending write laid over them.
  std::string view = inode.durable;
  for (const PendingWrite& write : inode.pending) apply(view, write.offset, write.data);

  const std::uint64_t available = view.size() - offset;
  return {DiskStatus::Ok, view.substr(static_cast<std::size_t>(offset),
                                      static_cast<std::size_t>(std::min(length, available)))};
}

DiskStatus Disk::finish_sync(const std::string& path, std::uint64_t horizon,
                             std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  const auto name = visible_names_.find(path);
  if (name == visible_names_.end()) return DiskStatus::NotFound;
  const InodeId inode_id = name->second;
  Inode& inode = inodes_.at(inode_id);

  // Split off the writes this sync covers; later ones stay pending.
  const auto uncovered = std::stable_partition(
      inode.pending.begin(), inode.pending.end(),
      [horizon](const PendingWrite& write) { return write.sequence >= horizon; });
  std::vector<PendingWrite> covered(std::make_move_iterator(uncovered),
                                    std::make_move_iterator(inode.pending.end()));
  inode.pending.erase(uncovered, inode.pending.end());

  // A failed sync loses the data it was meant to flush, as on real systems
  // where the kernel drops dirty pages after a write-back error.
  if (rng_.chance(fault_.sync_error_probability)) {
    refresh_logical_size(inode);
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  for (const PendingWrite& write : covered) apply(inode.durable, write.offset, write.data);

  // The file's creation becomes durable too: everything up to it, in order.
  std::size_t durable_count = 0;
  for (std::size_t i = 0; i < pending_names_.size(); ++i) {
    const PendingNameChange& change = pending_names_[i];
    if (change.sequence < horizon && change.kind == NameChange::Create &&
        change.inode == inode_id) {
      durable_count = i + 1;
    }
  }
  make_durable(durable_count);

  record(TraceEventKind::DiskSynced);
  return DiskStatus::Ok;
}

DiskStatus Disk::finish_rename(const std::string& from, const std::string& to,
                               std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  if (rng_.chance(fault_.write_error_probability)) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  const auto source = visible_names_.find(from);
  if (source == visible_names_.end()) return DiskStatus::NotFound;
  if (from == to) return DiskStatus::Ok;

  const InodeId inode_id = source->second;
  visible_names_.erase(source);
  visible_names_[to] = inode_id;  // Replaces whatever was there.
  pending_names_.push_back({next_sequence_++, NameChange::Rename, from, to, inode_id});
  record(TraceEventKind::DiskWritten);
  return DiskStatus::Ok;
}

DiskStatus Disk::finish_remove(const std::string& path, std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  if (rng_.chance(fault_.write_error_probability)) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  const auto name = visible_names_.find(path);
  if (name == visible_names_.end()) return DiskStatus::NotFound;

  pending_names_.push_back({next_sequence_++, NameChange::Remove, path, {}, name->second});
  visible_names_.erase(name);
  record(TraceEventKind::DiskWritten);
  return DiskStatus::Ok;
}

DiskStatus Disk::finish_sync_dir(const std::string& dir, std::uint64_t horizon,
                                 std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  if (rng_.chance(fault_.sync_error_probability)) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  std::size_t durable_count = 0;
  for (std::size_t i = 0; i < pending_names_.size(); ++i) {
    const PendingNameChange& change = pending_names_[i];
    if (change.sequence >= horizon) break;  // Made after the sync was issued.
    const bool touches_dir =
        parent_of(change.path) == dir ||
        (change.kind == NameChange::Rename && parent_of(change.target) == dir);
    if (touches_dir) durable_count = i + 1;
  }
  make_durable(durable_count);

  record(TraceEventKind::DiskSynced);
  return DiskStatus::Ok;
}

ListResult Disk::finish_list(const std::string& dir, std::uint64_t epoch) {
  if (epoch != epoch_) return {DiskStatus::Crashed, {}};

  const std::string prefix = dir.empty() ? std::string() : dir + "/";
  std::set<std::string> entries;
  for (const auto& name : visible_names_) {
    if (!name.first.starts_with(prefix)) continue;
    const std::string rest = name.first.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    entries.insert(slash == std::string::npos ? rest : rest.substr(0, slash + 1));
  }

  if (entries.empty() && !dir.empty()) return {DiskStatus::NotFound, {}};
  return {DiskStatus::Ok, std::vector<std::string>(entries.begin(), entries.end())};
}

// ---- Power loss ---------------------------------------------------------

void Disk::crash() {
  ++epoch_;
  record(TraceEventKind::DiskCrashed);

  // Directory changes persist in order: some prefix of the pending ones. The
  // simplest outcome, and what shrinking steers toward, is none of them.
  make_durable(static_cast<std::size_t>(rng_.next_below(pending_names_.size() + 1)));
  pending_names_.clear();
  visible_names_ = durable_names_;

  // Files whose names did not survive are gone, with their data.
  std::set<InodeId> surviving;
  for (const auto& name : durable_names_) surviving.insert(name.second);
  for (auto it = inodes_.begin(); it != inodes_.end();) {
    it = surviving.count(it->first) != 0 ? std::next(it) : inodes_.erase(it);
  }

  for (auto& entry : inodes_) {
    Inode& inode = entry.second;
    for (const PendingWrite& write : inode.pending) {
      if (write.data.empty()) continue;

      // 0: lost (the simplest outcome), 1: torn, 2: survives whole.
      switch (rng_.next_below(3)) {
        case 0:
          break;
        case 1: {
          // Whole sectors reach the platter; the write is cut at a sector edge.
          const std::uint64_t sectors = (write.data.size() + kSectorSize - 1) / kSectorSize;
          const auto kept_bytes = static_cast<std::size_t>(rng_.next_below(sectors) * kSectorSize);
          if (kept_bytes > 0) apply(inode.durable, write.offset, write.data.substr(0, kept_bytes));
          break;
        }
        default:
          apply(inode.durable, write.offset, write.data);
      }
    }
    inode.pending.clear();
    refresh_logical_size(inode);
  }
}

}  // namespace ravel
