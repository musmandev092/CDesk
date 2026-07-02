project(
  'dankc',
  ['c', 'cpp'],
  version: '0.0.1',
  default_options: [
    'c_std=c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE', language: 'c')

cc = meson.get_compiler('c')

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
# Protocol XML is vendored under protocol/; only wayland-scanner is needed.

deps = [wayland_client, wayland_egl, egl, glesv2, xkbcommon, libsystemd, pam, m_dep, fontconfig]

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
  'src/render/nvg.c',
  'src/ui/bar/bar.c',
  'src/ui/popout.c', 'src/ui/hover.c',
  'src/ui/controlcenter.c', 'src/ui/battery_popout.c', 'src/ui/osd.c', 'src/ui/toasts.c',
  'src/ui/launcher.c', 'src/ui/notifcenter.c', 'src/ui/clip_picker.c', 'src/ui/settings.c',
  'src/ui/dashboard.c',
  'src/ui/lock.c', 'src/ui/processes.c',
  'src/services/battery.c', 'src/services/audio.c', 'src/services/net.c',
  'src/services/dbus.c', 'src/services/bluez.c', 'src/services/mpris.c',
  'src/services/icons.c', 'src/services/apps.c', 'src/services/notifications.c',
  'src/services/clipboard.c', 'src/services/tray.c', 'src/services/auth.c', 'src/services/logind.c',
  'src/services/weather.c', 'src/services/sysmon.c',
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

install_data('assets/fonts/InterVariable.ttf',
             'assets/fonts/MaterialSymbolsRounded.ttf',
             'assets/fonts/NotoEmoji-Regular.ttf',
             install_dir: get_option('datadir') / 'dankc/fonts')
