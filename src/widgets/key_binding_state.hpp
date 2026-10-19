// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "action_state.hpp"
#include <portals/managed.hpp>

namespace portals {
struct ManagedScene::KeyRecord {
  struct Metadata {
    static constexpr unsigned char SCOPE_CONNECTED = 1u;
    static constexpr unsigned char OWNER_RETIRED = 2u;

    Metadata(GraphemeText caption, GraphemeText explanation)
        : label(std::move(caption)), description(std::move(explanation)) {
    }

    GraphemeText label;
    GraphemeText description;
    KeyBindingId id;
    Node<Widget> owner;
    Node<Widget> scope;
    std::weak_ptr<detail::ActionCancellation> cancellation;
    std::optional<std::weak_ptr<detail::ActionCancellation>> ownerLifetime;
    bool enabled{true};
    bool visible{true};

    bool ownerConnected() const noexcept {
      if (!ownerLifetime) {
        return true;
      }
      const auto state = ownerLifetime->lock();
      return state && state->connected.load();
    }

    unsigned char observation() const noexcept {
      const auto scopeState = cancellation.lock();
      return static_cast<unsigned char>((scopeState && scopeState->connected.load() ? SCOPE_CONNECTED : 0u) |
                                        (ownerConnected() ? 0u : OWNER_RETIRED));
    }
  };

  std::optional<KeyEvent> legacy;
  std::optional<Shortcut> shortcut;
  detail::ShortcutActions actions{detail::ShortcutActions::PRESS};
  // The legacy field remains direct. Only annotated registrations share an action owner.
  std::function<EditorCommandResult(const KeyEvent &)> action;
  std::shared_ptr<const std::function<EditorCommandResult(const KeyEvent &)>> annotatedAction;
  std::shared_ptr<const Metadata> metadata;
  std::optional<std::source_location> origin;
};

struct ManagedScene::KeyPending {
  struct Candidate {
    std::size_t record{};
    std::size_t alternative{};
  };

  std::vector<Candidate> candidates = std::vector<Candidate>(0);
  std::vector<Candidate> scratch = std::vector<Candidate>(0);
  Subscription timer;
  std::size_t depth{};
  std::uint64_t generation{1};
  std::uint64_t focusGeneration{};
  std::uint64_t inputGeneration{};
  SchedulerTime deadline{};
};

struct ManagedScene::KeySnapshot {
  std::vector<std::shared_ptr<KeyRecord>> records = std::vector<std::shared_ptr<KeyRecord>>(0);
  std::shared_ptr<std::function<std::expected<bool, ManagedError>(const InputEvent &)>> fallback;
  std::shared_ptr<KeyPending> pending;
  // Private cancellation observations; immutable public projections contain only historical data.
  std::optional<std::vector<unsigned char>> scopeConnected;
  bool annotated{};
};
} // namespace portals
