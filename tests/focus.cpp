// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/action.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
}

void *operator new(std::size_t size) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *p = std::malloc(size ? size : 1)) {
    return p;
  }

  throw std::bad_alloc();
}

void operator delete(void *p) noexcept {
  std::free(p);
}

void operator delete(void *p, std::size_t) noexcept {
  std::free(p);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *p) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, std::size_t) noexcept {
  ::operator delete(p);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  ::operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  ::operator delete[](p);
}

namespace {
using namespace portals;
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E> T take(std::expected<T, E> value) {
  if (!value) {
    std::cerr << "fixture failed\n";
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

template <class T> bool error(const std::expected<T, TreeError> &value, TreeError wanted) {
  return !value && value.error() == wanted;
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router;

  explicit Fixture(EventRouterOptions options = {}) : router(take(EventRouter::create(*tree, options))) {
  }

  Node<Widget> child(Node<Widget> parent) {
    return take(tree->emplace<Widget>(parent));
  }

  ActionBinding bind(Node<Widget> node, bool focusable = true) {
    return take(router->bind(
      node,
      [](Widget &, const RoutedEvent &) {
        return EventReply::IGNORED;
      },
      focusable));
  }

  DispatchResult input(std::string_view bytes) {
    auto decoder = take(InputDecoder::create());
    auto step = take(decoder.next(bytes, 0));

    check(step.event.has_value(), "decoder fixture emits an event");

    return take(router->dispatch(*step.event));
  }

  void arrange(Node<Widget> node, int x, int y, int width, int height) {
    auto rectangle = take(CellRect::from({x, y}, {width, height}));
    auto viewport = take(CellRect::from({0, 0}, {20, 8}));

    check(tree->arrange(node, rectangle, viewport).has_value(), "real tree arrangement succeeds");
  }
};

void activationAndInput() {
  Fixture f;
  auto child = f.child(f.root);
  int calls{};
  std::vector<RoutedEvent> seen;
  auto binding = take(f.router->bind(child, [&](Widget &, const RoutedEvent &event) {
    seen.push_back(event);

    if (const auto *action = std::get_if<ActionEvent>(&event); action && action->action == Action::ACTIVATE) {
      ++calls;
    }

    return EventReply::HANDLED;
  }));

  auto keyboard = f.input("\r");
  auto programmatic = take(f.router->invoke(child, {Action::ACTIVATE}));

  check(calls == 2 && keyboard.status == DispatchStatus::HANDLED && programmatic.status == DispatchStatus::HANDLED,
        "keyboard and programmatic activation share a handler exactly once");
  check(keyboard.target == child.id() && keyboard.handler == child.id(), "result preserves identities");
  check(std::get<ActionEvent>(seen[0]) == std::get<ActionEvent>(seen[1]), "both routes carry the same action");

  f.input(" ");
  check(std::get<TextInputEvent>(seen.back()).scalar == U' ', "space stays text for LineEdit");
  f.input("\x1b[200~hello\x1b[201~");
  check(std::get<PasteInputEvent>(seen.back()).bytes == "hello", "complete owning paste reaches handler");
  f.input("\x1b[1;2D");

  const auto shifted = std::get<ActionEvent>(seen.back());

  check(shifted.action == Action::MOVE_LEFT && shifted.modifiers.shift, "editing action preserves modifiers");

  const auto count = seen.size();

  check(take(f.router->dispatch(InputIssue{})).status == DispatchStatus::NO_TARGET && seen.size() == count,
        "decoder issues do not become widget events");
  take(f.router->dispatch(KeyEvent{InputKey::ENTER, 0, {}, KeyAction::RELEASE}));
  check(std::holds_alternative<KeyEvent>(seen.back()) && calls == 2, "key release does not activate");
}

void traversalAndReturn() {
  Fixture f;
  auto first = f.child(f.root);
  auto group = f.child(f.root);
  auto nested = f.child(group);
  auto last = f.child(f.root);
  auto first_binding = f.bind(first);
  auto group_binding = f.bind(group, false);
  auto nested_binding = f.bind(nested);
  auto last_binding = f.bind(last);

  check(f.input("\t").target == first.id(), "first Tab chooses first preorder node");
  check(f.input("\t").target == nested.id(), "Tab follows structural preorder");
  check(f.input("\t").target == last.id(), "Tab continues after descendants");
  check(f.input("\t").target == first.id(), "Tab wraps");
  check(f.input("\x1b[Z").target == last.id(), "Shift+Tab wraps backwards");

  check(f.tree->setEnabled(group, false).has_value(), "disable ancestor");
  check(f.input("\x1b[Z").target == first.id(), "disabled ancestor excludes descendant");

  check(f.tree->setVisible(first, false).has_value(), "hide focused node");
  check(take(f.router->focused()) == last, "hidden focus returns to most recent eligible node");

  check(f.tree->setEnabled(group, true).has_value(), "reenable ancestor");
  check(f.router->setFocus(nested).has_value(), "explicit focus succeeds without geometry");
  check(f.tree->remove(group).has_value(), "remove focused subtree");
  check(take(f.router->focused()) == last, "removed focus returns to previous live node");

  check(f.tree->setVisible(last, false).has_value(), "hide final focusable node");
  check(take(f.router->focused()).id().session == 0, "no eligible focus has empty identity");
  check(f.input("\r").status == DispatchStatus::NO_TARGET, "keyboard miss is explicit");

  check(f.tree->setVisible(first, true).has_value(), "show first again");
  check(take(f.router->focused()) == first, "recovery chooses remaining live node");
}

void traversalIdentityAndOrder() {
  Fixture f;
  auto retired = f.child(f.root);
  auto group = f.child(f.root);
  auto nested = f.child(group);
  auto last = f.child(f.root);
  auto retiredBinding = f.bind(retired);

  check(f.tree->remove(retired).has_value(), "retire the first slot before reuse");

  auto replacement = f.child(f.root);
  auto lastBinding = f.bind(last);
  auto replacementBinding = f.bind(replacement);
  auto groupBinding = f.bind(group, false);
  auto nestedBinding = f.bind(nested);

  check(replacement.id().slot == retired.id().slot && replacement.id().generation != retired.id().generation,
        "fixture reuses a slot with a new generation");
  check(take(f.router->focused()) == nested, "initial focus follows tree order, not registration or slot order");
  check(take(f.router->traverse()) == last, "forward traversal visits the next sibling after descendants");
  check(take(f.router->traverse()) == replacement, "reused early slot remains the final sibling");
  check(take(f.router->traverse()) == nested, "forward traversal wraps in tree order");
  check(take(f.router->traverse(true)) == replacement, "reverse traversal wraps in tree order");

  replacementBinding.disconnect();

  check(take(f.router->focused()) == nested, "disconnected focus recovers the most recent eligible node");
  check(f.tree->setVisible(group, false).has_value(), "hide ancestor of indexed candidate");
  check(take(f.router->focused()) == last, "hidden ancestor excludes the indexed descendant");
  check(f.tree->setVisible(group, true).has_value(), "restore hidden ancestor");
  check(f.tree->setEnabled(group, false).has_value(), "disable ancestor of indexed candidate");
  check(take(f.router->traverse()) == last, "disabled ancestor excludes the indexed descendant");
  check(f.tree->setEnabled(group, true).has_value(), "restore disabled ancestor");
  check(take(f.router->traverse()) == nested, "restored subtree returns to its structural position");
  check(error(f.router->setFocus(retired), TreeError::STALE_NODE), "reused slot cannot restore a retired identity");

  Fixture foreign;
  auto foreignNode = foreign.child(foreign.root);

  check(error(f.router->setFocus(foreignNode), TreeError::WRONG_SESSION),
        "another tree cannot borrow a registered slot");
}

void traversalAllocationRollback() {
  Fixture f;
  auto first = f.child(f.root);
  auto second = f.child(f.root);
  auto firstBinding = f.bind(first);
  auto secondBinding = f.bind(second);
  int rejected{}, completed{};
  constexpr std::ptrdiff_t MAX_ALLOCATION_ALLOWANCE = 40;

  for (std::ptrdiff_t allowance = 0; allowance < MAX_ALLOCATION_ALLOWANCE; ++allowance) {
    check(f.router->setFocus(first).has_value(), "reset focus before traversal allocation injection");
    take(f.tree->consumeFrameRequest());
    allocation_test::remaining = allowance;

    auto traversed = f.router->traverse();

    allocation_test::remaining = -1;

    if (!traversed) {
      ++rejected;
      check(traversed.error() == TreeError::NO_MEMORY, "failed traversal reports typed allocation error");
      check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "failed traversal does not request paint");
      check(take(f.router->focused()) == first, "failed traversal preserves the previous focus");
    } else {
      ++completed;
      check(*traversed == second, "successful traversal advances exactly once");
    }
  }

  check(rejected > 0 && completed > 0, "traversal allocation sweep reaches both failure and success");
}

void nestedScopes() {
  Fixture f;
  auto before = f.child(f.root);
  auto outer = f.child(f.root);
  auto first = f.child(outer);
  auto inner = f.child(outer);
  auto innerFirst = f.child(inner);
  auto preferred = f.child(inner);
  auto last = f.child(outer);
  auto after = f.child(f.root);
  auto beforeBinding = f.bind(before);
  auto firstBinding = f.bind(first);
  auto innerFirstBinding = f.bind(innerFirst);
  auto preferredBinding = f.bind(preferred);
  auto lastBinding = f.bind(last);
  auto afterBinding = f.bind(after);
  auto outerScope = take(f.router->focusScope(outer));
  auto innerScope = take(f.router->focusScope(inner, preferred));

  check(take(f.router->enterScope(outer)) == first, "outer entry begins in preorder");
  check(take(f.router->traverse()) == innerFirst, "Tab visits nested first control before its default");
  check(take(f.router->traverse()) == preferred, "Tab visits nested default in global preorder");
  check(take(f.router->traverse()) == last, "Tab leaves nested scope without trapping");
  check(take(f.router->traverse()) == after, "Tab leaves outer scope");
  check(take(f.router->traverse(true)) == last, "reverse traversal follows global predecessor");
  check(take(f.router->traverse(true)) == preferred, "reverse traversal visits nested final control");
  check(take(f.router->traverse(true)) == innerFirst, "reverse continues within scope in tree order");
  check(take(f.router->traverse(true)) == first, "reverse exits nested scope");
  check(take(f.router->traverse(true)) == before, "reverse exits outer scope");
  check(take(f.router->traverse()) == first, "forward traversal visits the outer first control");
  check(f.router->setFocus(preferred).has_value(), "explicit nested target focus");
  check(f.router->setFocus(after).has_value(), "programmatic focus can leave all scopes");
  check(take(f.router->enterScope(outer)) == preferred, "outer remembers exact nested descendant");
  take(f.tree->consumeFrameRequest());
  check(take(f.router->enterScope(outer)) == preferred, "reentering exact focus is stable");
  check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "unchanged scope entry requests no redraw");
  check(f.tree->setVisible(preferred, false).has_value(), "hide remembered/default descendant");
  check(take(f.router->enterScope(inner)) == innerFirst, "hidden last and default fall back inside scope");
  check(f.tree->setEnabled(inner, false).has_value(), "disable nested scope ancestor");
  check(take(f.router->focused()) == first, "disabled nested scope recovers within parent scope");
  check(f.tree->setEnabled(inner, true).has_value(), "restore nested scope");
  check(f.tree->setVisible(preferred, true).has_value(), "restore nested default");
  check(f.router->focusDefault(last).has_value(), "nearest scope default can change");
  check(error(f.router->focusDefault(before), TreeError::INVALID_ARGUMENT), "default requires enclosing scope");
  check(error(f.router->focusScope(inner), TreeError::INVALID_ARGUMENT), "duplicate scope rejected");
  check(error(f.router->focusScope(last, before), TreeError::INVALID_ARGUMENT), "foreign subtree default rejected");
  check(f.router->setFocus(preferred).has_value(), "remember target before retiring its generation");
  check(f.tree->remove(preferred).has_value(), "retire remembered scope identity");
  auto replacement = f.child(inner);
  auto replacementBinding = f.bind(replacement);

  check(replacement.id().slot == preferred.id().slot && replacement.id() != preferred.id(),
        "scope fixture reuses exact slot with new generation");
  check(take(f.router->enterScope(inner)) == innerFirst, "stale default cannot focus reused slot");

  std::thread worker([&] {
    check(error(f.router->enterScope(inner), TreeError::WRONG_THREAD), "scope entry checks UI affinity");
    innerScope.disconnect();
  });

  worker.join();
  check(error(f.router->enterScope(inner), TreeError::INVALID_ARGUMENT), "worker disconnect removes scope admission");
  check(f.tree->remove(outer).has_value(), "scope root removal retires descendants");
  check(error(f.router->enterScope(outer), TreeError::STALE_NODE), "scope root identity remains exact");
  outerScope.disconnect();
}

void scopeTraversalCycles() {
  Fixture f;
  auto group = f.child(f.root);
  auto first = f.child(group);
  auto nested = f.child(group);
  auto second = f.child(nested);
  auto outside = f.child(f.root);
  auto firstBinding = f.bind(first);
  auto secondBinding = f.bind(second);
  auto outsideBinding = f.bind(outside);
  auto groupScope = take(f.router->focusScope(group, second));
  auto nestedScope = take(f.router->focusScope(nested, second));

  check(take(f.router->focused()) == first, "initial global focus does not jump to a scope default");
  check(take(f.router->traverse()) == second, "forward cycle visits nested second control");
  check(take(f.router->traverse()) == outside, "forward cycle leaves nested and outer scopes");
  check(take(f.router->traverse()) == first, "forward wrap visits first instead of remembered boundary");
  check(take(f.router->traverse(true)) == outside, "reverse wrap visits global final control");
  check(take(f.router->traverse(true)) == second, "reverse cycle visits nested second control");
  check(take(f.router->traverse(true)) == first, "reverse cycle visits first control");
  check(f.router->setFocus(second).has_value(), "remember nested target for explicit entry");
  check(f.router->setFocus(outside).has_value(), "leave scope programmatically");
  check(take(f.router->enterScope(group)) == second, "explicit entry still restores exact remembered target");
  check(take(f.router->traverse()) == outside, "Tab leaves explicitly restored boundary target");
  check(take(f.router->traverse()) == first, "next wrap still visits every control");
}

void scopeBoundariesAndErrors() {
  Fixture f({3});
  auto first = f.child(f.root);
  auto group = f.child(f.root);
  auto second = f.child(group);
  auto firstBinding = f.bind(first);
  auto secondBinding = f.bind(second);
  auto scope = take(f.router->focusScope(group, second));

  check(error(f.router->focusScope(f.root), TreeError::NODE_LIMIT), "scope shares action registration budget");
  check(error(f.router->bind(group,
                             [](Widget &, const RoutedEvent &) {
                               return EventReply::IGNORED;
                             }),
              TreeError::NODE_LIMIT),
        "action checks budget consumed by scope");
  check(take(f.router->enterScope(group)) == second, "scope entry selects declared default");
  check(f.tree->setVisible(group, false).has_value(), "hide complete scope");
  check(take(f.router->enterScope(group)) == Node<Widget>{}, "hidden scope has empty eligible selection");
  check(f.tree->setVisible(group, true).has_value(), "show complete scope");
  scope.disconnect();

  auto rootScope = take(f.router->focusScope(f.root));

  check(take(f.router->enterScope(f.root)) == first, "cancelled scope no longer overrides preorder");
  rootScope.disconnect();
  firstBinding.disconnect();
  secondBinding.disconnect();
  int losses{}, gains{};
  auto errorBinding = take(f.router->bind(
    first,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool focused) -> std::expected<void, TreeError> {
      if (!focused) {
        ++losses;
        check(error(f.router->enterScope(group), TreeError::WOULD_BLOCK), "scope entry rejects callback reentry");

        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      return {};
    }));
  auto gainedBinding = take(f.router->bind(
    second,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool focused) -> std::expected<void, TreeError> {
      gains += focused;

      return {};
    }));
  auto groupScope = take(f.router->focusScope(group, second));

  check(f.router->setFocus(first).has_value(), "select previous target for typed loss error");
  check(error(f.router->enterScope(group), TreeError::INVALID_ARGUMENT), "typed blur error is returned");
  check(losses == 1 && gains == 1 && take(f.router->focused()) == second,
        "typed error preserves published target and gain notification");
  check(f.tree->remove(second).has_value(), "remove focused scoped target");
  check(take(f.router->focused()) == first, "scope fallback does not deliver to removed widget");
  f.router.reset();
  groupScope.disconnect();
}

void lateScopeRegistration() {
  Fixture f;
  auto outside = f.child(f.root);
  auto group = f.child(f.root);
  auto child = f.child(group);
  auto fallback = f.child(group);
  auto preferred = f.child(group);
  auto outsideBinding = f.bind(outside);
  auto childBinding = f.bind(child);
  auto fallbackBinding = f.bind(fallback);
  auto preferredBinding = f.bind(preferred);

  check(f.router->setFocus(outside).has_value(), "remember an outside target before late scope registration");
  check(f.router->setFocus(child).has_value(), "focus descendant before declaring its scope");

  auto scope = take(f.router->focusScope(group, preferred));

  check(take(f.router->enterScope(group)) == child, "late scope registration remembers existing focused descendant");
  check(f.tree->setVisible(child, false).has_value(), "invalidate late remembered focus");
  check(take(f.router->focused()) == preferred, "late scope recovery uses default before outside global history");
  scope.disconnect();

  auto rootScope = take(f.router->focusScope(f.root, outside));

  check(f.router->setFocus(fallback).has_value(), "root scope records programmatic descendant");
  check(f.tree->setVisible(group, false).has_value(), "hide whole nested group");
  check(take(f.router->focused()) == outside, "root scope skips ineligible history for configured default");
}

void scopeCallbackRetirement() {
  Fixture f;
  auto outside = f.child(f.root);
  auto group = f.child(f.root);
  auto child = f.child(group);
  int gains{};
  bool removeOnLoss{};
  auto outsideBinding = take(f.router->bind(
    outside,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool focused) -> std::expected<void, TreeError> {
      if (!focused && removeOnLoss) {
        check(error(f.router->focusScope(outside), TreeError::WOULD_BLOCK),
              "scope registration rejects focus-callback reentry");
        check(f.tree->remove(group).has_value(), "loss callback retires destination subtree");
      }

      return {};
    }));
  auto childBinding = take(f.router->bind(
    child,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool focused) -> std::expected<void, TreeError> {
      gains += focused;

      return {};
    }));
  auto scope = take(f.router->focusScope(group, child));

  check(f.router->setFocus(outside).has_value(), "select outside target before callback removal");
  removeOnLoss = true;
  check(take(f.router->enterScope(group)) == Node<Widget>{}, "scope entry returns empty after loss removes target");
  check(gains == 0, "removed scoped destination never receives gain");
  check(take(f.router->focused()) == outside, "next focus operation recovers after callback retirement");
}

void scopeAllocationRollback() {
  int registrationErrors{}, entryErrors{}, registrationSuccess{}, entrySuccess{};

  for (std::ptrdiff_t allowance = 0; allowance < 80; ++allowance) {
    Fixture f;
    auto before = f.child(f.root);
    auto group = f.child(f.root);
    auto child = f.child(group);
    auto beforeBinding = f.bind(before);
    auto childBinding = f.bind(child);

    check(f.router->setFocus(before).has_value(), "set focus before scope allocation sweep");
    allocation_test::remaining = allowance;

    auto registered = f.router->focusScope(group, child);

    allocation_test::remaining = -1;

    if (!registered) {
      ++registrationErrors;
      check(registered.error() == TreeError::NO_MEMORY, "scope registration translates allocation failure");
      check(error(f.router->enterScope(group), TreeError::INVALID_ARGUMENT), "failed scope publishes no record");
      registered = f.router->focusScope(group, child);
    } else {
      ++registrationSuccess;
    }

    take(f.tree->consumeFrameRequest());
    allocation_test::remaining = allowance;

    auto entered = f.router->enterScope(group);

    allocation_test::remaining = -1;

    if (!entered) {
      ++entryErrors;
      check(entered.error() == TreeError::NO_MEMORY, "scope entry translates allocation failure");
      check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "prepublication entry OOM requests no paint");
      check(take(f.router->focused()) == before, "prepublication entry OOM preserves old focus");
    } else {
      ++entrySuccess;
      check(*entered == child, "successful scope entry selects default");
    }
  }

  check(registrationErrors && entryErrors && registrationSuccess && entrySuccess,
        "scope sweeps exercise allocation failures and successful transitions");
}

void mouseGeometry() {
  Fixture f;
  auto under = f.child(f.root);
  auto over = f.child(f.root);
  auto label = f.child(over);
  auto under_binding = f.bind(under);
  int mouse_calls{};
  auto over_binding = take(f.router->bind(over, [&](Widget &, const RoutedEvent &event) {
    if (std::holds_alternative<MouseInputEvent>(event)) {
      ++mouse_calls;
    }

    return EventReply::HANDLED;
  }));

  f.arrange(f.root, 0, 0, 20, 8);
  f.arrange(under, 0, 0, 10, 4);
  f.arrange(over, 2, 1, 4, 2);
  f.arrange(label, 1, 0, 10, 1);
  check(take(f.router->hitTest({3, 1})) == label, "deepest later sibling wins");
  check(take(f.router->hitTest({6, 1})) == under, "ancestor clip and half-open edge exclude label");
  check(take(f.router->hitTest({20, 0})).id().session == 0, "viewport right edge misses");

  auto mouse = f.input("\x1b[<0;4;2M");

  check(mouse.target == label.id() && mouse.handler == over.id() && mouse_calls == 1,
        "real SGR input hits child and bubbles once");
  check(take(f.router->focused()) == over, "left press focuses nearest registered ancestor");

  check(f.tree->setEnabled(over, false).has_value(), "disable overlay");
  check(take(f.router->hitTest({3, 1})) == under, "disabled ancestor removes subtree from hit testing");

  check(f.tree->setEnabled(over, true).has_value(), "reenable overlay");
  check(f.tree->invalidate(over, Invalidation::LAYOUT).has_value(), "invalidate geometry");
  check(f.input("\x1b[<0;4;2M").status == DispatchStatus::NO_TARGET, "stale geometry cannot route mouse");
  check(f.input("\r").handler == over.id(), "stale geometry preserves keyboard focus");
}

struct LifetimeWidget : Widget {
  int &destroyed;
  int value{42};

  explicit LifetimeWidget(int &count) : destroyed(count) {
  }

  ~LifetimeWidget() override {
    ++destroyed;
  }
};

void bubblingAndLifetime() {
  Fixture f;
  int destroyed{};
  auto parent = take(f.tree->emplace<LifetimeWidget>(f.root, destroyed));
  auto child = take(f.tree->emplace<LifetimeWidget>(parent, destroyed));
  std::vector<int> order;
  auto root_binding = take(f.router->bind(
    f.root,
    [&](Widget &, const RoutedEvent &) {
      order.push_back(3);

      return EventReply::HANDLED;
    },
    false));
  auto parent_binding = take(f.router->bind(
    parent,
    [&](LifetimeWidget &, const RoutedEvent &) {
      order.push_back(2);

      return EventReply::IGNORED;
    },
    false));
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> weak_capture = capture;
  ActionBinding child_binding;

  child_binding = take(f.router->bind(child, [&, retained = capture](LifetimeWidget &widget, const RoutedEvent &) {
    order.push_back(1);
    child_binding.disconnect();
    check(f.tree->remove(parent).has_value(), "handler removes own ancestor");
    check(take(f.router->focused()).id().session == 0, "focus recovery cleans removed registrations during callback");
    check(widget.value == 42 && destroyed == 0 && *retained == 7 && !weak_capture.expired(),
          "current widget and captures survive ancestor removal and self-disconnect");

    return EventReply::IGNORED;
  }));

  capture.reset();

  auto result = take(f.router->invoke(child, {Action::ACTIVATE}));

  check(order == std::vector<int>({1, 3}), "removed ancestor skipped but original live root receives bubble");
  check(result.target == child.id() && result.handler == f.root.id(), "removed target still identifiable in result");
  check(destroyed == 2 && weak_capture.expired(), "widget and captures reclaimed after active handler");
  check(error(f.router->invoke(child, {Action::ACTIVATE}), TreeError::STALE_NODE), "removed node never invoked again");

  auto live = f.child(f.root);
  auto throwing = take(f.router->bind(live, [](Widget &, const RoutedEvent &) -> EventReply {
    throw std::runtime_error("handler failure");
  }));

  order.clear();
  check(error(f.router->invoke(live, {Action::ACTIVATE}), TreeError::CALLBACK_FAILED) && order.empty(),
        "handler exception stops bubbling");
  throwing.disconnect();

  auto replacement = f.bind(live);

  check(take(f.router->invoke(live, {Action::ACTIVATE})).handler == f.root.id(),
        "router remains usable after exception");
}

void selfRemovalAndDisconnect() {
  Fixture f;
  auto parent = f.child(f.root);
  auto child = f.child(parent);
  int parent_calls{};
  auto parent_binding = take(f.router->bind(
    parent,
    [&](Widget &, const RoutedEvent &) {
      ++parent_calls;

      return EventReply::HANDLED;
    },
    false));
  auto child_binding = take(f.router->bind(child, [&](Widget &, const RoutedEvent &) {
    check(f.tree->remove(child).has_value(), "handler removes itself");

    return EventReply::IGNORED;
  }));

  check(take(f.router->invoke(child, {Action::ACTIVATE})).handler == parent.id() && parent_calls == 1,
        "self-removal continues to surviving parent");

  auto other = f.child(parent);
  auto other_binding = take(f.router->bind(other, [&](Widget &, const RoutedEvent &) {
    parent_binding.disconnect();

    return EventReply::IGNORED;
  }));

  check(take(f.router->invoke(other, {Action::ACTIVATE})).status == DispatchStatus::UNHANDLED && parent_calls == 1,
        "disconnect excludes future ancestor callback in current bubble");
}

void boundariesAndCapacity() {
  Fixture f({2});
  Fixture foreign;
  auto first = f.child(f.root);
  auto second = f.child(f.root);
  auto third = f.child(f.root);
  int calls{};
  auto handler = [&](Widget &, const RoutedEvent &) {
    ++calls;

    return EventReply::HANDLED;
  };
  auto first_binding = take(f.router->bind(first, handler));
  auto second_binding = f.bind(second);

  check(error(f.router->bind(first, handler), TreeError::INVALID_ARGUMENT), "duplicate registration rejected");
  check(error(f.router->bind(third, handler), TreeError::NODE_LIMIT), "registration bound enforced");
  check(error(f.router->invoke(foreign.root, {Action::ACTIVATE}), TreeError::WRONG_SESSION), "foreign invoke rejected");
  check(error(f.router->bind(foreign.root, handler), TreeError::WRONG_SESSION), "foreign bind rejected");

  check(f.tree->setEnabled(first, false).has_value(), "disable invocation target");
  check(take(f.router->invoke(first, {Action::ACTIVATE})).status == DispatchStatus::NO_TARGET && calls == 0,
        "disabled programmatic target is explicit and not called");

  check(f.tree->setEnabled(first, true).has_value(), "reenable invocation target");

  std::thread worker([&] {
    check(error(f.router->invoke(first, {Action::ACTIVATE}), TreeError::WRONG_THREAD),
          "foreign-thread invoke rejected");
    check(error(f.router->dispatch(KeyEvent{InputKey::ENTER}), TreeError::WRONG_THREAD),
          "foreign-thread input rejected");
    check(error(f.router->bind(third, handler), TreeError::WRONG_THREAD), "foreign-thread bind rejected");
    check(error(f.router->focused(), TreeError::WRONG_THREAD), "foreign-thread focus rejected");
    second_binding.disconnect();
  });

  worker.join();
  check(calls == 0, "rejected operations never call user handler");

  auto third_binding = take(f.router->bind(third, handler));

  check(f.tree->remove(third).has_value(), "remove registered node");

  auto fourth = f.child(f.root);
  auto fourth_binding = take(f.router->bind(fourth, handler));

  check(fourth.id() != third.id(), "slot reuse keeps generation identity");
  check(error(f.router->invoke(third, {Action::ACTIVATE}), TreeError::STALE_NODE),
        "slot reuse cannot reroute stale ID");
  check(f.tree
          ->frame([&] {
            check(error(f.router->setFocus(first), TreeError::WOULD_BLOCK), "frame cannot mutate focus");
            check(error(f.router->invoke(first, {Action::ACTIVATE}), TreeError::WOULD_BLOCK),
                  "frame cannot invoke actions");
          })
          .has_value(),
        "frame fixture completes");
  check(take(f.router->focused()) == first && calls == 0, "rejected frame call leaves focus and handler untouched");
  first_binding.disconnect();

  auto nested_binding = take(f.router->bind(first, [&](Widget &, const RoutedEvent &) {
    ++calls;
    check(error(f.router->invoke(first, {Action::ACTIVATE}), TreeError::WOULD_BLOCK), "nested invoke is finite");
    check(error(f.router->dispatch(KeyEvent{InputKey::ENTER}), TreeError::WOULD_BLOCK), "nested dispatch is finite");
    check(error(f.router->bind(second, handler), TreeError::WOULD_BLOCK), "nested registration rejected explicitly");

    return EventReply::HANDLED;
  }));

  check(take(f.router->invoke(first, {Action::ACTIVATE})).status == DispatchStatus::HANDLED && calls == 1,
        "outer delivery continues after rejected reentrancy");
  f.router.reset();
  nested_binding.disconnect();
  fourth_binding.disconnect();
  check(calls == 1, "tokens safely outlive router without calls");
}

struct OnRelease {
  std::function<void()> function;

  explicit OnRelease(std::function<void()> callback) : function(std::move(callback)) {
  }

  ~OnRelease() {
    function();
  }
};

void captureReclamation() {
  Fixture f;
  auto child = f.child(f.root);
  auto *router = f.router.get();
  const auto owner = std::this_thread::get_id();
  int released{};
  auto capture = std::make_shared<OnRelease>([&] {
    check(std::this_thread::get_id() == owner, "worker cancellation reclaims captures on UI");
    check(error(router->focused(), TreeError::WOULD_BLOCK), "capture destructor cannot reenter registry collection");
    check(error(router->bind(child,
                             [](Widget &, const RoutedEvent &) {
                               return EventReply::IGNORED;
                             }),
                TreeError::WOULD_BLOCK),
          "capture destructor cannot mutate registry during collection");
    ++released;
  });
  ActionBinding binding;

  binding = take(router->bind(child, [&, retained = capture](Widget &, const RoutedEvent &) {
    std::thread worker([&] {
      binding.disconnect();
    });

    worker.join();
    check(released == 0 && retained != nullptr, "already admitted callback survives concurrent disconnect");

    return EventReply::HANDLED;
  }));

  capture.reset();
  check(take(router->invoke(child, {Action::ACTIVATE})).status == DispatchStatus::HANDLED,
        "admitted callback finishes");
  check(take(router->focused()).id().session == 0 && released == 1, "next UI operation reclaims disconnected captures");

  auto closing_capture = std::make_shared<OnRelease>([&] {
    check(std::this_thread::get_id() == owner, "router shutdown reclaims on UI");
    check(error(router->focused(), TreeError::CLOSED), "capture destructor sees closed router during shutdown");
    ++released;
  });
  auto closing_binding = take(router->bind(child, [retained = closing_capture](Widget &, const RoutedEvent &) {
    return retained ? EventReply::HANDLED : EventReply::IGNORED;
  }));

  closing_capture.reset();
  f.router.reset();
  closing_binding.disconnect();
  check(released == 2, "shutdown releases all callback captures exactly once");
}

void allocationFailures() {
  Fixture f;
  auto child = f.child(f.root);
  auto binding = f.bind(child);

  f.arrange(f.root, 0, 0, 20, 8);
  f.arrange(child, 0, 0, 4, 1);

  int create_errors{}, focus_errors{}, route_errors{}, hit_errors{}, bind_errors{};

  for (std::ptrdiff_t allowance = 0; allowance < 40; ++allowance) {
    allocation_test::remaining = allowance;

    auto created = EventRouter::create(*f.tree);

    allocation_test::remaining = -1;

    if (!created) {
      ++create_errors;
      check(created.error() == TreeError::NO_MEMORY, "create translates allocation failure");
    }

    allocation_test::remaining = allowance;

    auto focused = f.router->focused();

    allocation_test::remaining = -1;

    if (!focused) {
      ++focus_errors;
      check(focused.error() == TreeError::NO_MEMORY, "focus translates allocation failure");
    }

    allocation_test::remaining = allowance;

    auto routed = f.router->invoke(child, {Action::ACTIVATE});

    allocation_test::remaining = -1;

    if (!routed) {
      ++route_errors;
      check(routed.error() == TreeError::NO_MEMORY, "routing translates allocation failure");
    }

    allocation_test::remaining = allowance;

    auto hit = f.router->hitTest({1, 0});

    allocation_test::remaining = -1;

    if (!hit) {
      ++hit_errors;
      check(hit.error() == TreeError::NO_MEMORY, "hit test translates allocation failure");
    }

    auto other = f.child(f.root);

    allocation_test::remaining = allowance;

    auto registered = f.router->bind(other, [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    });

    allocation_test::remaining = -1;

    if (!registered) {
      ++bind_errors;
      check(registered.error() == TreeError::NO_MEMORY, "binding translates allocation failure");
    }

    check(f.tree->remove(other).has_value(), "allocation sweep removes own node");
  }

  check(create_errors > 0 && focus_errors > 0 && route_errors > 0 && hit_errors > 0 && bind_errors > 0,
        "failure injection reaches every allocating router operation");
  check(take(f.router->invoke(child, {Action::ACTIVATE})).status == DispatchStatus::UNHANDLED,
        "router remains usable after allocation failures");
}

struct CopyReentry {
  bool &entered;
  std::function<void()> operation;

  CopyReentry(bool &flag, std::function<void()> callback) : entered(flag), operation(std::move(callback)) {
  }

  CopyReentry(const CopyReentry &other) : entered(other.entered), operation(other.operation) {
    if (!entered) {
      entered = true;
      operation();
    }
  }

  EventReply operator()(Widget &, const RoutedEvent &) const {
    return EventReply::IGNORED;
  }
};

void registrationMaterialization() {
  Fixture f({1});
  auto first = f.child(f.root);
  auto second = f.child(f.root);
  ActionBinding nested;
  bool entered{};
  CopyReentry duplicate(entered, [&] {
    nested = f.bind(first);
  });

  check(error(f.router->bind(first, duplicate), TreeError::INVALID_ARGUMENT),
        "callback copy cannot bypass duplicate registration validation");
  nested.disconnect();
  entered = false;

  CopyReentry capacity(entered, [&] {
    nested = f.bind(second);
  });

  check(error(f.router->bind(first, capacity), TreeError::NODE_LIMIT),
        "callback copy cannot exceed registration capacity");
  nested.disconnect();
  entered = false;

  CopyReentry removal(entered, [&] {
    check(f.tree->remove(first).has_value(), "copy removes its prospective target");
  });

  check(error(f.router->bind(first, removal), TreeError::STALE_NODE), "callback copy cannot register a removed target");

  auto victim = f.child(f.root);
  auto garbage = std::make_shared<OnRelease>([&] {
    check(f.tree->remove(victim).has_value(), "capture destructor removes prospective target");
  });

  nested = take(f.router->bind(second, [retained = garbage](Widget &, const RoutedEvent &) {
    return retained ? EventReply::HANDLED : EventReply::IGNORED;
  }));

  garbage.reset();
  nested.disconnect();
  check(error(f.router->bind(victim,
                             [](Widget &, const RoutedEvent &) {
                               return EventReply::IGNORED;
                             }),
              TreeError::STALE_NODE),
        "collection side effects cannot register a removed target");
}

void focusReturnStack() {
  Fixture f;
  auto outside = f.child(f.root);
  auto outer = f.child(f.root);
  auto first = f.child(outer);
  auto inner = f.child(outer);
  auto second = f.child(inner);
  auto outsideBinding = f.bind(outside);
  auto firstBinding = f.bind(first);
  auto secondBinding = f.bind(second);
  auto outerScope = take(f.router->focusScope(outer, first));
  auto innerScope = take(f.router->focusScope(inner, second));

  take(f.router->setFocus(outside));
  check(take(f.router->enterFocusScope(outer)) == first, "return entry chooses default");
  check(error(f.router->enterFocusScope(outer), TreeError::INVALID_ARGUMENT), "duplicate activation rejects root");
  check(take(f.router->enterFocusScope(inner)) == second, "nested return entry chooses inner default");
  check(error(f.router->leaveFocusScope(outer), TreeError::INVALID_ARGUMENT), "leave rejects non-top root");
  check(take(f.router->leaveFocusScope(inner)) == first, "inner leaves to exact previous focus");
  check(take(f.router->leaveFocusScope(outer)) == outside, "outer returns to original focus");
  check(error(f.router->leaveFocusScope(outer), TreeError::INVALID_ARGUMENT), "published leave consumes record");

  take(f.router->enterFocusScope(outer));
  take(f.router->setFocus(outside));
  check(take(f.router->focused()) == outside, "activation permits explicit focus outside");
  check(take(f.router->traverse()) == first, "activation preserves global Tab traversal");
  check(take(f.router->leaveFocusScope(outer)) == outside, "outside focus does not overwrite saved return");

  take(f.router->enterFocusScope(outer));
  take(f.router->enterFocusScope(inner));
  take(f.tree->setVisible(outer, false));
  check(take(f.router->focused()) == outside, "hidden nested roots unwind together to original focus");
  take(f.tree->setVisible(outer, true));
  check(error(f.router->leaveFocusScope(inner), TreeError::INVALID_ARGUMENT), "showing does not revive ended return");
  take(f.router->enterFocusScope(outer));
  take(f.tree->setEnabled(outer, false));
  check(f.input("\r").target == outside.id(), "input dispatch recovers a disabled activation before routing");
  take(f.tree->setEnabled(outer, true));

  take(f.router->enterFocusScope(outer));
  take(f.router->enterFocusScope(inner));
  std::thread cancel([&] {
    outerScope.disconnect();
  });
  cancel.join();
  check(take(f.router->focused()) == second, "cancelled outer defers behind active inner");
  check(take(f.router->leaveFocusScope(inner)) == outside, "inner leave unwinds cancelled outer LIFO");
  outerScope = take(f.router->focusScope(outer, first));
  take(f.router->enterFocusScope(outer));
  outerScope.disconnect();
  outerScope = take(f.router->focusScope(outer, first));
  check(take(f.router->focused()) == outside, "same-generation re-registration cannot revive cancelled activation");

  take(f.router->enterFocusScope(outer));
  const auto old = outer;
  take(f.tree->remove(outer));
  outer = f.child(f.root);
  first = f.child(outer);
  auto replacementBinding = f.bind(first);
  outerScope = take(f.router->focusScope(outer, first));
  check(old != outer && take(f.router->focused()) == outside,
        "root removal and slot reuse preserve exact return identity");
  check(error(f.router->enterFocusScope(old), TreeError::STALE_NODE), "old root cannot enter replacement");

  auto empty = f.child(f.root);
  auto emptyScope = take(f.router->focusScope(empty));
  check(take(f.router->enterFocusScope(empty)) == Node<Widget>{}, "empty scope retains a return record");
  check(take(f.router->leaveFocusScope(empty)) == outside, "empty scope can return to saved focus");
  take(f.tree->setVisible(empty, false));
  check(error(f.router->enterFocusScope(empty), TreeError::INVALID_ARGUMENT), "ineligible activation is rejected");
  Fixture foreign;
  check(error(f.router->enterFocusScope(foreign.root), TreeError::WRONG_SESSION),
        "foreign activation preserves typed error");
  std::thread wrongThread([&] {
    check(error(f.router->enterFocusScope(outer), TreeError::WRONG_THREAD), "entry is UI-affine");
    check(error(f.router->leaveFocusScope(outer), TreeError::WRONG_THREAD), "return is UI-affine");
  });
  wrongThread.join();
}

void focusReturnFallback() {
  Fixture f;
  auto origin = f.child(f.root);
  auto prior = f.child(origin);
  auto fallback = f.child(origin);
  auto destination = f.child(f.root);
  auto target = f.child(destination);
  auto priorBinding = f.bind(prior);
  auto fallbackBinding = f.bind(fallback);
  auto targetBinding = f.bind(target);
  auto originScope = take(f.router->focusScope(origin, fallback));
  auto destinationScope = take(f.router->focusScope(destination, target));
  take(f.router->setFocus(prior));
  take(f.router->enterFocusScope(destination));
  const auto oldPrior = prior;
  take(f.tree->remove(prior));
  prior = f.child(origin);
  auto replacementBinding = f.bind(prior);
  check(prior != oldPrior, "return fixture reuses a distinct generation");
  check(take(f.router->leaveFocusScope(destination)) == fallback,
        "stale previous uses saved scope default, not reused slot");
  take(f.router->enterFocusScope(destination));
  take(f.tree->setVisible(origin, false));
  check(take(f.router->leaveFocusScope(destination)) == target,
        "ineligible saved ancestry uses eligible global fallback");
}

void focusReturnCallbacks() {
  Fixture f;
  auto before = f.child(f.root);
  auto group = f.child(f.root);
  auto target = f.child(group);
  bool failLoss{}, removeTarget{}, failReturn{}, reenter{};
  auto handler = [](Widget &, const RoutedEvent &) {
    return EventReply::IGNORED;
  };
  auto beforeBinding =
    take(f.router->bind(before, handler, [&](Widget &, bool gained) -> std::expected<void, TreeError> {
      if (!gained && reenter) {
        check(error(f.router->enterFocusScope(group), TreeError::WOULD_BLOCK),
              "focus callback cannot enter recursively");
        check(error(f.router->leaveFocusScope(group), TreeError::WOULD_BLOCK),
              "focus callback cannot leave recursively");
      }
      if (!gained && removeTarget) {
        take(f.tree->remove(group));
      }
      return !gained && failLoss ? std::unexpected(TreeError::NODE_LIMIT) : std::expected<void, TreeError>{};
    }));
  auto targetBinding =
    take(f.router->bind(target, handler, [&](Widget &, bool gained) -> std::expected<void, TreeError> {
      return !gained && failReturn ? std::unexpected(TreeError::INVALID_ARGUMENT) : std::expected<void, TreeError>{};
    }));
  auto scope = take(f.router->focusScope(group, target));
  take(f.router->setFocus(before));
  failLoss = reenter = true;
  check(error(f.router->enterFocusScope(group), TreeError::NODE_LIMIT), "typed entry blur error propagates");
  check(take(f.router->focused()) == target, "entry callback error preserves published focus");
  failLoss = reenter = false;
  failReturn = true;
  check(error(f.router->leaveFocusScope(group), TreeError::INVALID_ARGUMENT), "typed return blur error propagates");
  check(take(f.router->focused()) == before, "return callback error preserves published focus");
  check(error(f.router->leaveFocusScope(group), TreeError::INVALID_ARGUMENT),
        "failed published return consumes record");
  failReturn = false;
  removeTarget = true;
  check(take(f.router->enterFocusScope(group)) == Node<Widget>{}, "entry callback removal returns empty observer");
  check(take(f.router->focused()) == before, "next operation recovers removed activation");
}

void focusReturnRecoveryCycles() {
  Fixture f;
  auto prior = f.child(f.root);
  auto fallback = f.child(f.root);
  auto group = f.child(f.root);
  auto target = f.child(group);
  bool removeOnGain{};
  int gains{};
  auto priorBinding = take(f.router->bind(
    prior,
    [](Widget &, const RoutedEvent &) {
      return EventReply::IGNORED;
    },
    [&](Widget &, bool gained) -> std::expected<void, TreeError> {
      if (gained) {
        ++gains;
        if (removeOnGain) {
          take(f.tree->remove(prior));
        }
      }
      return {};
    }));
  auto fallbackBinding = f.bind(fallback);
  auto targetBinding = f.bind(target);
  auto scope = take(f.router->focusScope(group, target));
  take(f.router->setFocus(prior));
  take(f.router->enterFocusScope(group));
  take(f.tree->setVisible(group, false));
  removeOnGain = true;
  auto returned = take(f.router->dispatch(KeyEvent{InputKey::TAB}));
  check(returned.status == DispatchStatus::NO_TARGET && gains == 2,
        "automatic Tab return stops after callback self-removal without a second focus cycle");
  check(take(f.router->focused()) == fallback && gains == 2, "later query recovers after return self-removal");
  take(f.tree->setVisible(group, true));
  take(f.router->enterFocusScope(group));
  take(f.tree->setEnabled(group, false));
  check(take(f.router->traverse()) == fallback, "ended scope consumes one Tab step for return");
  take(f.tree->setEnabled(group, true));
  check(take(f.router->traverse()) == target, "following Tab resumes full global traversal");

  take(f.router->setFocus(fallback));
  take(f.router->enterFocusScope(group));
  take(f.tree->setVisible(group, false));
  allocation_test::remaining = 0;
  auto failed = f.router->focused();
  allocation_test::remaining = -1;
  check(error(failed, TreeError::NO_MEMORY), "lazy recovery allocation failure is typed");
  check(take(f.router->focused()) == fallback, "lazy return allocation failure retains retryable record");
  check(error(f.router->leaveFocusScope(group), TreeError::INVALID_ARGUMENT), "successful lazy retry consumes record");
}

void focusReturnDepthBudget() {
  Fixture f({1});
  auto first = f.child(f.root);
  auto second = f.child(f.root);
  auto scope = take(f.router->focusScope(first));
  take(f.router->enterFocusScope(first));
  scope.disconnect();
  scope = take(f.router->focusScope(second));
  check(error(f.router->enterFocusScope(second), TreeError::NODE_LIMIT),
        "cancelled deferred records cannot grow return stack beyond configured budget");
  take(f.router->focused());
  take(f.router->enterFocusScope(second));
  take(f.router->leaveFocusScope(second));
}

void focusReturnAllocations() {
  int enterErrors{}, leaveErrors{}, enterSuccess{}, leaveSuccess{};
  for (std::ptrdiff_t allowance = 0; allowance < 80; ++allowance) {
    Fixture f;
    auto before = f.child(f.root);
    auto group = f.child(f.root);
    auto target = f.child(group);
    auto beforeBinding = f.bind(before);
    auto targetBinding = f.bind(target);
    auto scope = take(f.router->focusScope(group, target));
    take(f.router->setFocus(before));
    take(f.tree->consumeFrameRequest());
    allocation_test::remaining = allowance;
    auto entered = f.router->enterFocusScope(group);
    allocation_test::remaining = -1;
    if (!entered) {
      ++enterErrors;
      check(entered.error() == TreeError::NO_MEMORY, "entry allocation failure is typed");
      check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "entry OOM requests no paint");
      check(take(f.router->focused()) == before, "entry OOM preserves old focus");
      check(error(f.router->leaveFocusScope(group), TreeError::INVALID_ARGUMENT),
            "entry OOM publishes no return record");
      take(f.router->enterFocusScope(group));
    } else {
      ++enterSuccess;
    }
    take(f.tree->consumeFrameRequest());
    allocation_test::remaining = allowance;
    auto left = f.router->leaveFocusScope(group);
    allocation_test::remaining = -1;
    if (!left) {
      ++leaveErrors;
      check(left.error() == TreeError::NO_MEMORY, "return allocation failure is typed");
      check(take(f.tree->consumeFrameRequest()) == Invalidation::NONE, "return OOM requests no paint");
      check(take(f.router->focused()) == target, "return OOM preserves focus and retryable stack");
      check(take(f.router->leaveFocusScope(group)) == before, "return OOM retry restores exact target");
    } else {
      ++leaveSuccess;
      check(*left == before, "successful return restores exact target");
    }
  }
  check(enterErrors && leaveErrors && enterSuccess && leaveSuccess, "return sweeps cover failures and successes");
}

} // namespace

int main() {
  focusReturnStack();
  focusReturnFallback();
  focusReturnCallbacks();
  focusReturnAllocations();
  focusReturnRecoveryCycles();
  focusReturnDepthBudget();
  activationAndInput();
  traversalAndReturn();
  traversalIdentityAndOrder();
  traversalAllocationRollback();
  nestedScopes();
  scopeTraversalCycles();
  scopeBoundariesAndErrors();
  lateScopeRegistration();
  scopeCallbackRetirement();
  scopeAllocationRollback();
  mouseGeometry();
  bubblingAndLifetime();
  selfRemovalAndDisconnect();
  boundariesAndCapacity();
  captureReclamation();
  allocationFailures();
  registrationMaterialization();

  return failures == 0 ? 0 : 1;
}
