#pragma once

// Raft (Ongaro and Ousterhout, "In Search of an Understandable Consensus
// Algorithm") on ravel: leader election and log replication across a small
// cluster, with each node persisting its state to a virtual disk. Nodes crash
// and reboot, the network delays, reorders and drops messages, and disks lose
// or tear whatever was not synced.
//
// It is a working implementation, not a sketch, and it is meant to be read: the
// protocol lives in Node's handlers, which follow the paper's rules one by one.
// Safety is checked as the run goes:
//   * election safety:        at most one leader is elected in any term;
//   * state machine safety:   no two nodes apply different entries at the same
//                             index;
//   * leader completeness:    a new leader holds every entry any node applied.
//
// Bug lets you break it on purpose, in ways real implementations have been
// broken, so ravel has something to find:
//   * SkipFileSync:       the state file is renamed into place before its data
//                         was synced, so a crash can leave an empty file;
//   * SkipDirectorySync:  the rename is never made durable, so a crash can
//                         bring back an older state file;
//   * IgnoreLogUpToDateCheck: a node grants its vote to a candidate whose log
//                         is behind its own.
// The first two make a node forget its term, vote or log after a reboot, so it
// can vote twice in one term or lose entries it acknowledged. SkipFileSync shows
// up on about a third of seeds. SkipDirectorySync is far rarer (roughly 1 seed in
// 600): the next sync of the temporary file happens to commit the previous
// rename, so only a crash right after a state change loses it. That is exactly
// the kind of bug that survives testing by hand.

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ravel/simulation.hpp"

namespace raft {

enum class Bug { None, SkipFileSync, SkipDirectorySync, IgnoreLogUpToDateCheck };

inline const char* to_string(Bug bug) {
  switch (bug) {
    case Bug::None: return "none";
    case Bug::SkipFileSync: return "skip-file-sync";
    case Bug::SkipDirectorySync: return "skip-directory-sync";
    case Bug::IgnoreLogUpToDateCheck: return "ignore-log-up-to-date-check";
  }
  return "?";
}

struct Config {
  Bug bug = Bug::None;
  int nodes = 3;
  int crashes = 8;                 // How many times a random node loses power.
  double message_loss = 0.05;
  ravel::VirtualClock::Tick run_for = 4000;

  ravel::VirtualClock::Tick heartbeat_interval = 50;
  ravel::VirtualClock::Tick election_timeout_min = 150;
  ravel::VirtualClock::Tick election_timeout_max = 300;
};

struct Entry {
  int term = 0;
  int value = 0;
  bool operator==(const Entry&) const = default;
};

// What the checkers see: filled in by the nodes as the run goes.
struct History {
  std::map<int, int> leader_of_term;  // term -> node elected in it
  std::map<int, Entry> applied;       // log index -> the entry first applied there
  std::vector<std::string> election_violations;
  std::vector<std::string> state_machine_violations;
  std::vector<std::string> completeness_violations;
  int leaders_elected = 0;
  int entries_applied = 0;  // Highest log index anyone applied.
};

// One Raft server. Handlers change state and queue outgoing messages; the run
// loop makes the changes durable before any message that depends on them is
// sent, as the paper requires.
class Node {
 public:
  Node(int id, const Config& config, ravel::Simulation& sim, History& history,
       ravel::Channel& inbox, ravel::Disk& disk)
      : id_(id), config_(config), sim_(sim), history_(history), inbox_(inbox), disk_(disk) {
    next_index_.assign(static_cast<std::size_t>(config.nodes), 1);
    match_index_.assign(static_cast<std::size_t>(config.nodes), 0);
  }

  void connect(std::vector<ravel::Channel*> inboxes_of_all_nodes) {
    peers_ = std::move(inboxes_of_all_nodes);
  }

  // Simulated power loss: the disk loses what was not synced, and the node
  // notices at its next step, drops its memory and reboots from disk.
  void crash() {
    crash_requested_ = true;
    disk_.crash();
  }

  // The node's whole life: boot, serve until power is lost, stay down a while,
  // repeat.
  ravel::Task run() {
    for (;;) {
      // ---- Boot: forget everything in memory, then read the state file. ----
      boot_ = disk_.crash_count();
      crash_requested_ = false;
      role_ = Role::Follower;
      commit_ = applied_ = votes_ = 0;
      term_ = 0;
      voted_for_ = -1;
      log_.clear();
      outbox_.clear();
      dirty_ = false;
      inbox_.clear_inbox();  // Whatever arrived while the node was down is lost.

      const ravel::ReadResult saved = co_await disk_.read(kStateFile, 0, 1 << 20);
      if (!dead()) {
        if (saved.status == ravel::DiskStatus::Ok) parse(saved.data);
        reset_election_timer();
      }

      // ---- Serve until the power goes. ----
      while (!dead()) {
        const auto message = co_await inbox_.receive_within(time_until_next_timer());
        if (dead()) break;

        if (message) {
          handle(*message);
        } else {
          on_timer();
        }

        if (dirty_) {
          // Replace the state file atomically: write a temporary file, sync it,
          // rename it over the old one, sync the directory. Nothing is sent
          // until this is done; if the power goes first, nothing is sent at all.
          const std::string data = serialize();
          const auto ok = [this](ravel::DiskStatus status) {
            return status == ravel::DiskStatus::Ok && !dead();
          };
          co_await disk_.remove(kTempFile);  // May not exist; the result does not matter.
          bool persisted = !dead();
          if (persisted) persisted = ok(co_await disk_.write(kTempFile, 0, data));
          if (persisted && config_.bug != Bug::SkipFileSync) {
            persisted = ok(co_await disk_.sync(kTempFile));
          }
          if (persisted) persisted = ok(co_await disk_.rename(kTempFile, kStateFile));
          if (persisted && config_.bug != Bug::SkipDirectorySync) {
            persisted = ok(co_await disk_.sync_dir("raft"));
          }
          if (!persisted) break;
          dirty_ = false;
        }

        for (const auto& [to, text] : outbox_) peers_[static_cast<std::size_t>(to)]->send(text);
        outbox_.clear();

        apply_committed();
      }

      // ---- Down: wait a while, then boot again. ----
      co_await sim_.scheduler().sleep(
          sim_.rng().next_between(config_.election_timeout_min / 2, config_.election_timeout_max));
    }
  }

 private:
  enum class Role { Follower, Candidate, Leader };

  static constexpr const char* kStateFile = "raft/state";
  static constexpr const char* kTempFile = "raft/state.tmp";

  using Tick = ravel::VirtualClock::Tick;

  // ---- Small helpers ----------------------------------------------------

  Tick now() const { return sim_.clock().now(); }
  int majority() const { return config_.nodes / 2 + 1; }
  int last_index() const { return static_cast<int>(log_.size()); }
  int term_at(int index) const { return index == 0 ? 0 : log_[static_cast<std::size_t>(index) - 1].term; }
  bool dead() const { return crash_requested_ || disk_.crash_count() != boot_; }

  void send(int to, std::string text) { outbox_.emplace_back(to, std::move(text)); }

  void reset_election_timer() {
    election_deadline_ =
        now() + sim_.rng().next_between(config_.election_timeout_min, config_.election_timeout_max);
  }

  Tick time_until_next_timer() const {
    const Tick due = role_ == Role::Leader ? next_heartbeat_ : election_deadline_;
    return due > now() ? due - now() : 0;
  }

  // ---- Durable state ----------------------------------------------------

  std::string serialize() const {
    std::ostringstream out;
    out << term_ << ' ' << voted_for_ << ' ' << log_.size();
    for (const Entry& entry : log_) out << ' ' << entry.term << ' ' << entry.value;
    return out.str();
  }

  // Anything unreadable (missing, empty, torn) counts as a node with no history.
  void parse(const std::string& text) {
    std::istringstream in(text);
    int term = 0;
    int voted = 0;
    std::size_t count = 0;
    if (!(in >> term >> voted >> count)) return;

    std::vector<Entry> log;
    for (std::size_t i = 0; i < count; ++i) {
      Entry entry;
      if (!(in >> entry.term >> entry.value)) return;
      log.push_back(entry);
    }
    term_ = term;
    voted_for_ = voted;
    log_ = std::move(log);
  }

  // Not part of the protocol: lets tests read the node's state.
 public:
  int term() const { return term_; }
  int commit_index() const { return commit_; }
  int log_size() const { return last_index(); }

 private:
  // ---- The protocol -----------------------------------------------------

  void step_down(int new_term) {
    term_ = new_term;
    voted_for_ = -1;
    role_ = Role::Follower;
    votes_ = 0;
    dirty_ = true;
  }

  void handle(const std::string& text) {
    std::istringstream in(text);
    std::string kind;
    in >> kind;
    if (kind == "RV") {
      on_request_vote(in);
    } else if (kind == "RVR") {
      on_vote_reply(in);
    } else if (kind == "AE") {
      on_append_entries(in);
    } else if (kind == "AER") {
      on_append_reply(in);
    } else if (kind == "C") {
      int value = 0;
      if (in >> value && role_ == Role::Leader) {
        log_.push_back({term_, value});
        dirty_ = true;
        for (int peer = 0; peer < config_.nodes; ++peer) {
          if (peer != id_) send_append_entries(peer);
        }
      }
    }
  }

  void on_timer() {
    if (role_ == Role::Leader) {
      next_heartbeat_ = now() + config_.heartbeat_interval;
      for (int peer = 0; peer < config_.nodes; ++peer) {
        if (peer != id_) send_append_entries(peer);
      }
      return;
    }

    // Election timeout: become a candidate for a new term and ask for votes.
    role_ = Role::Candidate;
    ++term_;
    voted_for_ = id_;
    votes_ = 1;
    dirty_ = true;
    reset_election_timer();
    for (int peer = 0; peer < config_.nodes; ++peer) {
      if (peer == id_) continue;
      send(peer, "RV " + std::to_string(term_) + " " + std::to_string(id_) + " " +
                     std::to_string(last_index()) + " " + std::to_string(term_at(last_index())));
    }
  }

  // RequestVote (paper, figure 2).
  void on_request_vote(std::istringstream& in) {
    int term = 0, candidate = 0, last_log_index = 0, last_log_term = 0;
    in >> term >> candidate >> last_log_index >> last_log_term;
    if (term > term_) step_down(term);

    const int my_last_term = term_at(last_index());
    const bool candidate_is_up_to_date =
        config_.bug == Bug::IgnoreLogUpToDateCheck || last_log_term > my_last_term ||
        (last_log_term == my_last_term && last_log_index >= last_index());

    const bool grant =
        term == term_ && (voted_for_ == -1 || voted_for_ == candidate) && candidate_is_up_to_date;
    if (grant) {
      voted_for_ = candidate;
      dirty_ = true;  // Persisted before the reply goes out.
      reset_election_timer();
    }
    send(candidate, "RVR " + std::to_string(term_) + " " + (grant ? "1 " : "0 ") + std::to_string(id_));
  }

  void on_vote_reply(std::istringstream& in) {
    int term = 0, granted = 0, from = 0;
    in >> term >> granted >> from;
    if (term > term_) {
      step_down(term);
      return;
    }
    if (role_ != Role::Candidate || term != term_ || granted == 0) return;
    if (++votes_ >= majority()) become_leader();
  }

  void become_leader() {
    role_ = Role::Leader;
    ++history_.leaders_elected;

    // Election safety: never two leaders in one term.
    const auto elected = history_.leader_of_term.emplace(term_, id_);
    if (!elected.second && elected.first->second != id_) {
      history_.election_violations.push_back("nodes " + std::to_string(elected.first->second) +
                                             " and " + std::to_string(id_) +
                                             " were both elected in term " + std::to_string(term_));
    }

    // Leader completeness: it holds everything that anyone applied.
    for (const auto& [index, entry] : history_.applied) {
      if (index > last_index() || !(log_[static_cast<std::size_t>(index) - 1] == entry)) {
        history_.completeness_violations.push_back(
            "node " + std::to_string(id_) + " became leader in term " + std::to_string(term_) +
            " without the entry applied at index " + std::to_string(index));
        break;
      }
    }

    next_index_.assign(static_cast<std::size_t>(config_.nodes), last_index() + 1);
    match_index_.assign(static_cast<std::size_t>(config_.nodes), 0);
    next_heartbeat_ = now() + config_.heartbeat_interval;
    for (int peer = 0; peer < config_.nodes; ++peer) {
      if (peer != id_) send_append_entries(peer);
    }
  }

  void send_append_entries(int peer) {
    const int next = next_index_[static_cast<std::size_t>(peer)];
    const int prev_index = next - 1;
    constexpr int kMaxEntriesPerMessage = 8;
    const int count = std::min(kMaxEntriesPerMessage, last_index() - prev_index);

    std::ostringstream out;
    out << "AE " << term_ << ' ' << id_ << ' ' << prev_index << ' ' << term_at(prev_index) << ' '
        << commit_ << ' ' << count;
    for (int i = 0; i < count; ++i) {
      const Entry& entry = log_[static_cast<std::size_t>(prev_index + i)];
      out << ' ' << entry.term << ' ' << entry.value;
    }
    send(peer, out.str());
  }

  // AppendEntries (paper, figure 2).
  void on_append_entries(std::istringstream& in) {
    int term = 0, leader = 0, prev_index = 0, prev_term = 0, leader_commit = 0, count = 0;
    in >> term >> leader >> prev_index >> prev_term >> leader_commit >> count;
    std::vector<Entry> entries;
    for (int i = 0; i < count; ++i) {
      Entry entry;
      in >> entry.term >> entry.value;
      entries.push_back(entry);
    }

    if (term > term_) step_down(term);
    if (term < term_) {
      send(leader, "AER " + std::to_string(term_) + " 0 " + std::to_string(id_) + " 0");
      return;
    }

    role_ = Role::Follower;  // A leader exists for this term.
    reset_election_timer();

    if (prev_index > last_index() || term_at(prev_index) != prev_term) {
      send(leader, "AER " + std::to_string(term_) + " 0 " + std::to_string(id_) + " 0");
      return;
    }

    for (int i = 0; i < count; ++i) {
      const int index = prev_index + 1 + i;
      const Entry& entry = entries[static_cast<std::size_t>(i)];
      if (index <= last_index()) {
        if (log_[static_cast<std::size_t>(index) - 1].term == entry.term) continue;  // Already have it.
        log_.resize(static_cast<std::size_t>(index) - 1);  // Conflict: drop it and all after.
      }
      log_.push_back(entry);
      dirty_ = true;
    }

    const int last_new = prev_index + count;
    if (leader_commit > commit_) commit_ = std::min(leader_commit, last_new);
    send(leader, "AER " + std::to_string(term_) + " 1 " + std::to_string(id_) + " " +
                     std::to_string(last_new));
  }

  void on_append_reply(std::istringstream& in) {
    int term = 0, success = 0, from = 0, match = 0;
    in >> term >> success >> from >> match;
    if (term > term_) {
      step_down(term);
      return;
    }
    if (role_ != Role::Leader || term != term_) return;

    const auto peer = static_cast<std::size_t>(from);
    if (success != 0) {
      match_index_[peer] = std::max(match_index_[peer], match);
      next_index_[peer] = match_index_[peer] + 1;
      advance_commit();
    } else {
      next_index_[peer] = std::max(1, next_index_[peer] - 1);
    }
  }

  // An entry is committed once a majority holds it, provided it is from the
  // leader's own term (figure 8 of the paper explains why).
  void advance_commit() {
    for (int index = last_index(); index > commit_; --index) {
      if (term_at(index) != term_) continue;
      int holders = 1;  // This node.
      for (int peer = 0; peer < config_.nodes; ++peer) {
        if (peer != id_ && match_index_[static_cast<std::size_t>(peer)] >= index) ++holders;
      }
      if (holders >= majority()) {
        commit_ = index;
        return;
      }
    }
  }

  // The state machine is the log itself: applying an entry records it, and the
  // checker confirms every node applies the same entry at each index.
  void apply_committed() {
    while (applied_ < commit_) {
      ++applied_;
      const Entry& entry = log_[static_cast<std::size_t>(applied_) - 1];
      const auto recorded = history_.applied.emplace(applied_, entry);
      if (!recorded.second && !(recorded.first->second == entry)) {
        history_.state_machine_violations.push_back("node " + std::to_string(id_) +
                                                    " applied a different entry at index " +
                                                    std::to_string(applied_));
      }
      history_.entries_applied = std::max(history_.entries_applied, applied_);
    }
  }

  int id_;
  Config config_;
  ravel::Simulation& sim_;
  History& history_;
  ravel::Channel& inbox_;
  ravel::Disk& disk_;
  std::vector<ravel::Channel*> peers_;

  // Persistent state (also in the state file).
  int term_ = 0;
  int voted_for_ = -1;
  std::vector<Entry> log_;  // Index i of the paper is log_[i - 1].

  // Volatile state.
  Role role_ = Role::Follower;
  int commit_ = 0;
  int applied_ = 0;
  int votes_ = 0;
  std::vector<int> next_index_;
  std::vector<int> match_index_;
  Tick election_deadline_ = 0;
  Tick next_heartbeat_ = 0;

  std::vector<std::pair<int, std::string>> outbox_;
  bool dirty_ = false;  // Persistent state changed and is not yet on disk.
  bool crash_requested_ = false;
  std::uint64_t boot_ = 0;  // Disk crash count when this life began.
};

// Builds a cluster: `config.nodes` servers, a client that keeps proposing
// values, and a chaos task that cuts power to random nodes. The run stops at
// `config.run_for` and the safety checks are applied then.
//
// Pass `history` only for a single run you want to inspect afterwards; runs
// that share one would race.
inline ravel::SimulationSetup setup(Config config, std::shared_ptr<History> history = nullptr) {
  return [config, history](ravel::Simulation& sim) {
    History& record = history ? *history : sim.make_state<History>();

    const ravel::FaultSpec network{.loss_probability = config.message_loss,
                                   .latency_min = 1,
                                   .latency_max = 15,
                                   .allow_reorder = true};
    std::vector<ravel::Channel*> inboxes;
    std::vector<ravel::Disk*> disks;
    for (int i = 0; i < config.nodes; ++i) {
      inboxes.push_back(&sim.add_channel("cluster", "node" + std::to_string(i), network));
      disks.push_back(&sim.add_disk("disk" + std::to_string(i), {.latency_min = 1, .latency_max = 5}));
    }

    auto& nodes = sim.make_state<std::vector<std::unique_ptr<Node>>>();
    for (int i = 0; i < config.nodes; ++i) {
      nodes.push_back(std::make_unique<Node>(i, config, sim, record, *inboxes[static_cast<std::size_t>(i)],
                                             *disks[static_cast<std::size_t>(i)]));
      nodes.back()->connect(inboxes);
    }
    for (int i = 0; i < config.nodes; ++i) {
      Node* node = nodes[static_cast<std::size_t>(i)].get();
      sim.scheduler().spawn("node" + std::to_string(i), [node] { return node->run(); });
    }

    // A client that proposes a new value every 60 ticks. It hands each to every
    // node; only the leader (if there is one) takes it.
    sim.scheduler().spawn("client", [&sim, inboxes]() -> ravel::Task {
      co_await sim.scheduler().sleep(400);
      for (int value = 1;; ++value) {
        for (ravel::Channel* inbox : inboxes) inbox->send("C " + std::to_string(value));
        co_await sim.scheduler().sleep(60);
      }
    });

    // Power cuts at random times, to random nodes.
    sim.scheduler().spawn("chaos", [&sim, &nodes, config]() -> ravel::Task {
      for (int crash = 0; crash < config.crashes; ++crash) {
        co_await sim.scheduler().sleep(sim.rng().next_between(200, 800));
        nodes[sim.rng().next_below(nodes.size())]->crash();
      }
    });

    sim.add_invariant("election_safety", [&record] { return record.election_violations.empty(); });
    sim.add_invariant("state_machine_safety",
                      [&record] { return record.state_machine_violations.empty(); });
    sim.add_invariant("leader_completeness",
                      [&record] { return record.completeness_violations.empty(); });
  };
}

inline ravel::SimulationOptions options(const Config& config) {
  ravel::SimulationOptions simulation;
  simulation.time_limit = config.run_for;  // Raft never goes quiet by itself.
  return simulation;
}

}  // namespace raft
