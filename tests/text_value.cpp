// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/text_value.hpp>

#include <memory>
#include <stdexcept>

namespace {
static_assert(portals::text("literal").resourceKey().empty());
static_assert(portals::i18n("currency").resourceKey() == "currency");
static_assert(std::copy_constructible<portals::TextValue>);
static_assert(std::move_constructible<portals::TextValue>);
static_assert(std::is_copy_assignable_v<portals::TextValue>);
static_assert(std::is_move_assignable_v<portals::TextValue>);
#if defined(_MSVC_STL_VERSION) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
static_assert(!std::is_nothrow_move_constructible_v<portals::TextValue>);
static_assert(!std::is_nothrow_move_assignable_v<portals::TextValue>);
#else
static_assert(std::is_nothrow_move_constructible_v<portals::TextValue>);
static_assert(std::is_nothrow_move_assignable_v<portals::TextValue>);
#endif
static_assert([] {
  auto original = portals::i18n("key");
  auto copied = original;
  auto transferred = std::move(copied);

  copied = std::move(transferred);

  return copied.resourceKey() == "key";
}());

struct ThrowingValue {};
} // namespace

TEST_CASE("text expressions preserve owning copy and rvalue construction assignment") {
  auto original = portals::i18n("owned-key");
  auto copied = original;
  auto transferred = std::move(original);

  CHECK(copied.resourceKey() == "owned-key");
  CHECK(transferred.resourceKey() == "owned-key");

  copied = portals::text("literal");
  transferred = std::move(copied);

  const auto value = transferred.resolve({});

  REQUIRE(value);
  CHECK(value->bytes() == "literal");
}

template <> struct std::formatter<ThrowingValue> : std::formatter<std::string_view> {
  auto format(const ThrowingValue &, std::format_context &) const -> std::format_context::iterator {
    throw std::runtime_error("custom formatter failed");
  }
};

TEST_CASE("literal and formatted expressions own bytes after source destruction") {
  using namespace portals;
  auto literal = [] {
    std::string source = "saved";
    auto value = text(source);

    source.assign("changed");

    return value;
  }();
  const auto formatted = text("{} {:.2f}", std::string("Price"), 123.0);
  const auto literalResult = literal.resolve(WidthPolicy{});
  const auto formattedResult = formatted.resolve(WidthPolicy{});
  const auto braces = text("{}").resolve(WidthPolicy{});

  REQUIRE(literalResult);
  REQUIRE(formattedResult);
  REQUIRE(braces);
  CHECK(literalResult->bytes() == "saved");
  CHECK(formattedResult->bytes() == "Price 123.00");
  CHECK(braces->bytes() == "{}");
  CHECK(literal.resourceKey().empty());
  CHECK(formatted.resourceKey().empty());
}

TEST_CASE("resource expressions own keys and resolve current caller content") {
  using namespace portals;
  const auto value = [] {
    std::string key = "currency";
    auto expression = i18n(key);

    key.assign("different");

    return expression;
  }();
  std::string translation = "USD";
  int calls = 0;
  const auto resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++calls;
    CHECK(key == "currency");

    return translation;
  };
  const auto first = value.resolve(WidthPolicy{}, resolver);

  translation = "EUR";

  const auto second = value.resolve(WidthPolicy{}, resolver);

  REQUIRE(first);
  REQUIRE(second);
  CHECK(first->bytes() == "USD");
  CHECK(second->bytes() == "EUR");
  CHECK(calls == 2);
  CHECK(value.resourceKey() == "currency");
}

TEST_CASE("resolution uses explicit policy and preserves original validation errors") {
  using namespace portals;
  const auto ambiguous = text("\xc2\xa1");
  WidthPolicy wide;

  wide.ambiguous = AmbiguousWidth::WIDE;
  wide.revision = 42;

  const auto narrowResult = ambiguous.resolve(WidthPolicy{});
  const auto wideResult = ambiguous.resolve(wide);

  REQUIRE(narrowResult);
  REQUIRE(wideResult);
  CHECK(narrowResult->measure() == 1);
  CHECK(wideResult->measure() == 2);
  CHECK(wideResult->policy() == wide);

  const std::string invalid = "prefix" + std::string(1, static_cast<char>(0xff));
  const auto invalidValue = text(invalid);
  const auto original = GraphemeText::from(invalid, wide);
  const auto rejected = invalidValue.resolve(wide);
  const auto formatted = text("{}", invalid).resolve(wide);

  REQUIRE_FALSE(original);
  REQUIRE_FALSE(rejected);
  REQUIRE_FALSE(formatted);
  REQUIRE(std::holds_alternative<TextError>(rejected.error()));
  CHECK(std::get<TextError>(rejected.error()) == original.error());
  CHECK(std::get<TextError>(formatted.error()) == original.error());

  TextLimits limits;

  limits.max_input_bytes = 1;

  const auto limited = ambiguous.resolve(wide, limits);
  const auto originalLimit = GraphemeText::from("\xc2\xa1", wide, limits);

  REQUIRE_FALSE(limited);
  REQUIRE_FALSE(originalLimit);
  CHECK(std::get<TextError>(limited.error()) == originalLimit.error());
}

TEST_CASE("resources distinguish missing resolver missing key and failed callback") {
  using namespace portals;
  const auto resource = i18n("currency");
  const auto absentResolver = resource.resolve(WidthPolicy{});
  const auto missing =
    resource.resolve(WidthPolicy{}, [](std::string_view) -> std::expected<std::string, TextResourceError> {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    });
  const auto failed =
    resource.resolve(WidthPolicy{}, [](std::string_view) -> std::expected<std::string, TextResourceError> {
      return std::unexpected(TextResourceError::RESOLVER_FAILED);
    });
  const auto thrown =
    resource.resolve(WidthPolicy{}, [](std::string_view) -> std::expected<std::string, TextResourceError> {
      throw std::runtime_error("lookup failed");
    });

  REQUIRE_FALSE(absentResolver);
  REQUIRE_FALSE(missing);
  REQUIRE_FALSE(failed);
  REQUIRE_FALSE(thrown);
  CHECK(std::get<TextResourceError>(absentResolver.error()) == TextResourceError::RESOLVER_REQUIRED);
  CHECK(std::get<TextResourceError>(missing.error()) == TextResourceError::MISSING_RESOURCE);
  CHECK(std::get<TextResourceError>(failed.error()) == TextResourceError::RESOLVER_FAILED);
  CHECK(std::get<TextResourceError>(thrown.error()) == TextResourceError::RESOLVER_FAILED);
}

TEST_CASE("resolver content is validated unchanged and empty translation is success") {
  using namespace portals;
  const std::string invalid = "ok" + std::string(1, static_cast<char>(0xff));
  const auto rejected =
    i18n("invalid").resolve(WidthPolicy{}, [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      return invalid;
    });
  const auto original = GraphemeText::from(invalid);
  const auto empty =
    i18n("empty").resolve(WidthPolicy{}, [](std::string_view) -> std::expected<std::string, TextResourceError> {
      return std::string{};
    });

  REQUIRE_FALSE(rejected);
  REQUIRE_FALSE(original);
  CHECK(std::get<TextError>(rejected.error()) == original.error());
  REQUIRE(empty);
  CHECK(empty->bytes().empty());
}

TEST_CASE("literal resolution does not call resolver and move-only resolver is borrowed") {
  using namespace portals;
  int calls = 0;
  auto resolver = [owner = std::make_unique<std::string>("owned"),
                   &calls](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++calls;

    return *owner;
  };
  const auto literal = text("literal").resolve(WidthPolicy{}, resolver);

  REQUIRE(literal);
  CHECK(calls == 0);

  const auto resource = i18n("key").resolve(WidthPolicy{}, resolver);

  REQUIRE(resource);
  CHECK(resource->bytes() == "owned");
  CHECK(calls == 1);
}

TEST_CASE("allocation and custom formatter exceptions remain observable") {
  using namespace portals;
  const auto resource = i18n("key");
  const auto allocationFailure = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    throw std::bad_alloc{};
  };
  const auto lengthFailure = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    throw std::length_error("resource too large");
  };

  CHECK_THROWS_AS(static_cast<void>(resource.resolve(WidthPolicy{}, allocationFailure)), std::bad_alloc);
  CHECK_THROWS_AS(static_cast<void>(resource.resolve(WidthPolicy{}, lengthFailure)), std::length_error);
  CHECK_THROWS_AS(static_cast<void>(text("{}", ThrowingValue{})), std::runtime_error);
}

TEST_CASE("translated content uses receiving policy and limits") {
  using namespace portals;
  WidthPolicy wide;
  TextLimits limits;

  wide.ambiguous = AmbiguousWidth::WIDE;
  limits.max_cells = 1;

  const auto resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return "\xc2\xa1";
  };
  const auto value = i18n("symbol");
  const auto measured = value.resolve(wide, resolver);
  const auto limited = value.resolve(wide, resolver, limits);
  const auto original = GraphemeText::from("\xc2\xa1", wide, limits);

  REQUIRE(measured);
  CHECK(measured->policy() == wide);
  CHECK(measured->measure() == 2);
  REQUIRE_FALSE(limited);
  REQUIRE_FALSE(original);
  CHECK(std::get<TextError>(limited.error()) == original.error());
}
