// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

namespace portals {
namespace {
ManagedError logError(Node<LogView> node, const LogError &cause) {
  if (const auto *tree = std::get_if<TreeError>(&cause)) {
    return {ManagedPhase::INPUT, node.id(), *tree, {}, {}};
  }

  const auto text = std::get<TextError>(cause);
  const bool limited = text.kind == TextErrorKind::INPUT_LIMIT || text.kind == TextErrorKind::CLUSTER_LIMIT ||
                       text.kind == TextErrorKind::GRAPHEME_LIMIT || text.kind == TextErrorKind::CELL_LIMIT;
  ManagedError failure{
    ManagedPhase::INPUT, node.id(), limited ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT, {}, {}};

  failure.text = text;

  return failure;
}
} // namespace

std::expected<Node<LogView>, ManagedError> ManagedScene::logView(Node<Widget> parent, std::string_view initial,
                                                                 LogViewConfig config, ControlStyle style,
                                                                 SizePolicy policy, std::optional<FrameOptions> frame) {
  const auto allowed = boundary(ManagedPhase::MOUNT, parent);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  const auto valid = validateLogLimits(config.limits);

  if (!valid) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, valid.error()));
  }

  auto node = frame ? emplace<LogView>(parent, widthPolicy(), config, style, policy, std::move(*frame))
                    : emplace<LogView>(parent, widthPolicy(), config, style, policy);

  if (!node) {
    return node;
  }

  const auto appended = appendLogBatch(*node, initial);

  if (!appended) {
    auto failure = appended.error();
    const auto removed = remove(*node);

    if (!removed) {
      if (const auto *cleanup = std::get_if<TreeError>(&removed.error().cause)) {
        failure.cleanup = *cleanup;
      }
    }

    return std::unexpected(failure);
  }

  return node;
}

std::expected<LogAppendReceipt, ManagedError> ManagedScene::appendLogBatch(Node<LogView> node, std::string_view bytes) {
  const auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  const auto result = portals::appendLogBatch(app_.tree(), node, bytes);

  return result ? std::expected<LogAppendReceipt, ManagedError>(*result)
                : std::unexpected(logError(node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::followLogTail(Node<LogView> node) {
  const auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  const auto result = portals::followLogTail(app_.tree(), node);

  return result ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::INPUT, node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::clearLog(Node<LogView> node) {
  const auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  const auto result = portals::clearLog(app_.tree(), node);

  return result ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::INPUT, node, result.error()));
}

std::expected<LogStatus, ManagedError> ManagedScene::logStatus(Node<LogView> node) {
  const auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  const auto result = portals::logStatus(app_.tree(), node);

  return result ? std::expected<LogStatus, ManagedError>(*result)
                : std::unexpected(error(ManagedPhase::INPUT, node, result.error()));
}
} // namespace portals
