#include "ravel/disk.hpp"

#include <algorithm>
#include <stdexcept>

namespace ravel {

const char* to_string(DiskStatus status) noexcept {
  switch (status) {
    case DiskStatus::Ok: return "Ok";
    case DiskStatus::NoSpace: return "NoSpace";
    case DiskStatus::IoError: return "IoError";
    case DiskStatus::Crashed: return "Crashed";
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

Disk::Operation<DiskStatus> Disk::sync(std::string path) {
  // Only writes that completed before this call are covered, however long the
  // sync takes: a write that lands during it is not guaranteed durable.
  return Operation<DiskStatus>(
      *this, [this, path = std::move(path), horizon = completed_writes_, epoch = epoch_] {
        return finish_sync(path, horizon, epoch);
      });
}

void Disk::apply(std::string& bytes, std::uint64_t offset, const std::string& data) {
  const std::size_t start = static_cast<std::size_t>(offset);
  if (bytes.size() < start + data.size()) bytes.resize(start + data.size(), '\0');
  bytes.replace(start, data.size(), data);
}

void Disk::refresh_logical_size(File& file) {
  file.logical_size = file.durable.size();
  for (const PendingWrite& write : file.pending) {
    file.logical_size = std::max<std::uint64_t>(file.logical_size, write.offset + write.data.size());
  }
}

std::uint64_t Disk::used_bytes() const {
  std::uint64_t used = 0;
  for (const auto& entry : files_) used += entry.second.logical_size;
  return used;
}

std::uint64_t Disk::file_size(const std::string& path) const {
  const auto file = files_.find(path);
  return file == files_.end() ? 0 : file->second.logical_size;
}

std::string Disk::durable_contents(const std::string& path) const {
  const auto file = files_.find(path);
  return file == files_.end() ? std::string() : file->second.durable;
}

DiskStatus Disk::finish_write(const std::string& path, std::uint64_t offset,
                              const std::string& data, std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  if (rng_.chance(fault_.write_error_probability)) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  const auto existing = files_.find(path);
  const std::uint64_t old_size = existing == files_.end() ? 0 : existing->second.logical_size;
  const std::uint64_t end = offset + data.size();
  const std::uint64_t new_size = std::max(old_size, end);
  const bool overflowed = end < offset;
  if (overflowed || used_bytes() - old_size + new_size > fault_.capacity_bytes) {
    record(TraceEventKind::DiskFailed);
    return DiskStatus::NoSpace;
  }

  File& file = files_[path];
  file.pending.push_back({completed_writes_++, offset, data});
  file.logical_size = new_size;
  record(TraceEventKind::DiskWritten);
  return DiskStatus::Ok;
}

ReadResult Disk::finish_read(const std::string& path, std::uint64_t offset, std::uint64_t length,
                             std::uint64_t epoch) {
  if (epoch != epoch_) return {DiskStatus::Crashed, {}};

  const auto found = files_.find(path);
  if (found == files_.end() || offset >= found->second.logical_size) return {};

  // What a read sees: the durable bytes with every pending write laid over them.
  std::string view = found->second.durable;
  for (const PendingWrite& write : found->second.pending) apply(view, write.offset, write.data);

  const std::uint64_t available = view.size() - offset;
  return {DiskStatus::Ok, view.substr(static_cast<std::size_t>(offset),
                                      static_cast<std::size_t>(std::min(length, available)))};
}

DiskStatus Disk::finish_sync(const std::string& path, std::uint64_t horizon,
                             std::uint64_t epoch) {
  if (epoch != epoch_) return DiskStatus::Crashed;

  const auto found = files_.find(path);
  if (found == files_.end()) return DiskStatus::Ok;
  File& file = found->second;

  // Split off the writes this sync covers; later ones stay pending.
  const auto uncovered = std::stable_partition(
      file.pending.begin(), file.pending.end(),
      [horizon](const PendingWrite& write) { return write.sequence >= horizon; });
  std::vector<PendingWrite> covered(std::make_move_iterator(uncovered),
                                    std::make_move_iterator(file.pending.end()));
  file.pending.erase(uncovered, file.pending.end());

  // A failed sync loses the data it was meant to flush, as on real systems
  // where the kernel drops dirty pages after a write-back error.
  if (rng_.chance(fault_.sync_error_probability)) {
    refresh_logical_size(file);
    record(TraceEventKind::DiskFailed);
    return DiskStatus::IoError;
  }

  for (const PendingWrite& write : covered) apply(file.durable, write.offset, write.data);
  record(TraceEventKind::DiskSynced);
  return DiskStatus::Ok;
}

void Disk::crash() {
  ++epoch_;
  record(TraceEventKind::DiskCrashed);

  for (auto& entry : files_) {
    File& file = entry.second;
    for (const PendingWrite& write : file.pending) {
      if (write.data.empty()) continue;

      // 0: lost (the simplest outcome), 1: torn, 2: survives whole.
      switch (rng_.next_below(3)) {
        case 0:
          break;
        case 1: {
          // Whole sectors reach the platter; the write is cut at a sector edge.
          const std::uint64_t sectors = (write.data.size() + kSectorSize - 1) / kSectorSize;
          const std::size_t kept_bytes = rng_.next_below(sectors) * kSectorSize;
          if (kept_bytes > 0) apply(file.durable, write.offset, write.data.substr(0, kept_bytes));
          break;
        }
        default:
          apply(file.durable, write.offset, write.data);
      }
    }
    file.pending.clear();
    refresh_logical_size(file);
  }
}

}  // namespace ravel
