// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/text_history.hpp>

int main() {
  using namespace portals;

  TextHistory history;
  auto text = GraphemeText::from("");

  if (!text) {
    return 1;
  }

  // Combining accents and fullwidth characters remain complete graphemes.
  TextHistorySnapshot before{};
  TextHistorySnapshot after{"e\xcc\x81\xe7\x95\x8c", 6, 6};
  const auto expectedBytes = after.bytes;
  const auto expectedCursor = after.cursor;
  auto edit = history.prepareEdit(before, after, TextHistoryKind::PROGRAMMATIC);

  if (!edit || !history.checkPrepared(*edit)) {
    return 2;
  }

  // Caller-owned inputs may change before acceptance and every later traversal.
  before.bytes.assign("replaced baseline snapshot");
  after.bytes.assign("replaced edited snapshot");

  edit->takeText(*text);

  if (!history.commit(std::move(*edit)) || !history.canUndo() || history.canRedo() || text->bytes() != expectedBytes) {
    return 3;
  }

  auto undo = history.prepareUndo();

  if (!undo || !history.checkPrepared(*undo) || undo->cursor() != 0 || undo->anchor() != 0) {
    return 4;
  }

  undo->takeText(*text);

  if (!history.commit(std::move(*undo)) || history.canUndo() || !history.canRedo() || !text->bytes().empty()) {
    return 5;
  }

  auto redo = history.prepareRedo();

  if (!redo || !history.checkPrepared(*redo) || redo->cursor() != expectedCursor || redo->anchor() != expectedCursor) {
    return 6;
  }

  redo->takeText(*text);

  if (!history.commit(std::move(*redo)) || text->bytes() != expectedBytes || history.operationCount() != 1) {
    return 7;
  }

  const TextHistorySnapshot baseline{expectedBytes, expectedCursor, expectedCursor};
  auto reset = history.prepareResetBaseline(baseline);

  if (!reset || !history.checkPrepared(*reset)) {
    return 8;
  }

  reset->takeText(*text);

  return history.commit(std::move(*reset)) && !history.canUndo() && !history.canRedo() ? 0 : 9;
}
