// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "compactness.hpp"

#include <exception>
#include <iostream>

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  try {
    std::vector<std::string> utf8Arguments;
    std::vector<std::string_view> arguments;

    utf8Arguments.reserve(static_cast<std::size_t>(argc));

    for (int index = 1; index < argc; ++index) {
#ifdef _WIN32
      const auto bytes = std::filesystem::path(argv[index]).u8string();

      utf8Arguments.emplace_back(reinterpret_cast<const char *>(bytes.data()), bytes.size());
#else
      utf8Arguments.emplace_back(argv[index]);
#endif
    }

    for (const auto &argument : utf8Arguments) {
      arguments.emplace_back(argument);
    }

    const auto options = portals::compactness::parseArguments(arguments);

    if (options.help) {
      std::cout << portals::compactness::helpText();

      return 0;
    }

    const auto before = portals::compactness::measureFiles(options.before);
    const auto after = portals::compactness::measureFiles(options.after);

    std::cout << portals::compactness::renderReport(before, after, options.format);

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "compactness: " << error.what() << '\n';

    return 1;
  }
}
