#!/bin/sh
# SPDX-FileCopyrightText: 2026 Georgios Kouros
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${OKULAIR_BUILD_DIR:-"$repo_dir/build"}
if [ ! -x "$build_dir/bin/okular" ]; then
    echo "No development build found at $build_dir/bin/okular. See docs/development.md." >&2
    exit 1
fi
export QT_PLUGIN_PATH="$build_dir/bin${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
# Upstream application IDs are still shared. A separate session prevents handing
# the PDF to an already running system Okular instead of this development build.
if command -v dbus-run-session >/dev/null 2>&1; then
    exec dbus-run-session -- "$build_dir/bin/okular" "$@"
fi
echo "dbus-run-session is unavailable. Close other Okular instances first." >&2
exec "$build_dir/bin/okular" "$@"
