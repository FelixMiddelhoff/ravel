// Fuzz target: the choice-list parser and what is done with its output.
//
// Any text may be handed to read_choices. It must either throw
// std::runtime_error or return a list that (a) survives a write/read round
// trip unchanged and (b) can be replayed without crashing.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ravel/shrink.hpp"

namespace {

// Draws of several kinds, so a replayed list is consumed in different shapes.
void draws_some_choices(ravel::Simulation& sim) {
  auto& link = sim.add_channel("a", "b", {.loss_probability = 0.3, .latency_min = 1, .latency_max = 9});
  sim.scheduler().spawn("sender", [&sim, &link]() -> ravel::Task {
    for (int i = 0; i < 4; ++i) {
      link.send("m");
      co_await sim.scheduler().sleep(1 + sim.rng().next_below(5));
    }
  });
  sim.scheduler().spawn("receiver", [&link]() -> ravel::Task {
    while (true) co_await link.receive();
  });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::istringstream in(std::string(reinterpret_cast<const char*>(data), size));
  ravel::Choices choices;
  try {
    choices = ravel::read_choices(in);
  } catch (const std::runtime_error&) {
    return 0;  // Rejected, as documented.
  }

  std::ostringstream text;
  ravel::write_choices(text, choices);
  std::istringstream again(text.str());
  if (ravel::read_choices(again) != choices) std::abort();

  ravel::replay(draws_some_choices, choices);
  return 0;
}
