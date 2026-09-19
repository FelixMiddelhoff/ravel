// Before: a peer monitor that cannot be simulated. It reads the real clock,
// sleeps in real time, starts a thread, and talks to a real socket. Compiled
// here to keep the docs honest, never run.

// [before]
#include <atomic>
#include <chrono>
#include <thread>

struct Socket {                 // Stand-in for a real network connection.
  void send_ping() {}
  bool wait_for_pong(std::chrono::milliseconds) { return true; }
};

class PeerMonitor {
 public:
  PeerMonitor(std::chrono::milliseconds ping_every, std::chrono::milliseconds dead_after)
      : ping_every_(ping_every), dead_after_(dead_after), last_pong_(std::chrono::steady_clock::now()) {}

  void start() { thread_ = std::thread([this] { loop(); }); }
  ~PeerMonitor() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  bool peer_alive() const { return alive_; }

 private:
  void loop() {
    while (!stop_) {
      socket_.send_ping();                                    // Real network.
      if (socket_.wait_for_pong(ping_every_)) {
        last_pong_ = std::chrono::steady_clock::now();        // Real clock.
      }
      if (std::chrono::steady_clock::now() - last_pong_ > dead_after_) alive_ = false;
      std::this_thread::sleep_for(ping_every_);               // Real waiting.
    }
  }

  Socket socket_;
  std::chrono::milliseconds ping_every_, dead_after_;
  std::chrono::steady_clock::time_point last_pong_;
  std::atomic<bool> stop_{false}, alive_{true};
  std::thread thread_;                                        // A real thread.
};
// [/before]

int main() {
  PeerMonitor monitor(std::chrono::milliseconds(100), std::chrono::milliseconds(300));
  return monitor.peer_alive() ? 0 : 1;  // Never started: this only has to compile.
}
