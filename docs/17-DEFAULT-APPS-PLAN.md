# DankC — Default Applications Manager Plan

Research + implementation plan for expanding `src/ui/settings.c`'s `TAB_DEFAULT_APPS` from its
current 3 categories (Web Browser, File Manager, Terminal) to a full ~18-20 category default-apps
manager, matching GNOME/KDE's professional pickers and DMS's own (QML) `DefaultAppsTab.qml`.

**Status**: design doc only. No code changed — `src/ui/settings.c` is being edited by another
agent concurrently; this doc is the handoff for whoever implements it next.

---

## 1. The mechanism (researched + verified against this machine)

### 1.1 mimeapps.list — the freedesktop MIME Applications Associations spec

Spec: [mime-apps-spec](https://specifications.freedesktop.org/mime-apps-spec/latest/) (freedesktop.org
xdg-specs). Ground-truthed here against the actual reference implementation installed on this box,
`/usr/bin/xdg-mime` (xdg-utils, POSIX shell) and `/usr/bin/xdg-settings`, since the live spec page is
behind an anti-bot wall (Anubis) that blocked automated fetches during this research pass.

A `mimeapps.list` file (INI format) has up to three groups:

- **`[Default Applications]`** — `mimetype=desktopfile1.desktop;desktopfile2.desktop;...` — the
  user/admin's explicit default(s) for a MIME type, most-preferred first. This is the group both
  `xdg-mime query default` and `xdg-mime default` read/write.
- **`[Added Associations]`** — same `mimetype=id.desktop;...` shape, but *additive*: apps offered as
  candidates ("Open With") for a type without becoming the sole default. Not needed for a default-apps
  picker — it's for building "Open With" menus.
- **`[Removed Associations]`** — blacklists an id.desktop from showing up as a candidate for a mimetype,
  inherited from a lower-precedence file (used mainly to override a distro default).

**Search order** (confirmed by reading `check_mimeapps_list()`/`defapp_generic()` in `/usr/bin/xdg-mime`,
lines ~1121-1199): for each directory in
`$XDG_CONFIG_HOME` → `$XDG_CONFIG_DIRS` (default `/etc/xdg`) → `$XDG_DATA_HOME/applications` →
`$XDG_DATA_DIRS/applications` (default `/usr/local/share:/usr/share`), it first checks a
**desktop-environment-prefixed** file — `${XDG_CURRENT_DESKTOP,,}-mimeapps.list` (e.g.
`gnome-mimeapps.list`, `kde-mimeapps.list`) — then the plain `mimeapps.list`. First match wins; if the
recorded `.desktop` id doesn't resolve to an installed, runnable app, it's skipped and the search
continues (so a stale entry doesn't "stick"). If nothing in any `mimeapps.list` matches, `xdg-mime`
falls back to scanning `applications/*.desktop` for `MimeType=` matches and picking the one with the
highest `InitialPreference=` (`defapp_fallback()`).

**Writes**: `xdg-mime default app.desktop mime1 [mime2 ...]` always writes to the *generic*
`$XDG_CONFIG_HOME/mimeapps.list` (`make_default_generic()`, confirmed at line 1007) unless a KDE or
LXQt runtime is detected (`make_default_kde`/`make_default_lxqt`, which shell out to that DE's own
tool). It's an `awk` script that inserts/replaces one `mimetype=app.desktop` line per MIME type passed,
under `[Default Applications]`, creating that section if absent. Passing several MIME types in one
invocation sets all of them atomically in one file rewrite — this is why a category picker should batch
all of a category's MIME types into a single `xdg-mime default` call, not one call per MIME type.

**Gotcha for dankc**: a *system* `mimeapps.list` (e.g. shipped by a distro at
`/usr/share/applications/mimeapps.list` or `/etc/xdg/mimeapps.list`) sits **behind**
`$XDG_CONFIG_HOME` in precedence, so a user write always wins — but a desktop-prefixed file
(`gnome-mimeapps.list`) in `$XDG_CONFIG_HOME` would be checked *before* the plain one, so if some other
tool (e.g. a GNOME session) ever writes that prefixed variant, dankc's plain-file write could silently
appear to have no effect. Worth a one-line note in the UI hint but not worth chasing further.

Live example from this machine's real `~/.config/mimeapps.list` (already xdg-mime-managed state,
confirms the shape above):
```ini
[Default Applications]
x-scheme-handler/claude-cli=claude-code-url-handler.desktop
text/html=brave-origin.desktop
x-scheme-handler/http=brave-origin.desktop
x-scheme-handler/https=brave-origin.desktop
x-scheme-handler/about=brave-origin.desktop
x-scheme-handler/unknown=brave-origin.desktop
```

### 1.2 Desktop-file IDs

A desktop-file id is the `.desktop` file's basename relative to its `applications/` directory, with
`/` replaced by `-` for subdirectories (e.g. `kde/foo.desktop` → `kde-foo.desktop`). dankc's `apps.c`
already stores ids without the `.desktop` suffix (`dc_app.id`); settings.c's existing
`strip_desktop_suffix()`/re-append pattern (see `default_app_role()`, settings.c:1509-1556) is correct
and should be reused unchanged for every new category.

### 1.3 Web Browser is *not* a plain MIME default — `xdg-settings`

Confirmed by reading `/usr/bin/xdg-settings` (`set_browser_gnome3()`, lines 998-1008 — this is the
generic/GNOME3-style path dankc's `xdg-settings set default-web-browser` call already exercises):
setting the default browser actually calls the *same* `xdg-mime default` machinery underneath, but for
**four** MIME/scheme types in one go: `x-scheme-handler/http`, `x-scheme-handler/https`,
`x-scheme-handler/about`, `x-scheme-handler/unknown` (`get_browser_gnome3()` reads back via
`x-scheme-handler/http` alone). `text/html` is a fifth association browsers commonly also declare, set
separately by browsers' own `.desktop` `MimeType=` line but not by `xdg-settings` itself. dankc's
current code already calls the real `xdg-settings set default-web-browser` binary rather than
reimplementing this, which is the right call — keep doing that; don't replace it with a raw
`xdg-mime default` for the browser category.

### 1.4 Terminal is not MIME-based at all — `xdg-terminal-exec` / `xdg-terminals.list`

A terminal doesn't *open a file type*, so it has no MIME identity and is deliberately outside
`xdg-mime`. The proposal spec is
[xdg-terminal-exec](https://github.com/Vladimir-csp/xdg-terminal-exec) (still a freedesktop.org
terminal-wg proposal, not yet a ratified spec, but the de-facto mechanism — GNOME/KDE/DMS/dankc all
already follow it). Candidate terminals are `.desktop` files that declare
`Categories=...;TerminalEmulator;...` and an `X-TerminalArgExec=` key; the user's preference is a
newline-separated list of desktop-entry ids (optionally `id:ActionId`) in
`${desktop}-xdg-terminals.list` or plain `xdg-terminals.list` under `$XDG_CONFIG_HOME` (or the rest of
the XDG config hierarchy), first match by installed-desktop priority wins. dankc's existing
`xdg_default_terminal()`/`xdg_set_terminal()` (settings.c:753-792, reading/writing
`$XDG_CONFIG_HOME/xdg-terminals.list` directly) already matches this correctly and needs no change,
just needs its candidate list switched from name-heuristics to `Categories=TerminalEmulator` (see §3).

### 1.5 How GNOME does it

GNOME Settings' "Default Applications" panel (`gnome-control-center`) exposes a short, deliberately
small list — historically **Web, Mail, Calendar, Music, Video, Photos** (older GNOME versions also had
a Calendar row; newer ones trimmed it). It is built on `gio`/`GAppInfo`, which is itself a thin C
wrapper around the exact same `mimeapps.list` mechanism above (`g_app_info_set_as_default_for_type()`
writes `$XDG_CONFIG_HOME/mimeapps.list` the same way `xdg-mime default` does).
Notably, **vanilla GNOME does not expose a Terminal picker at all** — this is exactly why the
xdg-terminal-exec proposal exists as an out-of-band bolt-on, and it validates dankc's decision to treat
Terminal as its own special case rather than trying to force it through the MIME table.

### 1.6 How KDE does it

KDE Plasma splits this across **two** modules, which is the more instructive design point for dankc:

- **"Default Applications"** (`kcm_componentchooser`, `System Settings → Applications → Default
  Applications`) — a small, curated, **non-MIME, role-based** list: Web Browser, File Manager, Email
  Client, Terminal Emulator (Plasma 6 added Instant Messenger too). These are roles where "just look at
  MimeType=" is the wrong tool (see the file-manager problem in §2 below), so KDE (like DMS) filters by
  `Categories=` instead.
- **"File Associations"** (`kcm_filetypes`) — the exhaustive, generic, one-row-per-MIME-type list
  (image viewer, PDF viewer, archive manager, office documents, etc. are all just entries here, not
  special-cased).

This two-tier split (small curated role list + exhaustive per-MIME list) is effectively what dankc
should build in one screen: a curated category table where **most** categories are "first MIME type in
this category's list, GNOME-style," but Web Browser/File Manager/Terminal use `Categories=` filtering
like KDE/DMS, because plain `MimeType=` matching is too noisy or too narrow for those three roles
specifically.

### 1.7 How DMS itself does it (existing reference, same repo)

`quickshell/Modules/Settings/DefaultAppsTab.qml` (in this DankMaterialShell-master checkout) already
implements 10 categories: WebBrowser, FileManager, TextEditor, ImageViewer, VideoPlayer, MusicPlayer,
PDFReader, Mail, Terminal, Calendar — each mapped to an ordered MIME-type array (first entry used for
reads/enumeration, the full array passed to the multi-mime write). It delegates the actual
read/write/enumerate work to a native companion-daemon IPC call (`mime.getDefault`, `mime.setDefaults`,
`mime.appsForMime` via `DMSService.sendRequest`) rather than shelling out to `xdg-mime` per call — i.e.
DMS's own backend re-implements the mimeapps.list read/enumerate logic natively and only uses the
`xdg-mime`/`xdg-settings` binaries as the last-mile writer, which is exactly the split this plan
proposes for dankc (§4). Two things worth deviating from in DMS's mapping when building dankc's table:

- DMS's `Calendar` category maps to `x-scheme-handler/calendar`, which is **not a registered
  scheme/MIME** anywhere in the shared-mime-info database or freedesktop registry — it appears to be a
  DMS-specific placeholder. The freedesktop-standard MIME for calendar (`.ics`) files is **`text/calendar`**.
  dankc's table should use `text/calendar` as primary and can keep `x-scheme-handler/calendar` as a
  secondary/no-op-safe extra if compatibility with DMS-configured systems matters, but should not treat
  it as the source of truth.
- DMS uses `Categories=WebBrowser` / `Categories=FileManager` (not raw MIME matching) for those two rows
  — confirmed correct against this machine (§2) and worth carrying into dankc.

---

## 2. Category → MIME-type table (20 categories)

All MIME strings below are the real registered types (per shared-mime-info / freedesktop.org registered
schemes), cross-checked against this machine's `/usr/share/applications/mimeinfo.cache` and `.desktop`
`MimeType=` lines where installed apps exist. "Primary" = the one MIME type used for
`xdg-mime query default` reads and for enumerating candidates; "Also set" = additional MIME types
batched into the same `xdg-mime default app.desktop <primary> <also-set...>` write.

| # | Category | Primary MIME | Also set (same write) | Match method |
|---|----------|--------------|------------------------|--------------|
| 1 | Web Browser | `x-scheme-handler/http` | `x-scheme-handler/https`, `text/html`, `application/xhtml+xml` | `Categories=WebBrowser` (+ `xdg-settings set default-web-browser`, not raw `xdg-mime`) |
| 2 | Email | `x-scheme-handler/mailto` | — | MIME (`MimeType=`) |
| 3 | Calendar | `text/calendar` | (`x-scheme-handler/calendar` optional extra) | MIME |
| 4 | Music / Audio | `audio/mpeg` | `audio/flac`, `audio/x-flac`, `audio/ogg`, `audio/wav`, `audio/x-wav`, `audio/aac`, `audio/opus` | MIME |
| 5 | Video | `video/mp4` | `video/x-matroska`, `video/webm`, `video/mpeg`, `video/quicktime`, `video/x-msvideo`, `video/ogg` | MIME |
| 6 | Image Viewer | `image/png` | `image/jpeg`, `image/gif`, `image/bmp`, `image/webp`, `image/svg+xml`, `image/tiff` | MIME |
| 7 | Text Editor | `text/plain` | — | MIME |
| 8 | PDF Viewer | `application/pdf` | — | MIME |
| 9 | Document / Word | `application/vnd.oasis.opendocument.text` | `application/msword`, `application/vnd.openxmlformats-officedocument.wordprocessingml.document`, `text/rtf` | MIME |
| 10 | Spreadsheet | `application/vnd.oasis.opendocument.spreadsheet` | `application/vnd.ms-excel`, `application/vnd.openxmlformats-officedocument.spreadsheetml.sheet`, `text/csv` | MIME |
| 11 | Presentation | `application/vnd.oasis.opendocument.presentation` | `application/vnd.ms-powerpoint`, `application/vnd.openxmlformats-officedocument.presentationml.presentation` | MIME |
| 12 | Archive Manager | `application/zip` | `application/x-tar`, `application/x-7z-compressed`, `application/vnd.rar`, `application/gzip`, `application/x-bzip2`, `application/x-xz` | MIME |
| 13 | File Manager | `inode/directory` | — | `Categories=FileManager` (MIME query for reads only) |
| 14 | Terminal | *(none — role-based)* | — | `Categories=TerminalEmulator`, via `xdg-terminals.list` |
| 15 | Torrent Client | `application/x-bittorrent` | `x-scheme-handler/magnet` | MIME |
| 16 | Contacts | `text/vcard` | `text/x-vcard` | MIME |
| 17 | RSS / News Reader | `application/rss+xml` | `application/atom+xml` | MIME |
| 18 | Maps | `x-scheme-handler/geo` | — | MIME |
| 19 | E-book Reader | `application/epub+zip` | `application/x-mobipocket-ebook` | MIME |
| 20 | Software / App Center | `application/x-desktop` | (distro-specific: `application/vnd.debian.binary-package` on deb-based, `application/x-rpm` on rpm-based — omit or gate by `/etc/os-release`) | MIME (lowest priority — mark optional in the S/M/L sizing below) |

Rows 1, 13, 14 are the three "role-based, not MIME-based" exceptions carried forward unchanged from the
existing dankc/DMS code (§1.6); rows 2-12 and 15-20 are the new GNOME-style straight-MIME categories.

---

## 3. Candidate-app enumeration — measured on this machine

This machine (a minimal dev box — 34 system `.desktop` files in `/usr/share/applications`, 2 in
`~/.local/share/applications`) is a good stress test for the "few or zero candidates" UI state, since
most categories here have 0-2 real candidates:

| MIME / Categories query | Apps declaring it on this machine |
|---|---|
| `application/pdf` | 2 (`brave-origin.desktop`, `chromium.desktop` — browsers, not real PDF viewers) |
| `image/png` | 2 (same two browsers) |
| `text/plain` | 1 (`vim.desktop`) |
| `application/zip` | 1 (`org.gnome.Nautilus.desktop` — GNOME Files' built-in archive mounting) |
| `application/x-tar` / `application/vnd.rar` | 1 each (same Nautilus) |
| `inode/directory` | 1 (`org.gnome.Nautilus.desktop`) |
| `x-scheme-handler/http` | 3 (`brave-origin`, `chromium`, `dms-open`) |
| `application/rss+xml` | 2 (the two browsers, which declare it opportunistically) |
| `audio/mpeg`, `video/mp4`, `text/calendar`, `x-scheme-handler/mailto`, `application/vnd.oasis.opendocument.*`, `text/vcard`, `application/x-bittorrent`, `x-scheme-handler/geo`, `application/epub+zip`, `text/csv`, `video/x-matroska`, `x-scheme-handler/magnet`, `application/atom+xml` | **0** — no installed app declares these at all |
| `Categories=WebBrowser` | 2 (`brave-origin.desktop`, `chromium.desktop`) |
| `Categories=FileManager` | 1 (`org.gnome.Nautilus.desktop`) |
| `Categories=TerminalEmulator` | 1 (`Alacritty.desktop`) |

Takeaways for the implementation:

- **Only-declaring apps, not all apps.** GNOME's own picker (and DMS's `getAppsForMimeType`) only lists
  apps that actually declare the relevant `MimeType=`/`Categories=` — showing every installed app in
  every category (as a raw alphabetical list) would make Video/Music/PDF categories list web browsers
  as "PDF viewers" (since Chromium/Brave both declare `application/pdf` as their in-browser viewer),
  which is technically correct MIME data but a bad UX surprise. Recommendation: **keep the
  only-declaring-apps filter** (matches DMS and GNOME) but don't specifically special-case browsers out
  — a user really can pick their browser as their PDF viewer, that's valid.
  - This machine's data shows the real value: for most doc/media categories the honest answer is "no
    dedicated app is installed" — the UI needs a good empty state (dankc's existing
    `ui_hint(c, "No installed apps matched...")` pattern, reused, see §4).
- **`MimeType=` parsing beats `mimeinfo.cache`.** `/usr/share/applications/mimeinfo.cache` (generated by
  `update-desktop-database`) is a precomputed reverse index and would be a legitimate fast-path, but it's
  a cache that can go stale after installing an app without running `update-desktop-database` (common on
  Nix/Flatpak/AppImage installs, which write straight to `~/.local/share/applications` without always
  regenerating the cache) — parsing `MimeType=`/`Categories=` directly from every `.desktop` file, the
  way dankc's launcher (`dc_apps_load()`) already does for `Name=`/`Exec=`/`Icon=`, is the same one-pass
  cost and always correct. **Recommendation: parse `MimeType=`/`Categories=` directly, don't shell out
  to or parse `mimeinfo.cache`.**

---

## 4. Read/write logic per category

**Reads** stay exactly as dankc already does them — live, unconditional, cheap, cached 3s
(`SYS_CACHE_SECONDS`, settings.c:681) — via `popen()`:
- MIME categories (2-12, 15-20): `xdg-mime query default <primary-mime>`.
- Web Browser: `xdg-settings get default-web-browser`.
- File Manager: `xdg-mime query default inode/directory` (unchanged from current code).
- Terminal: `cat "${XDG_CONFIG_HOME:-$HOME/.config}/xdg-terminals.list"` (unchanged).

**Writes** stay detached shell commands gated by `DANKC_XDG_DRYRUN` (`run_xdg_detached()`,
settings.c:692-699), extended to pass every MIME type in a category's row in one call:
- MIME categories: `xdg-mime default <app>.desktop <primary> <also-set...>` — **one process, all MIME
  types for the category**, matching how `make_default_generic()`'s awk script batches them (§1.1) and
  how DMS's `setDefaultAppForMimes()` batches its array (§1.7). Do **not** issue one `xdg-mime default`
  call per MIME type — that's N file rewrites instead of 1 and no more correct.
- Web Browser: unchanged, `xdg-settings set default-web-browser <app>.desktop` (already correctly
  batches all 4 scheme handlers internally per §1.3 — don't reimplement that with raw `xdg-mime`).
  Optionally also fire `xdg-mime default <app>.desktop text/html` right after, since `xdg-settings`
  doesn't touch `text/html` itself (§1.3) and a user picking "Web Browser" reasonably expects local
  `.html` files to open there too.
- File Manager / Terminal: unchanged from current code.

**Gotchas to carry into the implementation:**
- Desktop-file id form: always `<id>.desktop`, stripped for comparison against `dc_app.id` — reuse
  `strip_desktop_suffix()` verbatim (settings.c:1509), don't duplicate it.
- `xdg-mime default` silently no-ops (exit code 4, "action failed") if the target app's own
  `.desktop` `MimeType=` doesn't list that MIME type at all (see the `xdg-mime --manual` text: *"The
  application's desktop file must list support for all the MIME types that it wishes to be the default
  handler for"*) — so the picker must only ever offer apps that already declare the MIME (which §3's
  enumeration approach guarantees by construction; no extra validation needed).
- mimeapps.list precedence (§1.1): dankc's write always lands in
  `$XDG_CONFIG_HOME/mimeapps.list` (the plain, non-desktop-prefixed file) via the real `xdg-mime`
  binary, so it's future-proof even if the precedence rules change upstream — dankc should keep
  shelling out to the real `xdg-mime`/`xdg-settings` binaries for writes rather than hand-rolling the
  INI edit, exactly as it does today.

---

## 5. C implementation plan for `src/ui/settings.c` / `services/apps.c`

### 5.1 Data structures

Add a static category table (this *is* the new "~15-20 categories" surface, replacing the three
hand-written blocks currently in `tab_default_apps()`):

```c
/* One Default Apps category. mimes[0] is the primary (read + first write arg);
 * mimes[1..n-1] are "also set" (same write call). categories[], if non-NULL,
 * switches candidate enumeration to Categories= matching instead of MimeType=
 * matching (Web Browser / File Manager / Terminal, see docs/17). */
typedef struct {
    const char *label;       /* UI row label, e.g. "Web Browser" */
    const char *section;     /* ui_section() header this row falls under */
    const char *const *mimes;
    int nmimes;
    const char *const *categories; /* NULL => use mimes for matching too */
    int ncategories;
    bool is_browser;   /* true only for the one row using xdg-settings */
    bool is_terminal;  /* true only for the one row using xdg-terminals.list */
} dc_default_app_category;

static const dc_default_app_category DEFAULT_APP_CATEGORIES[] = {
    { "Web Browser", "INTERNET", browser_mimes, 4, browser_cats, 1, true, false },
    { "Email", "INTERNET", mailto_mimes, 1, NULL, 0, false, false },
    { "Calendar", "INTERNET", calendar_mimes, 1, NULL, 0, false, false },
    { "RSS Reader", "INTERNET", rss_mimes, 2, NULL, 0, false, false },
    { "Maps", "INTERNET", geo_mimes, 1, NULL, 0, false, false },
    { "Torrent Client", "INTERNET", torrent_mimes, 2, NULL, 0, false, false },
    { "File Manager", "UTILITIES", filemgr_mimes, 1, filemgr_cats, 1, false, false },
    { "Terminal", "UTILITIES", NULL, 0, term_cats, 1, false, true },
    { "Archive Manager", "UTILITIES", archive_mimes, 7, NULL, 0, false, false },
    { "Text Editor", "DOCUMENTS", text_mimes, 1, NULL, 0, false, false },
    { "PDF Viewer", "DOCUMENTS", pdf_mimes, 1, NULL, 0, false, false },
    { "Document Viewer", "DOCUMENTS", doc_mimes, 4, NULL, 0, false, false },
    { "Spreadsheet", "DOCUMENTS", sheet_mimes, 4, NULL, 0, false, false },
    { "Presentation", "DOCUMENTS", slide_mimes, 3, NULL, 0, false, false },
    { "Contacts", "DOCUMENTS", vcard_mimes, 2, NULL, 0, false, false },
    { "E-book Reader", "DOCUMENTS", ebook_mimes, 2, NULL, 0, false, false },
    { "Image Viewer", "MULTIMEDIA", image_mimes, 7, NULL, 0, false, false },
    { "Video Player", "MULTIMEDIA", video_mimes, 6, NULL, 0, false, false },
    { "Music Player", "MULTIMEDIA", audio_mimes, 7, NULL, 0, false, false },
};
```

This turns `tab_default_apps()` into a single loop over `DEFAULT_APP_CATEGORIES`, calling one
generalized `default_app_row()` (evolution of the existing `default_app_role()`) that branches three
ways on `is_browser`/`is_terminal`/plain-MIME — instead of one hand-written block per category. Section
headers (`ui_section()`) fire when `section` changes between consecutive rows, same pattern already used
elsewhere in settings.c.

### 5.2 `services/apps.c` helper: enumerate by MimeType=/Categories=

Add to `apps.c` (alongside the existing `Name=`/`Exec=`/`Icon=`/`Comment=` parse in `dc_apps_load()`),
parsing `MimeType=` and `Categories=` into the existing per-app struct:

```c
/* apps.h additions */
#define DC_APP_MIME_MAX 512   /* raw ';'-joined MimeType= string, or empty */
#define DC_APP_CATS_MAX 256   /* raw ';'-joined Categories= string, or empty */
/* added to dc_app: char mimetypes[DC_APP_MIME_MAX]; char categories[DC_APP_CATS_MAX]; */

/* apps.c: parsed once during dc_apps_load()'s existing per-file scan, no extra
 * pass over the filesystem -- just two more fgets()-loop cases alongside the
 * existing Name=/Exec=/Icon=/Comment= handling. */

/* New query surface (apps.h): */
/* Apps whose MimeType= contains `mime` (';'-delimited exact-token match, not
 * substring -- avoids "audio/mp" matching "audio/mpeg" by accident). Writes up
 * to `max` pointers into `out`, returns count. */
int dc_apps_find_by_mime(dc_apps *apps, const char *mime, const dc_app **out, int max);

/* Apps whose Categories= contains `category` (same token-exact matching).
 * Used for the 3 role-based rows (WebBrowser/FileManager/TerminalEmulator). */
int dc_apps_find_by_category(dc_apps *apps, const char *category, const dc_app **out, int max);
```

This directly replaces `id_matches_any()`'s keyword-heuristic (settings.c:794-809) — that function and
its three hand-written keyword arrays (`browser_kw`, `filemgr_kw`, `term_kw`, settings.c:1570-1579) are
deleted once this lands, since real `MimeType=`/`Categories=` data is strictly more correct than
guessing from substrings of the app's name (confirmed by this doc's own §3 measurements — e.g. `vim` is
correctly a `text/plain` handler by declared MIME type, with zero name-guessing required, and would have
been *missed* entirely by a keyword list not already primed with "vim").

### 5.3 Picker UI

Reuses the existing pattern in `default_app_role()` (settings.c:1524-1556) essentially unchanged:
`ui_value()` for the current default, then up to 6 `ui_list_row()` candidates with "Default" tag on the
active one, click sets it via the row's write path. The only change is the *source* of the candidate
list (`dc_apps_find_by_mime`/`dc_apps_find_by_category` instead of `dc_apps_search("") + id_matches_any`)
and a generalized write path that batches `mimes[0..nmimes-1]` into one `xdg-mime default` call instead
of the current single-MIME `xdg_set_filemanager()`-style one-liners. The empty-state hint
(`"No installed apps matched..."`) carries over unchanged — §3's measurements confirm it'll fire often
on real systems for the niche categories (Contacts, Torrent, Maps, RSS), which is expected and fine.

### 5.4 `DANKC_XDG_DRYRUN`

No change needed — `run_xdg_detached()` (settings.c:692-699) already gates every write behind this env
var and logs the would-be command instead of running it. The new generalized multi-mime write path just
needs to keep calling through this same helper so `DANKC_XDG_DRYRUN=1 dankc ctl settings` remains the
safe way to click through all ~20 new rows offline (confirmed this is already how the existing 3 rows
are tested, per the comment block at settings.c:683-699).

### 5.5 Sizing

- **S** (~2-3h, 1 agent): Data-only change — build `DEFAULT_APP_CATEGORIES[]` and generalize
  `default_app_role()`/`tab_default_apps()` to loop over it, but keep candidate enumeration as the
  existing name-heuristic (`id_matches_any` + expanded keyword lists per new category) rather than doing
  the `apps.c` MimeType=/Categories= parsing work. Gets all ~20 categories on-screen and functional
  fastest, at the cost of keeping the known heuristic-matching weakness for all categories (not just the
  original 3).
- **M** (~1 day, 1-2 agents, recommended): Everything in S, plus §5.2's real `apps.c` MimeType=/
  Categories= parsing (new fields on `dc_app`, two new query functions, wired into `dc_apps_load()`'s
  existing scan loop) replacing the heuristic entirely. This is the "do it right" scope and isn't much
  more work than S since `dc_apps_load()` already walks every `.desktop` file once.
  - **Files touched**: `src/services/apps.h`, `src/services/apps.c` (new fields + 2 functions),
    `src/ui/settings.c` (category table + generalized row renderer, deletes the 3 old keyword arrays and
    `id_matches_any()`).
- **L** (~2-3 days): M, plus: (a) the Web Browser row also fires the extra `xdg-mime default
  <app>.desktop text/html` call noted in §4; (b) a `/etc/os-release`-gated Software/App-Center row
  (category #20, the one genuinely distro-specific entry — worth gating rather than showing a
  permanently-empty row on non-deb/rpm systems); (c) surfacing `[Added Associations]`/multi-candidate
  "Open With" data (out of scope for a *defaults* manager per se, but the parsing groundwork from M makes
  it cheap to bolt on later if ever wanted — not recommended to build proactively, flagged only for
  completeness).

Recommend **M** as the target scope: it fully satisfies "professionally done" (real MIME-type data
driving the picker, matching how GNOME/KDE/DMS itself all actually do it) without the distro-detection
and Open-With scope creep of L.
