#!/usr/bin/env bash
# Install DankC build + dev dependencies. Detects the distro and runs the right
# package manager. Safe to re-run. See docs/05-PORTABILITY.md for the full matrix.
set -euo pipefail

id=""
[ -r /etc/os-release ] && id="$(. /etc/os-release; echo "${ID:-}")"

echo "Detected distro: ${id:-unknown}"

case "$id" in
arch | cachyos | endeavouros | manjaro | artix)
    sudo pacman -S --needed base-devel meson ninja pkgconf wayland wayland-protocols \
        libxkbcommon mesa libsystemd freetype2 fontconfig harfbuzz pango pipewire pam \
        curl sqlite cjson grim
    ;;
fedora | nobara | ultramarine)
    sudo dnf install -y gcc gcc-c++ meson ninja-build pkgconf wayland-devel \
        wayland-protocols-devel libxkbcommon-devel mesa-libEGL-devel mesa-libGLES-devel \
        systemd-devel freetype-devel fontconfig-devel harfbuzz-devel pango-devel \
        pipewire-devel pam-devel libcurl-devel sqlite-devel cjson-devel grim
    ;;
debian | ubuntu | pop | linuxmint)
    sudo apt-get update
    sudo apt-get install -y build-essential meson ninja-build pkgconf libwayland-dev \
        libwayland-bin wayland-protocols libxkbcommon-dev libegl-dev libgles-dev \
        libsystemd-dev libfreetype-dev libfontconfig-dev libharfbuzz-dev libpango1.0-dev \
        libpipewire-0.3-dev libpam0g-dev libcurl4-openssl-dev libsqlite3-dev libcjson-dev grim
    ;;
opensuse* | suse)
    sudo zypper install -y gcc gcc-c++ meson ninja pkgconf wayland-devel \
        wayland-protocols-devel libxkbcommon-devel Mesa-libEGL-devel Mesa-libGLESv2-devel \
        systemd-devel freetype2-devel fontconfig-devel harfbuzz-devel pango-devel \
        pipewire-devel pam-devel libcurl-devel sqlite3-devel cjson-devel grim
    ;;
*)
    echo "Unrecognised distro. Install the equivalents of:"
    echo "  meson ninja pkgconf wayland wayland-protocols libxkbcommon mesa(EGL/GLES)"
    echo "  libsystemd freetype fontconfig harfbuzz pango pipewire pam curl sqlite cjson grim"
    exit 1
    ;;
esac

echo "Done. Now: make   (or: meson setup build && ninja -C build)"
