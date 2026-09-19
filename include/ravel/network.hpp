#pragma once

#include <coroutine>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include "ravel/clock.hpp"
#include "ravel/rng.hpp"
#include "ravel/scheduler.hpp"
#include "ravel/trace.hpp"

namespace ravel {

using ChannelId = std::size_t;
using Message = std::string;

// Injectable fault behavior for a Channel. Every decision (drop? how much
// latency?) is drawn from the Simulation's VirtualRng, so it replays
// identically for a given seed.
struct FaultSpec {
  double loss_probability = 0.0;  // In [0, 1]: chance that a message is dropped.
  VirtualClock::Tick latency_min = 0;
  VirtualClock::Tick latency_max = 0;  // Delay is uniform in [min, max].
  // Off: messages arrive in the order they were sent (delays are raised so
  // none overtakes an earlier one). On: a message with a short delay may
  // overtake an earlier one with a long delay.
  bool allow_reorder = false;
};

// A virtual, one-way, in-process channel between two named endpoints. There
// are no real sockets: send() schedules delivery on the Scheduler, so a run
// never depends on actual network conditions.
//
//   channel.send("ping");                           // never blocks
//   ravel::Message m = co_await channel.receive();  // blocks until one arrives
//
// At most one task may wait in receive() at a time.
class Channel {
 public:
  Channel(ChannelId id, std::string from, std::string to, FaultSpec fault,
          Scheduler& scheduler, VirtualRng& rng, Trace& trace);

  // Pending deliveries refer to this object, so it must never move.
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  const std::string& from() const noexcept { return from_; }
  const std::string& to() const noexcept { return to_; }
  const FaultSpec& fault() const noexcept { return fault_; }

  // Applies the fault spec: the message is dropped, or delivered after a
  // random delay.
  void send(Message message);

  class ReceiveAwaiter {
   public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const { channel_.wait_for_message(); }
    Message await_resume() const { return channel_.take_message(); }

   private:
    friend class Channel;
    explicit ReceiveAwaiter(Channel& channel) noexcept : channel_(channel) {}
    Channel& channel_;
  };

  [[nodiscard]] ReceiveAwaiter receive() noexcept { return ReceiveAwaiter(*this); }

 private:
  void deliver(Message message);
  void wait_for_message();
  Message take_message();
  void record(TraceEventKind kind) { trace_.record({scheduler_.now(), id_, kind}); }

  ChannelId id_;
  std::string from_;
  std::string to_;
  FaultSpec fault_;
  Scheduler& scheduler_;
  VirtualRng& rng_;
  Trace& trace_;

  std::deque<Message> inbox_;                // Delivered, not yet received.
  std::optional<TaskId> waiting_receiver_;   // Task blocked in receive().
  VirtualClock::Tick last_delivery_at_ = 0;  // Keeps order when reordering is off.
};

}  // namespace ravel
