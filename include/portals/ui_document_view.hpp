// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/document_view.hpp>
#include <portals/ui_description.hpp>

namespace portals::ui {
namespace detail {
struct DocumentSourceAccess;
struct DocumentSourceData;
} // namespace detail

/** @brief Owning cold source descriptor; copies retain the same provider, revision namespace and budget.
 * Provider/file descriptors may be constructed before transfer to UI. After their first mount,
 * shared descriptor queries and reuse belong to that UI thread. Explicit low-level sources originate on UI.
 */
class DocumentSource {
public:
  DocumentSource() = default; ///< Empty descriptor; mount reports INVALID_ARGUMENT.
  /** @return Shared byte source after successful UI mount, or the explicitly supplied source; never performs I/O. */
  std::shared_ptr<ByteDocumentSource> byteSource() const noexcept;
  /** @return The source's existing shared credit root, or null for an empty descriptor. */
  std::shared_ptr<SourceBudget> budget() const noexcept;

private:
  friend struct detail::DocumentSourceAccess;
  std::shared_ptr<detail::DocumentSourceData> data_;
  std::optional<DocumentError> error_;
};

/** @param source Existing shared source; pages and view preparation use its existing budget.
 * @return Owning descriptor without inspecting or reading the source. The low-level source must originate on UI.
 */
DocumentSource document(std::shared_ptr<ByteDocumentSource> source);
/** @param provider Owning worker provider. @param budget Existing shared root, or null to create SDK defaults.
 * @param options Bounded source pages. @return Cold descriptor preserving typed creation failures.
 */
DocumentSource document(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget = {},
                        DocumentSourceOptions options = {});
/** @param text Existing shared immutable byte storage, retained without copying the whole input.
 * @param budget Existing shared root, or null for SDK defaults.
 * @return Cold memory descriptor; encoding remains an explicit viewer projection.
 */
DocumentSource document(std::shared_ptr<const std::string> text, std::shared_ptr<SourceBudget> budget = {});
/** @param path Owned UTF-8 path; no file is opened during construction.
 * @return Cold file descriptor using FileLog raw I/O and the standard shared budget.
 */
DocumentSource documentFile(std::string_view path);
/** @param source Owning cold source. @param config Bounded text presentation. @param style Manual appearances.
 * @return Managed retained viewport; runApp owns background execution and cancellation.
 */
Description<DocumentView> viewer(DocumentSource source, DocumentViewConfig config = {}, ControlStyle style = {});
/** @param reference Weak mounted observation. @param source Owning descriptor. @param config Text settings.
 * @param style Manual appearances. @return Viewer recipe publishing Ref after complete mount success.
 */
Description<DocumentView> viewer(Ref<DocumentView> reference, DocumentSource source, DocumentViewConfig config = {},
                                 ControlStyle style = {});
/** @param path Owned UTF-8 file path. @param config Text settings. @param style Manual appearances.
 * @return Cold file viewer recipe; file errors are reported through the scene collector.
 */
Description<DocumentView> viewer(std::string_view path, DocumentViewConfig config = {}, ControlStyle style = {});
/** @param reference Weak mounted observation. @param path Owned UTF-8 file path.
 * @param config Text settings. @param style Manual appearances. @return Ref-first cold file recipe.
 */
Description<DocumentView> viewer(Ref<DocumentView> reference, std::string_view path, DocumentViewConfig config = {},
                                 ControlStyle style = {});
} // namespace portals::ui

namespace portals {
/** @param scene UI owner outside frames. @param node Live managed viewer. @param source Cold owning binding.
 * @return Checked atomic replacement admission; old binding survives a rejected descriptor.
 */
std::expected<void, ManagedError> setDocumentModel(ManagedScene &scene, Node<DocumentView> node,
                                                   ui::DocumentSource source);
/** @param scene UI owner. @param node Live viewer. @param enabled Show proven logical line numbers.
 * @return Deferred reflow admission preserving the original byte anchor.
 */
std::expected<void, ManagedError> setDocumentLineNumbers(ManagedScene &scene, Node<DocumentView> node, bool enabled);
/** @param scene UI owner. @param node Live viewer. @param enabled Wrap ordinary complete graphemes.
 * @return Deferred reflow admission preserving the original byte anchor.
 */
std::expected<void, ManagedError> setDocumentWrap(ManagedScene &scene, Node<DocumentView> node, bool enabled);
/** @param scene UI owner. @param node Live viewer. @param encoding Explicit encoding selection.
 * @return Deferred reflow admission preserving the original byte anchor.
 */
std::expected<void, ManagedError> setDocumentEncoding(ManagedScene &scene, Node<DocumentView> node,
                                                      DocumentEncoding encoding);
} // namespace portals
