#include <portals/property.hpp>
#include <iostream>

int main() {
  using namespace portals;

  auto scheduler = Scheduler::create();

  if (!scheduler) {
    return 1;
  }

  auto tree = WidgetTree::create(**scheduler);

  if (!tree) {
    return 1;
  }

  auto root = (*tree)->root();

  if (!root) {
    return 1;
  }

  auto count = Property<int>::create(**tree, *root, 0, Invalidation::PAINT);

  if (!count) {
    return 1;
  }

  auto before = (*count)->snapshot();
  if (!before) {
    return 1;
  }

  auto update = (*count)->set(1);

  if (!update) {
    return 1;
  }

  std::cout << (*before)->value << " -> " << update->snapshot->value << '\n';
}
