#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Georgios Kouros
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix="${HOME}/.local/opt/okulair"
build_dir="${repo_dir}/build-install"
jobs=4
install_deps=false

usage() {
    cat <<'HELP'
Usage: install-okulair.sh [OPTIONS]

Build and install the desktop application with AI and PDF support.

  --install-deps     Install Ubuntu/Debian build dependencies using apt (sudo)
  --prefix PATH      Installation directory (default: ~/.local/opt/okulair)
  --build-dir PATH   Build directory (default: <checkout>/build-install)
  --jobs N          Parallel build jobs (default: 4)
  -h, --help        Show this help

Requires CMake 3.22+, Qt 6.8+, KDE Frameworks 6.13+, and Poppler 25.03+.
The selected distribution must provide sufficiently recent dependencies.
HELP
}

while (($#)); do
    case "$1" in
        --install-deps) install_deps=true; shift ;;
        --prefix|--build-dir|--jobs)
            if (($# < 2)) || [[ -z "$2" || "$2" == --* ]]; then
                echo "Missing value for $1" >&2
                exit 2
            fi
            case "$1" in
                --prefix) prefix=$2 ;;
                --build-dir) build_dir=$2 ;;
                --jobs) jobs=$2 ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo '--jobs must be a positive integer' >&2
    exit 2
fi
# Resolve relative paths before CMake interprets them against the build directory.
prefix=$(realpath -m -- "$prefix")
build_dir=$(realpath -m -- "$build_dir")

if "$install_deps"; then
    command -v apt-get >/dev/null || { echo 'Dependency installation requires apt-get.' >&2; exit 1; }
    privilege=()
    if ((EUID != 0)); then
        command -v sudo >/dev/null || { echo 'Dependency installation requires sudo.' >&2; exit 1; }
        privilege=(sudo)
    fi
    "${privilege[@]}" apt-get update
    "${privilege[@]}" apt-get install --no-install-recommends \
        build-essential cmake pkg-config gettext dbus-daemon \
        extra-cmake-modules qt6-base-dev qt6-base-private-dev libqt6svg6-dev \
        libkf6archive-dev libkf6bookmarks-dev libkf6completion-dev \
        libkf6config-dev libkf6configwidgets-dev libkf6coreaddons-dev \
        libkf6crash-dev libkf6i18n-dev libkf6kio-dev libkf6threadweaver-dev \
        libkf6windowsystem-dev libkf6xmlgui-dev libkf6iconthemes-dev \
        libkf6parts-dev libkf6colorscheme-dev libkf6textwidgets-dev \
        libkf6widgetsaddons-dev libpoppler-qt6-dev libssl-dev
fi

command -v cmake >/dev/null || { echo 'CMake is missing. Install dependencies first (see --help).' >&2; exit 1; }
cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_TESTING=OFF -DOKULAR_UI=desktop -DOKULAIR_AI=ON \
    '-DFORCE_NOT_REQUIRED_DEPENDENCIES=KF6Wallet;KF6DocTools;Qt6Qml;TIFF;LibSpectre;KExiv2Qt6;DjVuLibre;EPub;Discount' \
    -DCMAKE_DISABLE_FIND_PACKAGE_Qt6TextToSpeech=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_KF6Purpose=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_KF6DocTools=ON
cmake --build "$build_dir" --parallel "$jobs"
cmake --install "$build_dir"

# Keep the generated runtime environment with the installation, independent of
# the checkout. It supplies KDE data directories and Qt plugin search paths.
install -m 644 "$build_dir/prefix.sh" "$prefix/okulair-env.sh"
{
    printf '#!/usr/bin/env bash\nset -eo pipefail\n'
    printf 'source %q\n' "$prefix/okulair-env.sh"
    printf 'if command -v dbus-run-session >/dev/null 2>&1; then\n'
    printf '    exec dbus-run-session -- %q "$@"\n' "$prefix/bin/okular"
    printf 'fi\n'
    printf 'exec %q "$@"\n' "$prefix/bin/okular"
} > "$prefix/bin/okulair"
chmod +x "$prefix/bin/okulair"
printf '\nInstalled OkulAIr. Launch with:\n  %q /path/to/paper.pdf\n' "$prefix/bin/okulair"
