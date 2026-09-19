#include "ravel/network.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ravel {

Channel::Channel(ChannelId id, std::string from, std::string to, FaultSpec fault,
                 Scheduler& scheduler, VirtualRng& rng, Trace& trace)
    : id_(id),
      from_(std::move(from)),
      to_(std::move(to)),
      fault_(fault),
      scheduler_(scheduler),
      rng_(rng),
      trace_(trace) {
  if (!(fault_.loss_probability >= 0.0 && fault_.loss_probability <= 1.0)) {
    throw std::invalid_argument("FaultSpec: loss_probability must be in [0, 1]");
  }
  if (fault_.latency_min > fault_.latency_max) {
    throw std::invalid_argument("FaultSpec: latency_min must not exceed latency_max");
  }
}

VirtualClock::Tick Channel::draw_delay() {
  const VirtualClock::Tick span = fault_.latency_max - fault_.latency_min;
  if (span == 0) return fault_.latency_min;
  // span + 1 outcomes; only the full 64-bit range would overflow that.
  const bool full_range = span == std::numeric_limits<VirtualClock::Tick>::max();
  return fault_.latency_min + (full_range ? rng_.next_u64() : rng_.next_below(span + 1));
}

void Channel::send(Message message) {
  record(TraceEventKind::MessageSent);

  // A fault-free channel makes no draws, so it cannot disturb the
  // scheduler's choices.
  if (rng_.chance(fault_.loss_probability)) {
    record(TraceEventKind::MessageDropped);
    return;
  }

  const VirtualClock::Tick now = scheduler_.now();
  VirtualClock::Tick delivery_at = now + draw_delay();
  if (!fault_.allow_reorder) delivery_at = std::max(delivery_at, last_delivery_at_);
  last_delivery_at_ = delivery_at;

  // Equal delivery times run in send order, which keeps a FIFO channel FIFO.
  scheduler_.call_after(delivery_at - now,
                        [this, message = std::move(message)] { deliver(message); });
}

void Channel::deliver(Message message) {
  inbox_.push_back(std::move(message));
  record(TraceEventKind::MessageDelivered);

  if (waiting_receiver_) {
    scheduler_.make_runnable(*waiting_receiver_);
    waiting_receiver_.reset();
  }
}

// Called as the receiving task suspends. If a message is already waiting the
// task just goes back on the run queue, so every receive is a scheduling point.
void Channel::wait_for_message() {
  if (!inbox_.empty()) {
    scheduler_.make_runnable(scheduler_.current_task());
    return;
  }
  if (waiting_receiver_) {
    throw std::logic_error("Channel: a second task is already waiting to receive");
  }
  waiting_receiver_ = scheduler_.current_task();
}

Message Channel::take_message() {
  Message message = std::move(inbox_.front());
  inbox_.pop_front();
  return message;
}

}  // namespace ravel
