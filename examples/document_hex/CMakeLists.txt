# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
add_executable(portals_document_hex_example main.cpp)

target_link_libraries(portals_document_hex_example PRIVATE portals::core)
portals_enable_warnings(portals_document_hex_example)
