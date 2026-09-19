#include "ravel/network.hpp"

#include <algorithm>
#include <cstdint>
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

void Channel::send(Message message) {
  record(TraceEventKind::MessageSent);

  // A fault-free channel makes no draws, so it cannot disturb the
  // scheduler's choices.
  if (rng_.chance(fault_.loss_probability)) {
    record(TraceEventKind::MessageDropped);
    return;
  }

  const VirtualClock::Tick now = scheduler_.now();
  VirtualClock::Tick delivery_at = now + rng_.next_between(fault_.latency_min, fault_.latency_max);
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
    scheduler_.make_runnable(waiting_receiver_->task);
    waiting_receiver_.reset();
  }
}

// Called as the receiving task suspends. If a message is already waiting the
// task just goes back on the run queue, so every receive is a scheduling point.
void Channel::wait_for_message(std::optional<VirtualClock::Tick> timeout) {
  if (!inbox_.empty()) {
    scheduler_.make_runnable(scheduler_.current_task());
    return;
  }
  if (waiting_receiver_) {
    throw std::logic_error("Channel: a second task is already waiting to receive");
  }

  const std::uint64_t wait_id = ++waits_started_;
  waiting_receiver_ = Waiter{scheduler_.current_task(), wait_id};
  if (timeout) {
    // If the receiver is still waiting on this same wait when the time is up,
    // wake it with nothing. A message that got there first has ended the wait,
    // so the timer finds a different (or no) waiter and does nothing.
    scheduler_.call_after(*timeout, [this, wait_id] {
      if (!waiting_receiver_ || waiting_receiver_->id != wait_id) return;
      scheduler_.make_runnable(waiting_receiver_->task);
      waiting_receiver_.reset();
    });
  }
}

Message Channel::take_message() {
  if (inbox_.empty()) {
    throw std::logic_error("Channel: the inbox was cleared while a receive was about to return");
  }
  Message message = std::move(inbox_.front());
  inbox_.pop_front();
  return message;
}

std::optional<Message> Channel::take_message_if_any() {
  if (inbox_.empty()) return std::nullopt;  // The wait timed out.
  return take_message();
}

}  // namespace ravel
