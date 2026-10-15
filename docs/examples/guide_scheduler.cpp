#include <portals/scheduler.hpp>
#include <chrono>
#include <iostream>

int main() {
  using namespace portals;
  using namespace std::chrono;

  auto scheduler = Scheduler::create();

  if (!scheduler) {
    return 1;
  }

  auto scope = (*scheduler)->makeScope();

  if (!scope) {
    return 1;
  }

  constexpr auto DELAY = seconds(1);
  const auto deadline = duration_cast<milliseconds>(steady_clock::now().time_since_epoch() + DELAY);
  bool ready = false;
  auto timer = (*scheduler)->at(*scope, static_cast<SchedulerTime>(deadline.count()),
                               [&ready] {
                                 ready = true;
                               });

  if (!timer) {
    return 1;
  }

  while (!ready) {
    if (!(*scheduler)->waitFor(duration_cast<milliseconds>(DELAY)) || !(*scheduler)->dispatch()) {
      return 1;
    }
  }

  std::cout << "Ready after one second\n";
}
