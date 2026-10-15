#!/bin/sh
# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
if [ "${1-}" = "--help" ]; then
  cat <<'PORTALS_HELP'
Usage: run.sh [EXAMPLE [ARG...]]
       run.sh --help

Run an installed example from bin relative to this extracted bundle launcher.
EXAMPLE: dashboard (default), gallery, custom_widget, editor_history, click_handlers.
ARG... are forwarded unchanged. No environment variables configure this launcher;
the example inherits the caller's environment. Requires a POSIX shell and the
matching Linux executable in the bundle. This source is copied to the bundle root.
Starts an interactive example; does not build, download or publish anything.
Exit: the example's exit code; 2 for an unknown example; shell exec errors normally
126 (not executable) or 127 (not found). --help prints this text and exits 0 before
resolving the bundle directory or launching any process.
PORTALS_HELP
  exit 0
fi
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
