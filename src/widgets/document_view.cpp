// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "document_view_state.hpp"
#include <algorithm>

namespace portals::detail {
ScrollAreaOptions documentScrollOptions() {
  ScrollAreaOptions options;
  options.wheelStep = 1;
  options.horizontal = ScrollBarPolicy::NEVER;
  options.vertical = ScrollBarPolicy::AUTO;
  options.placement = ScrollPlacement::ON_FRAME;
  options.frame = BorderOptions{};
  return options;
}

} // namespace portals::detail

namespace portals {
namespace {
SourceError documentTreeError(TreeError error) noexcept {
  switch (error) {
  case TreeError::WRONG_THREAD:
    return SourceError::WRONG_THREAD;
  case TreeError::NO_MEMORY:
    return SourceError::NO_MEMORY;
  case TreeError::CLOSED:
    return SourceError::CLOSED;
  case TreeError::WOULD_BLOCK:
    return SourceError::WOULD_BLOCK;
  default:
    return SourceError::INVALID_ARGUMENT;
  }
}
} // namespace

DocumentView::DocumentView(WidthPolicy width, DocumentViewConfig config, ControlStyle style, FrameOptions frame,
                           std::optional<ScrollAreaOptions> scroll)
    : ScrollArea({}, width, scroll.value_or(detail::documentScrollOptions()), style, DEFAULT_VIEWPORT_POLICY,
                 std::move(frame), std::in_place),
      document_(std::make_shared<detail::DocumentViewState>()) {
  document_->width = width;
  document_->config = config;
}

DocumentView::~DocumentView() = default;

std::expected<CellSize, TreeError> DocumentView::measureChildren(std::span<const MeasuredChild> children) const {
  if (!children.empty()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto &scroll = detail::ScrollWidgetAccess::state(*this);
  const auto insets = detail::scrollFrameInsets(scroll);
  const auto frame = document_->frame;
  const auto rows = frame ? frame->rows.size() : 0;
  const auto width = frame ? frame->viewport.width : 0;
  // The vertical chrome projects byte position, never the size of the resident row window.
  // Unknown extents have no global thumb; keyboard and wheel navigation remain available.
  scroll.extent = {static_cast<std::uint64_t>(std::max(width, 0)),
                   frame && frame->status.byteExtent ? *frame->status.byteExtent : 0};

  return CellSize{width + insets.left + insets.right, static_cast<std::int32_t>(rows) + insets.top + insets.bottom};
}

std::expected<void, TreeError> DocumentView::paint(PaintContext &context) {
  // Capture owning payload and value-only presentation before sink callbacks.
  // Submission uses only captured owning payload and value-only presentation.
  const auto &scroll = detail::ScrollWidgetAccess::state(*this);
  const auto viewport = CellRect::from(scroll.geometry.viewportOrigin, scroll.geometry.viewport);
  const bool arranged = scroll.arranged;
  const auto frame = document_->frame;
  const auto palette = document_->roles.snapshot();
  std::optional<ComputedStyle> manual;
  const auto computed = computedStyle();
  if (!computed) {
    manual.emplace(styleDefaults());
  }
  const auto resolved = computed ? detail::ComputedStyleView(*computed) : detail::ComputedStyleView(*manual);
  const auto state = !scroll.enabled ? StyleState::DISABLED : scroll.focused ? StyleState::FOCUSED : StyleState::NORMAL;
  const auto surface = !scroll.enabled ? resolved.disabled : scroll.focused ? resolved.focused : resolved.normal;
  const auto roleStyle = [&](std::string_view name) {
    return palette ? palette->find(name, state).value_or(surface) : surface;
  };
  const auto body = roleStyle("body");
  const auto gutter = roleStyle("gutter");
  const auto position = roleStyle("position");
  const auto *options = detail::scrollFrameOptions(scroll);
  std::optional<CellRect> footer;
  const bool currentPosition =
    frame && frame->footer && !document_->status.error && (!document_->status.waiting || frame->status.waiting);
  if (currentPosition && options && options->sides.bottom && scroll.allocation.width > 2 &&
      scroll.allocation.height > 1) {
    auto area = CellRect::from({1, scroll.allocation.height - 1}, {scroll.allocation.width - 2, 1});
    if (area) {
      footer = *area;
    }
  }
  const auto chrome = ScrollArea::paint(context);
  if (!chrome) {
    return chrome;
  }
  if (!arranged || !viewport || viewport->empty()) {
    return {};
  }
  const auto gutterCells = frame ? std::min(frame->gutterCells, viewport->size().width) : 0;
  const auto bodyArea = CellRect::from({viewport->origin().x + gutterCells, viewport->origin().y},
                                       {viewport->size().width - gutterCells, viewport->size().height});
  const auto gutterArea = CellRect::from(viewport->origin(), {gutterCells, viewport->size().height});
  if (!bodyArea || !gutterArea) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  const auto fillPart = [&](CellRect area, CellStyle paint) {
    if (area.empty()) {
      return std::expected<void, TreeError>{};
    }
    const RenderCommand fill = FillCommand{area, area, paint};
    return context.submit(std::span(&fill, 1));
  };
  auto painted = fillPart(*bodyArea, body);
  if (!painted) {
    return painted;
  }
  painted = fillPart(*gutterArea, gutter);
  if (!painted || !frame) {
    return painted;
  }
  const auto count = std::min<std::size_t>(frame->rows.size(), static_cast<std::size_t>(viewport->size().height));
  for (std::size_t row = 0; row < count; ++row) {
    const auto y = viewport->origin().y + static_cast<std::int32_t>(row);
    if (!bodyArea->empty()) {
      const RenderCommand command = TextCommand{{bodyArea->origin().x, y}, frame->rows[row], *bodyArea, body};
      painted = context.submit(std::span(&command, 1));
      if (!painted) {
        return painted;
      }
    }
    if (!gutterArea->empty() && frame->gutters && row < frame->gutters->size()) {
      const RenderCommand command =
        TextCommand{{gutterArea->origin().x, y}, (*frame->gutters)[row], *gutterArea, gutter};
      painted = context.submit(std::span(&command, 1));
      if (!painted) {
        return painted;
      }
    }
  }
  if (footer) {
    const RenderCommand command = TextCommand{footer->origin(), *frame->footer, *footer, position};
    return context.submit(std::span(&command, 1));
  }
  return {};
}

std::expected<void, SourceError> seekDocumentByte(WidgetTree &tree, Node<DocumentView> node, std::uint64_t offset) {
  std::expected<void, SourceError> result;
  const auto edited = tree.edit(node, [&](DocumentView &widget) {
    auto &state = detail::DocumentViewAccess::state(widget);
    if (!state.source || !state.activity) {
      result = std::unexpected(SourceError::INVALID_ARGUMENT);
      return;
    }
    if (!state.source->capabilities().seekable && offset != state.anchor) {
      result = std::unexpected(SourceError::UNSUPPORTED);
      return;
    }
    state.anchor = offset;
    state.rowDelta = 0;
    state.tail = false;
    state.refresh = true;
    state.status.preparing = true;
    ++state.serial;
  });
  if (!edited) {
    return std::unexpected(documentTreeError(edited.error()));
  }
  if (!result) {
    return result;
  }
  const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);
  return invalidated ? std::expected<void, SourceError>{} : std::unexpected(documentTreeError(invalidated.error()));
}

std::expected<void, SourceError> refreshDocument(WidgetTree &tree, Node<DocumentView> node) {
  std::expected<void, SourceError> result;
  const auto edited = tree.edit(node, [&](DocumentView &widget) {
    auto &state = detail::DocumentViewAccess::state(widget);
    if (!state.activity) {
      result = std::unexpected(SourceError::INVALID_ARGUMENT);
      return;
    }
    state.refresh = true;
    state.status.preparing = true;
    ++state.serial;
  });
  if (!edited) {
    return std::unexpected(documentTreeError(edited.error()));
  }
  if (!result) {
    return result;
  }
  const auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);
  return invalidated ? std::expected<void, SourceError>{} : std::unexpected(documentTreeError(invalidated.error()));
}

std::expected<DocumentViewStatus, TreeError> documentViewStatus(WidgetTree &tree, Node<DocumentView> node) {
  DocumentViewStatus status;
  const auto read = tree.read(node, [&](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);
    status = state.status;
    status.byteOffset = state.anchor;
  });
  return read ? std::expected<DocumentViewStatus, TreeError>(status) : std::unexpected(read.error());
}
} // namespace portals
