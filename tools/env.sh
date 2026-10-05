#!/usr/bin/env bash
# Source this to get an ESP-IDF shell for THIS project only, without touching
# any other ESP-IDF install on the system:
#
#   source tools/env.sh
#
# It reuses the existing ESP-IDF repo at ~/devl/esp-idf (read-only) but keeps
# all downloaded tools (compiler, cmake, python venv) in ./.espressif inside
# this project, so nothing system-wide (~/.espressif) is modified.

export IDF_TOOLS_PATH="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)/.espressif"
export IDF_PATH="$HOME/devl/esp-idf"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
