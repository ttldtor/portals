// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>
#include <new>
#include <numeric>
#include <portals/layout.hpp>

namespace portals {
namespace {
using Wide = std::int64_t;
constexpr Wide cell_limit = std::numeric_limits<std::int32_t>::max();
thread_local bool layout_active{};

bool valid(AxisSize policy) {
  return policy.minimum >= 0 && policy.maximum >= policy.minimum && policy.preferred >= intrinsic_extent &&
         policy.weight > 0 &&
         (policy.mode == SizeMode::automatic || policy.mode == SizeMode::fixed || policy.mode == SizeMode::flex);
}

bool valid(SizePolicy policy) {
  return valid(policy.width) && valid(policy.height);
}

bool valid(LinearOptions options) {
  auto alignment = [](LayoutAlign value) {
    return value == LayoutAlign::start || value == LayoutAlign::center || value == LayoutAlign::end ||
           value == LayoutAlign::stretch;
  };

  return options.gap >= 0 && options.padding.left >= 0 && options.padding.top >= 0 && options.padding.right >= 0 &&
         options.padding.bottom >= 0 && alignment(options.main_align) && alignment(options.cross_align) &&
         options.overflow == LayoutOverflow::clip;
}

std::expected<std::int32_t, TreeError> cells(Wide value) {
  if (value < 0 || value > cell_limit) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  return static_cast<std::int32_t>(value);
}

Wide requested(AxisSize policy, std::int32_t intrinsic) {
  auto preferred = policy.preferred == intrinsic_extent ? intrinsic : policy.preferred;

  return std::clamp<Wide>(preferred, policy.minimum, policy.maximum);
}

Wide aligned(Wide spare, LayoutAlign align) {
  spare = std::max<Wide>(0, spare);

  if (align == LayoutAlign::center) {
    return spare / 2;
  }

  return align == LayoutAlign::end ? spare : 0;
}

AxisSize main_policy(const MeasuredChild &child, bool horizontal) {
  return horizontal ? child.policy.width : child.policy.height;
}

AxisSize cross_policy(const MeasuredChild &child, bool horizontal) {
  return horizontal ? child.policy.height : child.policy.width;
}

Wide main_desired(const MeasuredChild &child, bool horizontal) {
  return requested(main_policy(child, horizontal), horizontal ? child.desired.width : child.desired.height);
}

Wide cross_desired(const MeasuredChild &child, bool horizontal) {
  return requested(cross_policy(child, horizontal), horizontal ? child.desired.height : child.desired.width);
}

std::expected<void, TreeError> validate_children(std::span<const MeasuredChild> children) {
  for (const auto &child : children) {
    if (!valid(child.policy) || child.desired.width < 0 || child.desired.height < 0) {
      return std::unexpected(TreeError::invalid_argument);
    }
  }

  return {};
}

void grow_flex(std::span<const MeasuredChild> children, bool horizontal, std::vector<Wide> &sizes, Wide spare) {
  while (spare > 0) {
    Wide weights{};

    for (std::size_t i = 0; i < children.size(); ++i) {
      auto policy = main_policy(children[i], horizontal);

      if (policy.mode == SizeMode::flex && sizes[i] < policy.maximum) {
        weights += policy.weight;
      }
    }

    if (weights == 0) {
      break;
    }

    const Wide round_budget = spare;
    Wide assigned{};

    for (std::size_t i = 0; i < children.size(); ++i) {
      auto policy = main_policy(children[i], horizontal);

      if (policy.mode == SizeMode::flex && sizes[i] < policy.maximum) {
        auto share = std::min<Wide>(policy.maximum - sizes[i], round_budget * policy.weight / weights);

        sizes[i] += share;
        assigned += share;
      }
    }

    spare -= assigned;

    // At most one remainder cell per eligible child in this round; caps force another round.
    for (std::size_t i = 0; i < children.size() && spare > 0; ++i) {
      auto policy = main_policy(children[i], horizontal);

      if (policy.mode == SizeMode::flex && sizes[i] < policy.maximum) {
        ++sizes[i];
        --spare;
      }
    }
  }
}

struct Entry {
  Node<Widget> node;
  std::vector<std::size_t> children = std::vector<std::size_t>(0);
  MeasuredChild measured;

  Entry() = default;

  explicit Entry(Node<Widget> value) : node(value) {
  }

  Entry(const Entry &) = default;
};
class Traversal {
public:
  explicit Traversal(WidgetTree &tree) : tree_(tree) {
  }

  std::expected<std::size_t, TreeError> measure(Node<Widget> node, CellSize available) {
    auto snapshot = tree_.inspect(node);

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    const auto index = entries_.size();
    CellSize child_bounds = available;
    std::expected<void, TreeError> status;

    entries_.emplace_back(node);
    entries_[index].measured.node = node.id();

    auto read = tree_.read(node, [&](const Widget &widget) {
      if (auto item = dynamic_cast<const LayoutItem *>(&widget)) {
        entries_[index].measured.policy = item->size_policy();
      }

      if (auto layout = dynamic_cast<const Layout *>(&widget)) {
        auto result = layout->child_available(available);

        if (result) {
          child_bounds = *result;
        } else {
          status = std::unexpected(result.error());
        }
      }
    });

    if (!read || !status) {
      return std::unexpected(read ? status.error() : read.error());
    }

    if (!valid(entries_[index].measured.policy)) {
      return std::unexpected(TreeError::invalid_argument);
    }

    for (auto child : snapshot->children) {
      auto child_snapshot = tree_.inspect(child);

      if (!child_snapshot) {
        return std::unexpected(child_snapshot.error());
      }

      if (!child_snapshot->visible) {
        continue;
      }

      auto result = measure(child, child_bounds);

      if (!result) {
        return std::unexpected(result.error());
      }

      entries_[index].children.push_back(*result);
    }

    auto intrinsic = tree_.measure(node, available);

    if (!intrinsic) {
      return std::unexpected(intrinsic.error());
    }

    auto children = child_inputs(index);

    entries_[index].measured.desired = *intrinsic;
    read = tree_.read(node, [&](const Widget &widget) {
      if (auto layout = dynamic_cast<const Layout *>(&widget)) {
        auto result = layout->measure_children(children);

        if (result) {
          entries_[index].measured.desired = result->normalized();
        } else {
          status = std::unexpected(result.error());
        }
      } else if (children.size() > 1) {
        status = std::unexpected(TreeError::invalid_argument);
      } else if (children.size() == 1) {
        entries_[index].measured.desired = children.front().desired;
      }
    });

    if (!read || !status) {
      return std::unexpected(read ? status.error() : read.error());
    }

    return index;
  }

  std::expected<void, TreeError> arrange(std::size_t index, CellRect local, CellRect viewport) {
    auto result = tree_.arrange(entries_[index].node, local, viewport);

    if (!result) {
      return result;
    }

    auto children = child_inputs(index);
    std::expected<std::vector<CellRect>, TreeError> placements(std::in_place, 0);
    auto read = tree_.read(entries_[index].node, [&](const Widget &widget) {
      if (auto layout = dynamic_cast<const Layout *>(&widget)) {
        placements = layout->arrange_children(children, local.size());
      } else if (!children.empty()) {
        auto rect = CellRect::from({}, local.size());

        if (rect) {
          try {
            placements->push_back(*rect);
          } catch (const std::bad_alloc &) {
            placements = std::unexpected(TreeError::no_memory);
          }
        } else {
          placements = std::unexpected(TreeError::invalid_geometry);
        }
      }
    });

    if (!read || !placements) {
      return std::unexpected(read ? placements.error() : read.error());
    }

    if (placements->size() != entries_[index].children.size()) {
      return std::unexpected(TreeError::invalid_argument);
    }

    for (std::size_t i = 0; i < placements->size(); ++i) {
      auto child_result = arrange(entries_[index].children[i], (*placements)[i], viewport);

      if (!child_result) {
        return child_result;
      }
    }

    return {};
  }

private:
  std::vector<MeasuredChild> child_inputs(std::size_t index) const {
    std::vector<MeasuredChild> result(0);
    result.reserve(entries_[index].children.size());

    for (auto child : entries_[index].children) {
      result.push_back(entries_[child].measured);
    }

    // Copy into an elided result: MSVC Debug vector moves can allocate inside noexcept.
    return std::vector<MeasuredChild>(result);
  }

  WidgetTree &tree_;
  std::vector<Entry> entries_ = std::vector<Entry>(0);
};
} // namespace

SizedWidget::SizedWidget(SizePolicy policy) : policy_(policy) {
}

SizePolicy SizedWidget::size_policy() const {
  return policy_;
}

LinearLayout::LinearLayout(bool horizontal, LinearOptions options, SizePolicy policy)
    : SizedWidget(policy), horizontal_(horizontal), options_(options) {
}

Row::Row(LinearOptions options, SizePolicy policy) : LinearLayout(true, options, policy) {
}

Column::Column(LinearOptions options, SizePolicy policy) : LinearLayout(false, options, policy) {
}

LinearOptions LinearLayout::options() const {
  return options_;
}

std::expected<CellSize, TreeError> LinearLayout::child_available(CellSize available) const {
  if (!valid(options_)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  auto size = available.normalized();
  auto width = cells(std::max<Wide>(0, Wide{size.width} - options_.padding.left - options_.padding.right));
  auto height = cells(std::max<Wide>(0, Wide{size.height} - options_.padding.top - options_.padding.bottom));

  if (!width || !height) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  return CellSize{*width, *height};
}

std::expected<CellSize, TreeError> LinearLayout::measure_children(std::span<const MeasuredChild> children) const {
  auto checked = validate_children(children);

  if (!valid(options_) || !checked) {
    return std::unexpected(TreeError::invalid_argument);
  }

  Wide main{}, cross{};

  for (const auto &child : children) {
    main += main_desired(child, horizontal_);
    cross = std::max(cross, cross_desired(child, horizontal_));

    if (main > cell_limit) {
      return std::unexpected(TreeError::invalid_geometry);
    }
  }

  if (!children.empty()) {
    main += Wide{options_.gap} * static_cast<Wide>(children.size() - 1);
  }

  auto width = cells((horizontal_ ? main : cross) + options_.padding.left + Wide{options_.padding.right});
  auto height = cells((horizontal_ ? cross : main) + options_.padding.top + Wide{options_.padding.bottom});

  if (!width || !height) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  return CellSize{*width, *height};
}

std::expected<std::vector<CellRect>, TreeError> LinearLayout::arrange_children(std::span<const MeasuredChild> children,
                                                                               CellSize available) const {
  auto checked = validate_children(children);
  auto content = child_available(available);

  if (!checked || !content) {
    return std::unexpected(checked ? content.error() : checked.error());
  }

  try {
    const Wide main_space = horizontal_ ? content->width : content->height;
    const Wide cross_space = horizontal_ ? content->height : content->width;
    const Wide gaps = children.empty() ? 0 : Wide{options_.gap} * static_cast<Wide>(children.size() - 1);
    std::vector<Wide> sizes(0);
    Wide total = gaps;

    sizes.reserve(children.size());

    for (const auto &child : children) {
      sizes.push_back(main_desired(child, horizontal_));
      total += sizes.back();
    }

    Wide deficit = std::max<Wide>(0, total - main_space);

    // Later auto/flex children yield preferred space first; fixed and minimum survive.
    for (std::size_t i = children.size(); i > 0 && deficit > 0; --i) {
      auto policy = main_policy(children[i - 1], horizontal_);

      if (policy.mode != SizeMode::fixed) {
        auto reduction = std::min(deficit, sizes[i - 1] - policy.minimum);

        sizes[i - 1] -= reduction;
        deficit -= reduction;
      }
    }

    total = std::accumulate(sizes.begin(), sizes.end(), gaps);
    grow_flex(children, horizontal_, sizes, std::max<Wide>(0, main_space - total));
    total = std::accumulate(sizes.begin(), sizes.end(), gaps);

    Wide cursor =
      (horizontal_ ? options_.padding.left : options_.padding.top) + aligned(main_space - total, options_.main_align);
    std::vector<CellRect> result(0);

    result.reserve(children.size());

    for (std::size_t i = 0; i < children.size(); ++i) {
      auto policy = cross_policy(children[i], horizontal_);
      Wide cross = cross_desired(children[i], horizontal_);

      if (policy.mode != SizeMode::fixed) {
        cross = std::clamp<Wide>((options_.cross_align == LayoutAlign::stretch || policy.mode == SizeMode::flex)
                                   ? cross_space
                                   : std::min(cross, cross_space),
                                 policy.minimum, policy.maximum);
      }

      auto start = cells(cursor);
      auto cross_start = cells((horizontal_ ? options_.padding.top : options_.padding.left) +
                               aligned(cross_space - cross, options_.cross_align));
      auto extent = cells(sizes[i]);
      auto cross_extent = cells(cross);

      if (!start || !cross_start || !extent || !cross_extent) {
        return std::unexpected(TreeError::invalid_geometry);
      }

      auto rect = horizontal_ ? CellRect::from({*start, *cross_start}, {*extent, *cross_extent})
                              : CellRect::from({*cross_start, *start}, {*cross_extent, *extent});

      if (!rect) {
        return std::unexpected(TreeError::invalid_geometry);
      }

      result.push_back(*rect);
      cursor += sizes[i] + options_.gap;
    }

    // Construct the expected directly from a copy; its elided return avoids noexcept vector moves.
    return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

std::expected<void, TreeError> set_size_policy(WidgetTree &tree, Node<SizedWidget> node, SizePolicy policy) {
  if (!valid(policy)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  auto result = tree.edit(node, [&](SizedWidget &widget) {
    widget.policy_ = policy;
  });

  if (!result) {
    return result;
  }

  return tree.invalidate(node, Invalidation::layout);
}

std::expected<void, TreeError> set_linear_options(WidgetTree &tree, Node<LinearLayout> node, LinearOptions options) {
  if (!valid(options)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  auto result = tree.edit(node, [&](LinearLayout &widget) {
    widget.options_ = options;
  });

  if (!result) {
    return result;
  }

  return tree.invalidate(node, Invalidation::layout);
}

std::expected<void, TreeError> layout_tree(WidgetTree &tree, Node<Widget> node, CellRect local, CellRect viewport) {
  if (layout_active) {
    return std::unexpected(TreeError::would_block);
  }

  struct Guard {
    Guard() {
      layout_active = true;
    }
    ~Guard() {
      layout_active = false;
    }
  } guard;

  try {
    // Enabled is irrelevant to layout: check visibility of each ancestor explicitly.
    auto ancestor = node;

    for (;;) {
      auto state = tree.inspect(ancestor);

      if (!state) {
        return std::unexpected(state.error());
      }

      if (!state->visible) {
        return {};
      }

      if (state->parent == Node<Widget>{}) {
        break;
      }

      ancestor = state->parent;
    }

    Traversal traversal(tree);
    auto measured = traversal.measure(node, local.size());

    if (!measured) {
      return std::unexpected(measured.error());
    }

    return traversal.arrange(*measured, local, viewport);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}
} // namespace portals