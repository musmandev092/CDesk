project(
  'dankc',
  ['c', 'cpp'],
  version: '0.0.1',
  default_options: [
    'c_std=c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
    # LTO across all buildtypes: lets gcc/clang inline across translation
    # units at link time, which matters on the hot text-shaping
    # (render/shape.c) and nanovg-tessellation paths that cross .c file
    # boundaries.
    #
    # For the shipped/packaged build (see packaging/PKGBUILD) don't use
    # `--buildtype=release`: it implies -O3, and -O3's more aggressive
    # inlining combined with b_lto=true trips false-positive
    # -Wstringop-overflow/-Wstringop-overread warnings (both in vendored
    # third_party/nanovg's bundled stb_image.h and in src/services/apps.c —
    # a known GCC diagnostic issue at higher optimization, not real bugs).
    # packaging/PKGBUILD instead passes -Doptimization=2 -Ddebug=false
    # -Db_ndebug=true explicitly, which still drops -g and adds -DNDEBUG but
    # keeps -O2 (matching the Makefile's release profile) and stays
    # warning-clean.
    'b_lto=true',
  ],
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Portable perf/size flags: only add if the compiler actually supports them
# (keeps this meson.build working on clang / older gcc / non-x86 without
# hardcoding a compiler-specific flag list). -fno-plt avoids the PLT
# indirection on every D-Bus/GL call site; -fvisibility=hidden shrinks the
# dynamic symbol table and lets LTO reason more aggressively about what's
# actually reachable from outside the binary — safe here since dankc is an
# executable, not a shared library.
foreach flag : ['-fno-plt', '-fvisibility=hidden']
  if cc.has_argument(flag)
    add_project_arguments(flag, language: 'c')
  endif
  if cpp.has_argument(flag)
    add_project_arguments(flag, language: 'cpp')
  endif
endforeach

# --- dependencies ---------------------------------------------------------
wayland_client = dependency('wayland-client')
wayland_egl    = dependency('wayland-egl')
egl            = dependency('egl')
glesv2         = dependency('glesv2')
xkbcommon      = dependency('xkbcommon')
libsystemd     = dependency('libsystemd')          # sd-bus (swap for elogind/basu on non-systemd distros)
pam            = cc.find_library('pam')             # lock-screen authentication
m_dep          = cc.find_library('m', required: false)
fontconfig     = dependency('fontconfig')          # non-Latin fallback font lookup (render/nvg.c)
harfbuzz       = dependency('harfbuzz')            # complex text shaping, Arabic/Urdu joining (render/shape.c)
# FriBidi (real UAX#9 BiDi reordering) is optional: if absent, render/shape.c
# falls back to its documented "UBA-lite" approximation (contiguous RTL
# segments reversed, numbers kept LTR) instead of failing the build.
fribidi        = dependency('fribidi', required: false)
# Protocol XML is vendored under protocol/; only wayland-scanner is needed.

deps = [wayland_client, wayland_egl, egl, glesv2, xkbcommon, libsystemd, pam, m_dep, fontconfig, harfbuzz]
if fribidi.found()
  deps += fribidi
  add_project_arguments('-DDC_HAVE_FRIBIDI=1', language: 'c')
endif

# --- wayland protocol generation -----------------------------------------
wayland_scanner = find_program('wayland-scanner')
scanner_code = generator(wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
scanner_client = generator(wayland_scanner,
  output: '@BASENAME@-client-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'])

protocols = [
  'protocol/wlr-layer-shell-unstable-v1.xml',
  'protocol/wlr-data-control-unstable-v1.xml',
  'protocol/ext-session-lock-v1.xml',
  'protocol/xdg-shell.xml',
  'protocol/viewporter.xml',
  'protocol/fractional-scale-v1.xml',
  'protocol/cursor-shape-v1.xml',
]

proto_src = []
foreach xml : protocols
  proto_src += scanner_code.process(xml)
  proto_src += scanner_client.process(xml)
endforeach

# --- vendored third-party (relaxed warnings) -----------------------------
thirdparty = static_library('dankc-thirdparty',
  [
    'third_party/nanovg/nanovg.c',
    'third_party/nanovg/nanovg_gl_impl.c',
    'third_party/nanosvg/nanosvg_impl.c',
    'third_party/cjson/cJSON.c',
  ],
  include_directories: include_directories(
    'third_party/nanovg', 'third_party/nanosvg', 'third_party/cjson'),
  c_args: ['-w'],
)

# --- first-party sources --------------------------------------------------
src = files(
  'src/main.c',
  'src/core/log.c', 'src/core/loop.c', 'src/core/config.c', 'src/core/dms_import.c',
  'src/core/anim.c',
  'src/wayland/wl.c', 'src/wayland/egl.c',
  'src/render/nvg.c', 'src/render/shape.c',
  'src/ui/bar/bar.c',
  'src/ui/popout.c', 'src/ui/hover.c',
  'src/ui/controlcenter.c', 'src/ui/battery_popout.c', 'src/ui/osd.c', 'src/ui/toasts.c',
  'src/ui/launcher.c', 'src/ui/notifcenter.c', 'src/ui/clip_picker.c', 'src/ui/settings.c',
  'src/ui/dashboard.c',
  'src/ui/lock.c', 'src/ui/processes.c', 'src/ui/powermenu.c', 'src/ui/dock.c',
  'src/ui/frame.c', 'src/ui/material_bg.c', 'src/ui/tray_menu.c', 'src/ui/notif_image.c',
  'src/ui/keybinds_modal.c', 'src/ui/polkit_modal.c', 'src/ui/text_edit.c',
  'src/services/calc.c', 'src/services/history.c', 'src/services/notepad_storage.c',
  'src/services/battery.c', 'src/services/audio.c', 'src/services/net.c',
  'src/services/dbus.c', 'src/services/bluez.c', 'src/services/mpris.c',
  'src/services/icons.c', 'src/services/apps.c', 'src/services/notifications.c',
  'src/services/clipboard.c', 'src/services/tray.c', 'src/services/auth.c', 'src/services/logind.c',
  'src/services/weather.c', 'src/services/sysmon.c', 'src/services/power.c', 'src/services/printers.c',
  'src/services/display.c',
  'src/services/autostart.c',
  'src/services/sound.c', 'src/services/polkit.c',
  'src/services/nightlight.c', 'src/services/firewall.c',
  'src/services/niri_input.c', 'src/services/timedate.c',
  'src/services/systheme.c', 'src/services/systheme_term.c', 'src/services/systheme_apps.c',
  'src/services/systheme_launchers.c',
  'src/services/systheme_term2.c',
  'src/services/systheme_notify.c',
  'src/services/systheme_qtkde.c',
  'src/services/systheme_editors.c',
  'src/services/systheme_browser.c',
  'src/niri/niri.c',
  'src/theme/theme.c',
  'src/ipc/control.c',
)

# The single C++ translation unit (Material colour math).
cpp_src = files('src/theme/dynamic.cpp')

inc = include_directories(
  'src', 'third_party/nanovg', 'third_party/nanosvg', 'third_party/cjson')

executable('dankc',
  src, cpp_src, proto_src,
  include_directories: inc,
  dependencies: deps,
  link_with: thirdparty,
  install: true,
)

# docs/POLISH.md P7 item 3: install the pyftsubset'd icon font (~248KB,
# covers exactly render/icons.h's DC_ICON_* codepoints), not the 14.5MB full
# variable font it's generated from — that one stays repo-only as the
# regeneration source for scripts/subset-fonts.sh, see render/nvg.c's
# ICON_FONT_CANDIDATES comment.
install_data('assets/fonts/InterVariable.ttf',
             'assets/fonts/MaterialSymbolsRounded.subset.ttf',
             'assets/fonts/NotoEmoji-Regular.ttf',
             install_dir: get_option('datadir') / 'dankc/fonts')

# docs/14-COMPLETION-PLAN.md W1.3: vendored freedesktop notification sounds,
# used only when no system sound-theme provides them (services/sound.c
# resolve_sound_file() tries /usr/share/sounds/freedesktop first).
install_data('assets/sounds/freedesktop/message.oga',
             'assets/sounds/freedesktop/message-new-instant.oga',
             install_dir: get_option('datadir') / 'dankc/sounds/freedesktop')
