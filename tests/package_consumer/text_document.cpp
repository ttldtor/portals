// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/text_document.hpp>

int main() {
  auto created = portals::FiniteTextDocument::create("a\xcc\x81"
                                                     "b");

  if (!created) {
    return 1;
  }

  auto &document = **created;
  const auto identity = document.resource();
  auto bookmark = document.bookmark(3, portals::PositionAffinity::FORWARD);

  if (!bookmark || !document.setSelection({3, 0}) || !document.replace(3, 3, "!")) {
    return 2;
  }

  portals::PositionRequest request{document.session(), document.revision(), 1, *bookmark};
  const portals::PositionResolver &resolver = document;
  auto reply = resolver.resolve(request, {});

  if (!reply || !portals::positionReplyMatches(request, *reply) || reply->state != portals::PositionResolution::EXACT ||
      reply->position.offset != 4) {
    return 3;
  }

  auto save = document.beginSave("renamed.txt");

  if (!save || !document.acknowledgeSave(*save) || document.dirty() || document.resource() != identity ||
      !document.canUndo() || !document.undo() || document.selection() != portals::TextSelection{3, 0} ||
      !document.dirty() || !document.redo() || document.dirty()) {
    return 4;
  }

  document.observeExternalReplacement();

  if (!document.conflicted() || document.replace(0, 0, "x") || !document.acceptExternalReplacement("new") ||
      document.resource().lineage == identity.lineage) {
    return 5;
  }

  return 0;
}
