// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/search.hpp>

namespace portals {
class LineEdit;
class Dialog;

namespace detail {
struct ChoiceRecord;
struct ChoiceValue;
struct ChoiceAccess;
} // namespace detail

/** @brief Owning accepted row identity and bounded label, independent of editor draft and source lifetime. */
class ChoiceSelection {
public:
  ChoiceSelection() = default;
  /** @return Whether this handle represents an accepted source row. */
  explicit operator bool() const noexcept;
  /** @return Full stable source row identity, or zero for no selection. */
  RowId rowId() const noexcept;
  /** @return Zero-based original source ordinal, or zero for an empty handle. */
  std::uint64_t index() const noexcept;
  /** @return Original UTF-8 label borrowed while this handle owns its value. */
  std::string_view text() const noexcept;
  /** @return Exact accepted source revision, or zero for no selection. */
  std::uint64_t sourceRevision() const noexcept;

private:
  friend struct detail::ChoiceAccess;
  std::shared_ptr<const detail::ChoiceValue> value_;
};

/** @brief Retained editable model-backed choice; the tree owns its ordinary editor and trigger. */
class ComboBox : public Row {
public:
  /** @brief Create an empty retained choice container; declarative recipes attach its editor and source.
   * @param layout Ordinary row alignment, gaps and padding.
   * @param policy Ordinary retained sizing policy.
   */
  explicit ComboBox(LinearOptions layout = {}, SizePolicy policy = {});
  /** @brief Cancel main-control work; scene retirement releases any surviving popup. */
  ~ComboBox() override;
  /** @return Standard editor identity; observe through a checked tree read. */
  Node<LineEdit> editor() const;
  /** @return Existing dialog popup identity, owned by the same scene-level overlay host. */
  Node<Dialog> popup() const;
  /** @return Accepted owning selection; highlight and draft remain independent. */
  ChoiceSelection selection() const;

private:
  friend struct detail::ChoiceAccess;
  std::shared_ptr<detail::ChoiceRecord> record_;
};

/** @brief Read-only main display with complete model-backed popup browsing. */
class DropDown : public ComboBox {
public:
  /** @copydoc portals::ComboBox::ComboBox */
  using ComboBox::ComboBox;
};
} // namespace portals
