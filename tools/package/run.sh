#!/bin/sh
# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
set -eu
bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
example=${1:-dashboard}
if [ "$#" -gt 0 ]; then shift; fi
case "$example" in
  dashboard) program=portals_dashboard ;;
  gallery) program=portals_gallery ;;
  custom_widget) program=portals_custom_widget ;;
  editor_history) program=portals_editor_history ;;
  click_handlers) program=portals_click_handlers ;;
  *) printf 'Unknown example: %s\n' "$example" >&2; exit 2 ;;
esac
exec "$bundle_dir/bin/$program" "$@"
