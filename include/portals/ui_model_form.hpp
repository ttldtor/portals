// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_model_edit.hpp>

#include <array>
#include <charconv>
#include <cmath>

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
template <class T>
concept DefaultFormScalar =
  std::same_as<T, std::string> || (((std::integral<T> || std::floating_point<T>) && !std::same_as<T, bool>) &&
                                   requires(T value, char *output, const char *input) {
                                     std::to_chars(output, output, value);
                                     std::from_chars(input, input, value);
                                   });

template <DefaultFormScalar Value> std::expected<std::string, TreeError> formatFormScalar(const Value &value) {
  if constexpr (std::same_as<Value, std::string>) {
    return std::expected<std::string, TreeError>(std::in_place, value);
  } else {
    // General floating-point notation has a bounded mantissa and an int-sized decimal exponent.
    constexpr auto SIGN_AND_ROUNDING_BYTES = 3;
    constexpr auto EXPONENT_PUNCTUATION_BYTES = 4;
    constexpr auto CONVERSION_BYTES =
      std::integral<Value>
        ? std::numeric_limits<Value>::digits10 + SIGN_AND_ROUNDING_BYTES
        : std::numeric_limits<Value>::max_digits10 + std::numeric_limits<int>::digits10 + EXPONENT_PUNCTUATION_BYTES;
    std::array<char, CONVERSION_BYTES> buffer{};

    if constexpr (std::floating_point<Value>) {
      if (!std::isfinite(value)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);

    if (converted.ec != std::errc{}) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    return std::expected<std::string, TreeError>(std::in_place, buffer.data(),
                                                 static_cast<std::size_t>(converted.ptr - buffer.data()));
  }
}

template <DefaultFormScalar Value> std::expected<Value, TreeError> parseFormScalar(std::string_view text) {
  if constexpr (std::same_as<Value, std::string>) {
    return std::expected<Value, TreeError>(std::in_place, text.data(), text.size());
  } else {
    if (text.empty()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    Value value{};

    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);

    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if constexpr (std::floating_point<Value>) {
      if (!std::isfinite(value)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    return value;
  }
}

template <DefaultFormScalar Value> std::shared_ptr<const EditTextCodec<Value>> defaultFormCodec() {
  static const auto CODEC = std::make_shared<const EditTextCodec<Value>>(
    EditTextCodec<Value>{.format = formatFormScalar<Value>, .parse = parseFormScalar<Value>});

  return CODEC;
}

/// @endcond
} // namespace detail

/** @brief Owning declaration of one current-record field; no widget or edit runtime is created here.
 * @tparam Row Owning copyable record shared by navigation and its edit session.
 * @tparam Value Owning copyable member value; default text editors support string and arithmetic except bool.
 * Other values use an explicit codec/editor and a compatible endpoint ownership/accounting contract. Member pointers
 * are local selectors, not persistent schema identities. Copying this declaration retains its callbacks, caption and
 * codec.
 */
template <class Row, class Value> class FormFieldMapping {
public:
  /** @brief Keep owning configuration transfers fallible before mount. */
  ~FormFieldMapping() = default;

  /** @param caption Literal UTF-8 label copied now. @param member Local record selector.
   * Owns its declaration and retains no record/page reference.
   */
  FormFieldMapping(std::string_view caption, Value Row::*member) : caption_(caption), member_(member) {
  }

  /** @param enabled Reject empty scalar text at publication and show the native required marker.
   * @return This declaration with the standard FormField required policy.
   */
  FormFieldMapping &required(bool enabled = true) {
    config_.required = enabled;

    return *this;
  }

  /** @param enabled Block user editing with the existing native policy; explicit typed commands remain checked.
   * @return This declaration retaining navigation and readable current-record values.
   */
  FormFieldMapping &readOnly(bool enabled = true) {
    config_.editor.options.readOnly = enabled;

    return *this;
  }

  /** @param rule Owning typed synchronous rule; borrowed candidates expire when it returns.
   * @return This declaration with an additional typed publication rule.
   */
  FormFieldMapping &validate(std::function<bool(const Value &)> rule) {
    rule_ = std::move(rule);

    return *this;
  }

  /** @param config Existing FormField text validation, async validation, messages and editor settings.
   * @return This declaration replacing its ordinary native configuration.
   */
  FormFieldMapping &options(FormFieldConfig config) {
    auto caption = std::move(config_.captionRecipe);

    config_ = std::move(config);
    if (caption) {
      config_.captionRecipe = std::move(caption);
    }

    return *this;
  }

  /** @param reference Ordinary FormField observation published after the complete form mounts.
   * @return This declaration using the existing Ref family; use reference.editor() for its LineEdit.
   */
  FormFieldMapping &reference(Ref<FormField> reference) {
    reference_ = std::move(reference);

    return *this;
  }

  /** @param key Nonempty owning schema identity for requests; omitted uses the session's local member key.
   * @return This declaration using the explicit key at mapping time.
   */
  FormFieldMapping &key(std::string_view key) {
    key_ = key;

    return *this;
  }

  /** @param codec Owning complete scalar conversion, retained together with target-local text history.
   * @return This declaration with an explicit codec; copies share the same codec owner.
   * A field cannot be remapped to a different codec while its edit session retains the original representation.
   */
  FormFieldMapping &codec(EditTextCodec<Value> codec) {
    codec_ = std::make_shared<const EditTextCodec<Value>>(std::move(codec));

    return *this;
  }

  /** @tparam Factory Copyable owning function returning an ordinary widget recipe.
   * @param factory Receives the typed field and requested user read-only policy. It must bind its real property
   * through the typed field/endpoint and honor this policy in its editor. The wrapper supplies captions/help and
   * required marker; custom value validation, history, focus and source-readonly presentation belong to the factory.
   * @return This declaration using a real custom editor; no arbitrary widget-property connection is inferred.
   */
  template <class Factory> FormFieldMapping &editor(Factory factory) {
    factory_ = [factory = std::move(factory)](EditField<Row, Value> field, bool readOnly) mutable {
      return detail::Access::erase(std::invoke(factory, std::move(field), readOnly));
    };

    return *this;
  }

  /// @cond INTERNAL
  template <class Caption> void captionSource(Caption source) {
    config_.captionRecipe = [source = std::move(source)](ControlStyle style) {
      return ui::label(source, style);
    };
  }

  Description<FormField> map(const EditSession<Row> &session) const {
    auto field = key_.empty() ? session.field(member_) : session.field(key_, member_);
    auto codec = codec_;

    if constexpr (detail::DefaultFormScalar<Value> && !std::same_as<Value, std::string>) {
      if (!codec) {
        codec = detail::defaultFormCodec<Value>();
      }
    }

    if (!factory_) {
      if constexpr (std::same_as<Value, std::string>) {
        if (!codec && field.definition_ && field.definition_->textCodec) {
          return detail::boundFormField(caption_, EditField<Row, Value>{}, config_).reference(reference_);
        }
      }
      if (codec && !detail::configureEditTextCodec(field, codec)) {
        return detail::boundFormField(caption_, EditField<Row, Value>{}, config_).reference(reference_);
      }
    }

    if (rule_) {
      field.editorValidate_ = std::make_shared<const std::function<bool(const Value &)>>(rule_);
    }

    if (factory_) {
      FormFieldOptions options{.help = config_.help,
                               .required = config_.required,
                               .requiredMessage = config_.requiredMessage,
                               .appearance = config_.appearance};

      return detail::Access::formField(caption_, factory_(field, config_.editor.options.readOnly), std::move(options),
                                       config_.captionRecipe)
        .reference(reference_);
    }
    if constexpr (std::same_as<Value, std::string>) {
      if (!codec) {
        // Preserve the native string path and its pre-copy retention admission.
        return detail::boundFormField(caption_, std::move(field), config_).reference(reference_);
      }
    }

    if (!codec) {
      field = {};
    }

    return detail::boundFormField(caption_, std::move(field), config_).reference(reference_);
  }

  /// @endcond

private:
  std::string caption_;
  Value Row::*member_{};
  std::string key_{"", 0};
  FormFieldConfig config_;
  Ref<FormField> reference_;
  std::function<bool(const Value &)> rule_;
  std::shared_ptr<const EditTextCodec<Value>> codec_;
  std::function<Description<Widget>(EditField<Row, Value>, bool)> factory_;
};

/** @tparam Row Owning record type. @tparam Value Owning member type.
 * @param caption Literal UTF-8 copied now. @param member Local pointer-to-member selector.
 * @return Cold owning field declaration for form(); default scalar text editors are bounded LineEdits.
 */
template <class Row, class Value> FormFieldMapping<Row, Value> field(std::string_view caption, Value Row::*member) {
  return FormFieldMapping<Row, Value>(caption, member);
}

/** @tparam Caption Existing owning Label caption source: TextValue, live scalar/model or ValueText expression.
 * @tparam Row Owning record type. @tparam Value Owning member type.
 * @param caption Existing caption source retained by its normal Label recipe. Explicit locale dependencies
 * use the existing text(i18n(...), resolver, locale) mechanism; no ambient language state is added.
 * @param member Local pointer-to-member selector.
 * @return Owning mapping whose caption uses the existing live Label delivery and error policy.
 */
template <class Caption, class Row, class Value>
  requires(!std::convertible_to<Caption, std::string_view>) && requires(Caption caption) { ui::label(caption); }
FormFieldMapping<Row, Value> field(Caption caption, Value Row::*member) {
  FormFieldMapping<Row, Value> result("", member);

  result.captionSource(std::move(caption));

  return result;
}

namespace detail {
/// @cond INTERNAL
template <class Row, class Value>
Description<FormField> formChild(const EditSession<Row> &session, const FormFieldMapping<Row, Value> &mapping) {
  return mapping.map(session);
}

template <class Row, DescriptionInput Child> Child &&formChild(const EditSession<Row> &, Child &&child) {
  return std::forward<Child>(child);
}

/// @endcond
} // namespace detail

/** @tparam Row Owning record shared by all mappings. @tparam Mappings Field declarations for that record or consuming
 * ordinary widget recipes.
 * @param session Existing current-record edit session, preserving its modes, credit and exact-target reply contract.
 * @param mappings Field declarations and ordinary children consumed in argument order.
 * @return Ordinary Column with native gap one; customize it through the existing layout/fluent API.
 * No current row renders empty inactive editors. Readonly sources remain navigable. Commit is per-field unless
 * an endpoint supplies stronger capability; the form adds no multi-field transaction guarantee.
 */
template <class Row, class... Mappings>
Description<Column> form(const EditSession<Row> &session, Mappings &&...mappings) {
  return vbox(LinearOptions{.gap = 1}, detail::formChild(session, std::forward<Mappings>(mappings))...);
}
} // namespace portals::ui
