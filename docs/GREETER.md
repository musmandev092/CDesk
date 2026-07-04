# dankc as a greetd greeter — install / enable / rollback

This documents `scripts/dankc-greeter` (docs/28-GREETER-PLAN.md T5): the
wrapper script that lets [greetd](https://git.sr.ht/~kennylevinsen/greetd)
launch `dankc greeter` (the login UI, T1-T4) as the graphical login screen,
on niri (dankc is niri-only — no other compositor is supported).

## READ THIS FIRST: how you can get locked out, and how not to

`/etc/greetd/config.toml` controls what runs on your login VT. If the
command it points at fails to start (missing binary, bad permissions, a
crash before the UI comes up), greetd will keep retrying it and you will
have **no graphical way to log in** until you fix it from elsewhere.

Before changing your real `/etc/greetd/config.toml`:

1. **Test in a VM first**, or on a spare machine, if at all possible.
2. **Keep another way in**: either
   - a different, already-working display manager/greeter you can switch
     back to (don't uninstall it — just don't point greetd at dankc yet
     if you're testing something else), or
   - a **root (or sudo-capable) TTY already logged in** on another VT
     (e.g. Ctrl+Alt+F3) *before* you restart/reload greetd, so you can
     edit the config back and restart the service without a second
     machine.
3. Only after `dankc greeter` itself has been verified to run (via
   `fakegreet`/a mock socket, or `DANKC_GREETER_DEMO=1` on your desktop —
   see T6 and `src/ui/greeter_main.c`) should you point real greetd at it.
4. **Real-VT verification (actually switching your login VT over to dankc
   and logging in through it) is intentionally left to you** — it needs a
   reboot/VT switch this environment can't safely automate. Everything
   below gets you to "greetd is configured and ready to try"; the actual
   VT switch and login attempt should be done by hand, with the
   precautions above in place.

If you're not sure you can recover from a bad greetd config on this
machine, don't do this outside a VM.

## What `dankc-greeter` does

`scripts/dankc-greeter` is the command greetd's `[default_session]` in
`/etc/greetd/config.toml` should invoke (see
`packaging/greetd/config.toml.example`). It runs as the unprivileged
`greeter` system user and:

1. Fakes a writable `HOME` and `XDG_STATE_HOME`/`XDG_DATA_HOME`/
   `XDG_CACHE_HOME` under a cache directory (default
   `/var/cache/dankc-greeter`, override with `--cache-dir`), and exports
   `DANKC_GREETER_STATE_DIR` to the same path — this is where
   `src/services/greeter_data.c` persists last-user/last-session memory
   between login attempts.
2. Falls back to its own `$CACHE_DIR/run` for `$XDG_RUNTIME_DIR` if none
   is already set (systems without `pam_rundir`/logind providing one,
   e.g. seatd-only setups) — logind-managed systems keep whatever they
   were handed.
3. Generates a small, throwaway niri config in a temp file (hotkey
   overlay + hot corners off, solid black background so a crash before
   first frame doesn't flash the previous VT), optionally including
   `/etc/greetd/niri-overrides.kdl` if present (for local input/output
   tweaks), and appends `spawn-at-startup` for
   `sh -c "dankc greeter; niri msg action quit --skip-confirmation"`.
4. `exec`s `niri -c <temp config>` — dankc's greeter runs inside that
   niri instance; when `dankc greeter` exits (successful login handoff,
   user cancel, or a crash), niri quits and greetd decides what happens
   next (respawn the greeter, or start the authenticated session
   greetd itself launched).

`niri` must be in `PATH` as seen by the `greeter` user (a normal package
install to `/usr/bin` covers this).

Run `dankc-greeter --help` for its flags; `--dry-run` generates the temp
niri config, prints its path, and exits without touching `$CACHE_DIR` or
execing niri — useful for inspecting/validating the generated config
(e.g. with `niri validate -c <path>`) without needing a real greetd/VT.

## Install

1. **Install greetd itself** from your distro (not part of this repo),
   e.g. on Arch: `pacman -S greetd`. Do **not** enable/start its service
   yet.
2. **Install dankc** (the `dankc` binary must be in `PATH`, e.g.
   `/usr/bin/dankc` via `meson install` or the Arch package — see
   `packaging/README.md`).
3. **Install the wrapper script**:
   ```sh
   meson install -C build   # installs scripts/dankc-greeter to $bindir (e.g. /usr/bin)
   ```
   or by hand:
   ```sh
   install -Dm755 scripts/dankc-greeter /usr/bin/dankc-greeter
   ```
4. **Create the `greeter` system user** (skip if your greetd package
   already made one — check first with `getent passwd greeter`):
   ```sh
   systemd-sysusers packaging/systemd/sysusers-dankc-greeter.conf
   ```
   or install the fragment to `/etc/sysusers.d/dankc-greeter.conf` first
   if you want it applied automatically on future boots too.
5. **Create the cache directory** (`/var/cache/dankc-greeter`, owned by
   `greeter:greeter`, used as fake `HOME`/XDG dirs — see above):
   ```sh
   systemd-tmpfiles --create packaging/systemd/tmpfiles-dankc-greeter.conf
   ```
   or install the fragment to `/etc/tmpfiles.d/dankc-greeter.conf` for it
   to be recreated automatically (e.g. after `/var/cache` is cleared).
6. **Point greetd at dankc**: copy
   `packaging/greetd/config.toml.example` to `/etc/greetd/config.toml`
   — after backing up whatever is there first:
   ```sh
   cp /etc/greetd/config.toml /etc/greetd/config.toml.bak
   cp packaging/greetd/config.toml.example /etc/greetd/config.toml
   ```
   Adjust `[terminal] vt =` to match the VT greetd should own on your
   system if it differs from the example.

None of the steps above are performed automatically by any tooling in
this repo — they're manual, deliberate steps you run yourself, in order,
after verifying the precautions in the section above.

## Enable

Only after you've re-read the warnings above and have a rollback path
ready:

```sh
systemctl enable --now greetd.service
```

If you're switching from an existing display manager (gdm/sddm/lightdm/
etc.), disable it first (`systemctl disable --now <old-dm>.service`) —
having two display managers fight over the same VT is its own way to get
stuck.

## Rollback

If dankc's greeter doesn't come up, or you just want to go back:

```sh
systemctl stop greetd.service
cp /etc/greetd/config.toml.bak /etc/greetd/config.toml   # restore your old config
systemctl disable greetd.service                          # if you're going back to another DM
systemctl enable --now <old-dm>.service                   # re-enable it
```

All of this can be done from the root TTY you kept open per the warnings
above, without needing a second machine.

## Testing without touching a real greeter

Before ever pointing real greetd at this:

- `DANKC_GREETER_DEMO=1 dankc greeter` runs the UI on your normal desktop
  with a fake auth flow — no `$GREETD_SOCK`, no privilege, no VT switch
  (see `src/ui/greeter_main.c`).
- `fakegreet` (from the `greetd` project) or a small mock socket server
  can exercise the real greetd JSON IPC (`src/services/greetd.c`) end to
  end without touching `/etc/greetd` or any system session.
- `dankc-greeter --dry-run` (see above) checks the wrapper's own config
  generation without execing niri or requiring root.

See docs/28-GREETER-PLAN.md T6 for the intended verification path.
