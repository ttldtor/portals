// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/text_document.hpp>
#include <portals/ui_model.hpp>

namespace portals::ui {
/** @brief Default finite UI limits match a scalar editor: 4096 UTF-8 bytes and graphemes.
 * Explicit options can select another finite bound; no windowed editing is implied.
 */
inline constexpr FiniteTextDocumentOptions DEFAULT_FINITE_TEXT_DOCUMENT_OPTIONS{.limits = {4096, 4096, 4096, 8192}};

/** @brief Owning finite-document presentation; never a window into an arbitrary file. */
struct TextDocumentValue {
  std::string text = std::string(0, '\0');   ///< Current complete finite UTF-8 text.
  std::string target = std::string(0, '\0'); ///< Last acknowledged destination metadata; no I/O.
  TextSelection selection;                   ///< Current byte-boundary selection.
  std::uint64_t refreshStamp{};              ///< UI retry epoch; not a document revision.
  std::uint64_t revision{};                  ///< Current text revision, independent of save requests.
  bool dirty{};                              ///< Current content differs from its savepoint.
  bool conflicted{};                         ///< External replacement awaits explicit resolution.
  bool canUndo{};                            ///< Existing document history has an Undo operation.
  bool canRedo{};                            ///< Existing document history has a Redo operation.
  /** @param other Owning presentation to compare. @return Equality of all fields. */
  bool operator==(const TextDocumentValue &other) const = default;
};

namespace detail {
/// @cond INTERNAL
inline TreeError textDocumentCause(TextDocumentError error) noexcept {
  switch (error) {
  case TextDocumentError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case TextDocumentError::EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case TextDocumentError::STALE:
    return TreeError::STALE_NODE;
  case TextDocumentError::EMPTY:
  case TextDocumentError::WOULD_BLOCK:
  case TextDocumentError::CONFLICT:
    return TreeError::WOULD_BLOCK;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

struct UiTextDocumentState {
  std::unique_ptr<portals::FiniteTextDocument> document;
  ManagedState<TextDocumentValue> presentation;
  std::thread::id owner{std::this_thread::get_id()};
  NodeId scene;
  ManagedScene *sceneOwner{};
  std::weak_ptr<const void> sceneLifetime;
  std::uint64_t refreshStamp{};
  TreeError failure{TreeError::INVALID_ARGUMENT};
  std::optional<TextDocumentError> lastError;

  std::unexpected<ManagedError> error(TreeError cause) const {
    return std::unexpected(ManagedError{ManagedPhase::TEXT, scene, cause, {}, {}});
  }

  std::expected<void, ManagedError> check() const {
    if (owner != std::this_thread::get_id()) {
      return error(TreeError::WRONG_THREAD);
    }
    if (!document) {
      return error(failure);
    }
    if (scene != NodeId{}) {
      if (sceneLifetime.expired() || Access::modelSceneClosed(*sceneOwner)) {
        return error(TreeError::CLOSED);
      }
      auto current = presentation.snapshot();
      if (!current) {
        return error(current.error());
      }
    }

    return {};
  }

  std::shared_ptr<const TextDocumentValue> snapshot() const {
    auto value = std::make_shared<TextDocumentValue>();
    value->text.assign(document->text().bytes());
    value->target.assign(document->saveTarget());
    value->selection = document->selection();
    value->revision = document->revision();
    value->refreshStamp = refreshStamp;
    value->dirty = document->dirty();
    value->conflicted = document->conflicted();
    value->canUndo = document->canUndo();
    value->canRedo = document->canRedo();

    return value;
  }
};

/// @endcond
} // namespace detail

class FiniteTextDocument;
FiniteTextDocument finiteTextDocument(std::string_view text, FiniteTextDocumentOptions options);

/** @brief Copyable UI-affine finite document using the existing read-only scalar binding runtime.
 * Copies share the document/session, not a view. Sources bind to one scene, like ui::Model;
 * replacing a view in that scene preserves text, bookmarks, selection, savepoint and history.
 * The core edit commits before presentation publication. A publication failure may leave the
 * accepted document newer than its last visible value; refresh() retries publication.
 * Methods return checked ManagedError for runApp. lastError() preserves a core typed cause.
 */
class FiniteTextDocument {
public:
  /** @param other Shared finite session/source to retain. */
  FiniteTextDocument(const FiniteTextDocument &other) = default;
  /** @param other Shared finite session/source to retain. @return This facade after assignment. */
  FiniteTextDocument &operator=(const FiniteTextDocument &other) = default;

  /** @return Cold read-only model for ordinary label projections; writes are rejected. */
  Model<TextDocumentValue> source() const {
    return source_;
  }

  /** @param offset Current UTF-8 byte boundary. @param affinity Insertion preference.
   * @return Metadata-only bookmark or checked core error; no UI/view is retained.
   */
  std::expected<ModelBookmark, SourceError> bookmark(std::size_t offset,
                                                     PositionAffinity affinity = PositionAffinity::FORWARD) const {
    if (!state_ || state_->owner != std::this_thread::get_id() || !state_->document) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    return state_->document->bookmark(offset, affinity);
  }

  /** @param begin Inclusive byte boundary. @param end Exclusive byte boundary.
   * @param text Borrowed replacement copied by the finite core.
   * @return Checked edit/presentation result; no file I/O.
   */
  std::expected<void, ManagedError> replace(std::size_t begin, std::size_t end, std::string_view text) const {
    return apply([&](auto &document) {
      return document.replace(begin, end, text);
    });
  }

  /** @param offset Insertion byte boundary. @param text Borrowed UTF-8 to copy.
   * @return Checked edit/presentation result.
   */
  std::expected<void, ManagedError> insert(std::size_t offset, std::string_view text) const {
    return replace(offset, offset, text);
  }

  /** @brief Undo through the existing document history; empty history is a UI no-op.
   * @return Checked edit/presentation result.
   */
  std::expected<void, ManagedError> undo() const {
    return apply(
      [](auto &document) {
        return document.undo();
      },
      true);
  }

  /** @brief Redo through existing history; empty history is a UI no-op.
   * @return Checked edit/presentation result.
   */
  std::expected<void, ManagedError> redo() const {
    return apply(
      [](auto &document) {
        return document.redo();
      },
      true);
  }

  /** @param bookmark Scalar read position from this logical document/lineage.
   * @return Checked caret restoration; no write authority is inferred from the bookmark.
   */
  std::expected<void, ManagedError> restore(const ModelBookmark &bookmark) const {
    return apply([&](auto &document) -> std::expected<void, TextDocumentError> {
      PositionRequest request{document.session(), document.revision(), 1, bookmark};
      auto resolved = document.resolve(request);
      if (!resolved ||
          (resolved->state != PositionResolution::EXACT && resolved->state != PositionResolution::NEAREST)) {
        return std::unexpected(TextDocumentError::STALE);
      }
      const auto offset = static_cast<std::size_t>(resolved->position.offset);

      return document.setSelection({offset, offset});
    });
  }

  /** @param bookmark Checked bookmark result, forwarded without caller-side unwrapping.
   * @return Checked restoration or source-admission error.
   */
  std::expected<void, ManagedError> restore(const std::expected<ModelBookmark, SourceError> &bookmark) const {
    if (!bookmark) {
      return failure(bookmark.error() == SourceError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT);
    }

    return restore(*bookmark);
  }

  /** @param target Destination metadata, not a path to be opened here.
   * @return Save ticket for the current bytes; caller acknowledges only after its save succeeds.
   */
  std::expected<TextSaveTicket, ManagedError> beginSave(std::string_view target) const {
    auto state = state_;
    auto valid = state ? state->check() : std::expected<void, ManagedError>(failure(TreeError::NO_MEMORY));
    if (!valid) {
      return std::unexpected(valid.error());
    }
    auto ticket = state->document->beginSave(target);
    if (!ticket) {
      state->lastError = ticket.error();
      return state->error(detail::textDocumentCause(ticket.error()));
    }

    return *ticket;
  }

  /** @param ticket Successful save receipt; stale acknowledgements remain checked failures.
   * @return Checked savepoint/presentation result, without changing caret or history.
   */
  std::expected<void, ManagedError> acknowledgeSave(const TextSaveTicket &ticket) const {
    return apply([&](auto &document) {
      return document.acknowledgeSave(ticket);
    });
  }

  /** @param ticket Checked request result, propagated without caller-side unwrapping.
   * @return Checked acknowledgement or the original request failure.
   */
  std::expected<void, ManagedError> acknowledgeSave(const std::expected<TextSaveTicket, ManagedError> &ticket) const {
    return ticket ? acknowledgeSave(*ticket) : std::expected<void, ManagedError>(std::unexpected(ticket.error()));
  }

  /** @brief Mark the current in-memory content saved; performs no physical write.
   * @param target Explicit in-memory destination metadata.
   * @return Checked synchronous savepoint result, suitable for memory-only demonstrations.
   */
  std::expected<void, ManagedError> savepoint(std::string_view target) const {
    return acknowledgeSave(beginSave(target));
  }

  /** @return Most recent core failure, if any; UI publication failures are ManagedError only. */
  std::optional<TextDocumentError> lastError() const {
    return state_ && state_->owner == std::this_thread::get_id() ? state_->lastError : std::nullopt;
  }

  /** @brief Retry presentation after a prior notification failure; does not edit content.
   * @return Checked existing-property publication result.
   */
  std::expected<void, ManagedError> refresh() const {
    return apply(
      [](auto &) -> std::expected<void, TextDocumentError> {
        return {};
      },
      false, true);
  }

private:
  friend FiniteTextDocument finiteTextDocument(std::string_view, FiniteTextDocumentOptions);

  FiniteTextDocument(std::shared_ptr<detail::UiTextDocumentState> state, Model<TextDocumentValue> source)
      : state_(std::move(state)), source_(std::move(source)) {
  }

  std::unexpected<ManagedError> failure(TreeError cause) const {
    return std::unexpected(ManagedError{ManagedPhase::TEXT, {}, cause, {}, {}});
  }

  template <class Operation>
  std::expected<void, ManagedError> apply(Operation operation, bool emptyIsNoop = false,
                                          bool forcePublication = false) const {
    auto state = state_; // Pin the core and callable's owners across presentation callbacks.
    if (!state) {
      return failure(TreeError::NO_MEMORY);
    }
    auto valid = state->check();
    if (!valid) {
      return valid;
    }
    if (forcePublication && state->refreshStamp == std::numeric_limits<std::uint64_t>::max()) {
      return state->error(TreeError::IDENTITY_EXHAUSTED);
    }
    try {
      auto changed = operation(*state->document);
      if (!changed && !(emptyIsNoop && changed.error() == TextDocumentError::EMPTY)) {
        state->lastError = changed.error();
        return state->error(detail::textDocumentCause(changed.error()));
      }
      state->lastError.reset();
      if (forcePublication) {
        ++state->refreshStamp;
      }
      if (state->scene != NodeId{}) {
        auto value = state->snapshot();
        auto published = state->presentation.set(*value);
        if (!published) {
          return state->error(published.error());
        }
        if (published->notification_error) {
          ManagedError failure{ManagedPhase::TEXT, state->scene, TreeError::WOULD_BLOCK, {}, {}};
          failure.application = ApplicationError{ApplicationPhase::DISPATCH, *published->notification_error, {}, {}};

          return std::unexpected(failure);
        }
      }

      return {};
    } catch (const std::bad_alloc &) {
      return state->error(TreeError::NO_MEMORY);
    } catch (...) {
      return state->error(TreeError::CALLBACK_FAILED);
    }
  }

  std::shared_ptr<detail::UiTextDocumentState> state_;
  Model<TextDocumentValue> source_;
};

/** @brief Declare one finite in-memory document with a cold read-only presentation source.
 * @param text Initial UTF-8 copied before return. @param options Explicit finite bounds/history policy.
 * @return Copyable UI session; admission failures are reported when its source mounts.
 * Includes no terminal, file writer, replay or continuous polling.
 */
inline FiniteTextDocument finiteTextDocument(std::string_view text,
                                             FiniteTextDocumentOptions options = DEFAULT_FINITE_TEXT_DOCUMENT_OPTIONS) {
  std::shared_ptr<detail::UiTextDocumentState> state;
  try {
    state = std::make_shared<detail::UiTextDocumentState>();
    auto created = portals::FiniteTextDocument::create(text, options);
    if (created) {
      state->document = std::move(*created);
    } else {
      state->failure = detail::textDocumentCause(created.error());
      state->lastError = created.error();
    }
    auto source = detail::ValueModelAccess::deferred<TextDocumentValue>(
      [state](ManagedScene &scene,
              detail::MountContext &context) -> std::expected<ManagedState<TextDocumentValue>, ManagedError> {
        auto valid = state->check();
        if (!valid) {
          return std::unexpected(valid.error());
        }
        auto value = state->snapshot();
        auto handle = detail::Access::createValueState(scene, *value);
        if (!handle) {
          return std::unexpected(handle.error());
        }
        const auto owner = detail::Access::lifetime(scene);
        try {
          context.rollback.emplace_back([state, &scene, owner, handle = *handle]() mutable {
            state->presentation = {};
            state->scene = {};
            state->sceneOwner = nullptr;
            state->sceneLifetime.reset();
            if (!owner.expired()) {
              detail::Access::retireValueState(scene, handle);
            }
          });
        } catch (...) {
          detail::Access::retireValueState(scene, *handle);
          throw;
        }
        state->presentation = *handle;
        state->scene = scene.root().id();
        state->sceneOwner = &scene;
        state->sceneLifetime = owner;

        return *handle;
      });

    return FiniteTextDocument(std::move(state), std::move(source));
  } catch (const std::bad_alloc &) {
    return FiniteTextDocument({}, detail::ModelAccess::failed<TextDocumentValue>(TreeError::NO_MEMORY));
  } catch (...) {
    return FiniteTextDocument({}, detail::ModelAccess::failed<TextDocumentValue>(TreeError::CALLBACK_FAILED));
  }
}

/** @param document Shared finite session. @return Ordinary read-only label bound to current text. */
inline Description<Label> label(const FiniteTextDocument &document) {
  return label(document.source(), [](const TextDocumentValue &value) {
    return value.text;
  });
}
} // namespace portals::ui
