# dankc Greeter (greetd login) — Plan (2026-07-04)

BIGGEST Group-1 item (~1800-2500 LOC). NOT just lock+different auth: new greetd JSON IPC client
(length-prefixed native-u32 + JSON over $GREETD_SOCK), fullscreen UI (user picker + password + session
selector), reduced init mode, system integration (wrapper script + /etc/greetd/config.toml + greeter
user). REALISTIC: v1 code + fakegreet-verified TODAY; real-VT login = deferred follow-up (root/reboot).

## Ground truth: lock.c (per-output surface, scale/EGL/render, clock/date/password-pill draw, key handling
— REUSE visuals; but greeter uses fullscreen OVERLAY layer-shell surface like powermenu.c L455-560, NOT
ext_session_lock). auth.c = PAM (lock); greeter uses greetd protocol NOT PAM. main.c has subcommand
dispatch ~L1071 (ctl/keybinds exit early) → add `greeter`. loop.h dc_loop_add_fd for the greetd socket.
cJSON vendored. Single binary + `dankc greeter` subcommand (not 2nd binary). Wrapper exec niri -c $TMP
with spawn-at-startup "dankc greeter".

## greetd wire proto (proto.md, stable): unix stream $GREETD_SOCK, each msg = native-u32 len + JSON.
Requests create_session{username} → post_auth_message_response{response|null} → start_session{cmd,env} /
cancel_session. Responses success / error{error_type,description} / auth_message{auth_message_type
visible|secret|info|error, auth_message}. Flow: create → N auth round-trips (usually 1 secret) → success
→ start_session → success → EXIT greeter (greetd waits for process death then starts session).
User enum: getpwent uid∈[1000,60000) shell not nologin/false, name≠nobody, dir≠/var/empty. Session enum:
scan XDG_DATA_DIRS else /usr+/usr/local/share /{wayland,x}-sessions/*.desktop → Name/Exec/DesktopNames,
strip %-codes (apps.c pattern). Last-user/session memory in DANKC_GREETER_STATE_DIR (cache dir).

## Tasks (Sonnet each): T1 services/greetd.c/.h — IPC client + state machine (connect $GREETD_SOCK
O_NONBLOCK + dc_loop_add_fd; create_session/respond/start_session/cancel; event cb SUCCESS/AUTH_MESSAGE/
ERROR; u32-frame tx queue + rx accumulator handling partial frames; DANKC_GREETD_SOCK_PATH override for
mock). NO build-file edits (T5). NEW files, no deps. T2 services/greeter_data.c/.h — user+session enum +
last-user/session memory. NEW files no deps. T3 ui/greeter.c/.h — per-output fullscreen overlay surface
(powermenu pattern + lock.c scale/EGL) + widgets (user list, password pill w/ dots+error+status line for
greetd info/error msgs, session cycle) + input + state machine IDLE→CREATING→PROMPT→VALIDATING→STARTING→
DONE via dc_greetd_* callbacks; done_cb stops loop on success. Needs T1/T2 headers (agree sigs upfront →
parallel). T4 ui/greeter_main.c + main.c dispatch — reduced init (log/theme/config/wayland/egl/render/
loop/greeter/greetd, NO dbus/tray/notif/niri-ipc/polkit); DANKC_GREETER_DEMO=1 fakes prompt for on-desktop
visual test; _exit(0) on success. main.c: `if argv[1]=="greeter" return dc_greeter_main`. T5 (SERIALIZE
build files) scripts/dankc-greeter wrapper (niri-only, port DMS dms-greeter: fake HOME/XDG at cache dir,
temp niri config black bg + spawn "sh -c 'dankc greeter; niri msg action quit'", exec niri -c $TMP) +
meson.build+Makefile source adds + packaging/greetd config.toml.example + sysusers/tmpfiles + docs/GREETER.md.
T6 verify via `fakegreet 'dankc greeter'` (or python mock socket) + DANKC_GREETER_DEMO visual; real-VT deferred.

## Risks: LOCK-OUT (never flip real config.toml in a task; test fakegreet/mock/demo only; docs mandate
VM/spare-VT + root TTY open; wrapper exec niri so greeter crash still quits→greetd respawns). proto edge
cases (multi auth_message, visible prompt, error-during-start → treat any error as cancel+return-to-
password, never deadlock). fake-HOME must not crash dc_config_load on unwritable dirs. reduced init must
truly not touch dbus. T3 is ~700 LOC (big). v1 = single-user-ish picker + password + default session.
