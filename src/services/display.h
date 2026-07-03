/* display.h — monitor/output configuration over native niri IPC.
 *
 * Read side talks the raw niri socket protocol ($NIRI_SOCKET, newline-JSON,
 * see docs/03-SERVICES.md sec.12) directly with a one-shot connection per
 * call -- the same "one connection per one-shot request" rule
 * dc_niri_connect()'s EventStream comment documents, just not reusing that
 * long-lived connection (it's pinned to the event stream).
 *
 * Write side shells out to the `niri msg output <name> <action>` CLI
 * (fire-and-forget fork+execlp), matching dc_niri_focus_workspace()'s shape
 * in src/niri/niri.c -- these are RUNTIME-ONLY changes; niri forgets them on
 * restart. Call dc_display_persist() to also write a matching `output {}`
 * KDL block so the change survives, mirroring the Window Rules editor's
 * managed-include pattern (src/ui/settings.c sec. "Window Rules").
 */
#ifndef DC_SERVICES_DISPLAY_H
#define DC_SERVICES_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

#define DC_DISPLAY_MAX_OUTPUTS 16
#define DC_DISPLAY_MAX_MODES 64
#define DC_DISPLAY_NAME_MAX 64
#define DC_DISPLAY_STR_MAX 128

typedef struct dc_display_mode {
    int width;
    int height;
    /* Refresh rate in millihertz, exactly as niri reports it (e.g. 60020 =
     * 60.020 Hz). Divide by 1000.0 for display/CLI formatting. */
    int refresh_mhz;
    bool is_preferred;
} dc_display_mode;

typedef enum {
    DC_DISPLAY_TRANSFORM_NORMAL = 0,
    DC_DISPLAY_TRANSFORM_90,
    DC_DISPLAY_TRANSFORM_180,
    DC_DISPLAY_TRANSFORM_270,
    DC_DISPLAY_TRANSFORM_FLIPPED,
    DC_DISPLAY_TRANSFORM_FLIPPED_90,
    DC_DISPLAY_TRANSFORM_FLIPPED_180,
    DC_DISPLAY_TRANSFORM_FLIPPED_270,
} dc_display_transform;

/* One connected output, as reported by `"Outputs"` (niri msg -j outputs). */
typedef struct dc_display_info {
    char name[DC_DISPLAY_NAME_MAX]; /* connector, e.g. "eDP-1", "HDMI-A-1" */
    char make[DC_DISPLAY_STR_MAX];
    char model[DC_DISPLAY_STR_MAX];
    char serial[DC_DISPLAY_STR_MAX]; /* empty if niri reported null */

    dc_display_mode modes[DC_DISPLAY_MAX_MODES];
    int mode_count;
    int current_mode_idx; /* index into modes[], -1 if none/unknown */

    bool vrr_supported;
    bool vrr_enabled;

    /* niri omits "logical" entirely when the output is disabled -- absence
     * of a logical block is exactly what `enabled` tracks. */
    bool enabled;
    int x, y;               /* logical position, arrangement/primary layout */
    int logical_width, logical_height;
    double scale;
    dc_display_transform transform;

    /* Not a niri wire field: set by dc_display_list() from a second
     * "FocusedOutput" request so the UI can highlight the active monitor.
     * niri has no separate "primary" concept -- by convention (and this
     * plan's design) the output at logical position (0,0) is "primary". */
    bool is_focused;
} dc_display_info;

/* One-shot read: connects to $NIRI_SOCKET, sends "Outputs" and
 * "FocusedOutput", fills `out[0..DC_DISPLAY_MAX_OUTPUTS)`, returns the
 * count (0 on any failure -- NIRI_SOCKET unset, connect failed, bad JSON).
 * Synchronous/blocking (bounded by AF_UNIX localhost round-trip); do not
 * call from a latency-sensitive render path. */
int dc_display_list(dc_display_info out[DC_DISPLAY_MAX_OUTPUTS]);

/* Format a mode's refresh rate as niri's CLI expects, e.g. 60020 -> "60.020".
 * Writes into `buf` (must be >= 16 bytes). Convenience for callers building
 * their own mode strings; dc_display_set_mode() already does this. */
void dc_display_format_refresh(int refresh_mhz, char *buf, size_t buf_cap);

/* --- runtime writes (niri msg output <name> ...) ------------------------
 *
 * All fire-and-forget fork+execlp, exactly like dc_niri_focus_workspace():
 * apply instantly for the running session, forgotten on niri restart unless
 * also persisted via dc_display_persist(). Safe to call even if `name`
 * isn't currently connected (niri itself no-ops/logs and returns success --
 * verified in place: it does not report output-not-found back over the
 * CLI's exit code).
 *
 * When $DANKC_DISPLAY_DRYRUN=1, none of these fork/exec anything -- they
 * log the exact `niri msg output ...` argv at INFO level instead. Use this
 * to verify call shape without touching a live session. */
void dc_display_set_mode(const char *name, int width, int height, int refresh_mhz);
void dc_display_set_mode_auto(const char *name);
void dc_display_set_scale(const char *name, double scale);
void dc_display_set_position(const char *name, int x, int y);
void dc_display_set_position_auto(const char *name);
void dc_display_set_transform(const char *name, dc_display_transform transform);
void dc_display_set_enabled(const char *name, bool enabled);
void dc_display_set_vrr(const char *name, bool enabled);

/* --- persistence (~/.config/niri/config.kdl) ----------------------------
 *
 * Runtime `niri msg output` changes above do NOT survive a niri restart --
 * niri's own docs call this out ("Change output configuration temporarily").
 * To persist, dankc writes/updates a managed KDL include file exactly like
 * the Window Rules editor (src/ui/settings.c, dankc-rules.kdl): a
 * dankc-owned ~/.config/niri/dankc-outputs.kdl holding one `output "<name>"
 * { ... }` block per configured monitor, plus (only on first use, and only
 * if not already present) a single backed-up `include` line appended to the
 * user's real config.kdl. dankc never rewrites hand-written config.kdl
 * content beyond that one include line.
 *
 * This is explicit/opt-in -- callers should treat persistence as a
 * "Save as default" action, not something that happens on every live drag
 * of a slider, since KDL writes touch disk and (on first use) the user's
 * real config.
 */
typedef struct dc_display_persist_config {
    char name[DC_DISPLAY_NAME_MAX];
    bool has_mode;
    int width, height, refresh_mhz; /* refresh_mhz 0 => omit @refresh (any matching mode) */
    bool mode_auto;
    bool has_scale;
    double scale;
    bool has_transform;
    dc_display_transform transform;
    bool has_position;
    int x, y;
    bool position_auto;
    bool has_enabled;
    bool enabled;
    bool has_vrr;
    bool vrr_enabled;
} dc_display_persist_config;

/* Rewrites the managed KDL file (dankc-outputs.kdl by default) from the
 * caller-supplied config list, creating/backing-up the config.kdl `include`
 * line on first use. `config_dir` overrides "$HOME/.config/niri" (pass NULL
 * for the real path) -- tests point this at an isolated temp directory so
 * nothing ever touches the user's real ~/.config/niri/config.kdl during
 * verification. Returns true on success. */
bool dc_display_persist(const dc_display_persist_config configs[], int count,
        const char *config_dir_override);

/* Human-readable transform name matching niri's own CLI vocabulary
 * ("normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180",
 * "flipped-270") -- used both for `niri msg output ... transform <t>` and
 * for the persisted KDL's `transform "<t>"` value. */
const char *dc_display_transform_name(dc_display_transform t);
dc_display_transform dc_display_transform_from_name(const char *name);

#endif /* DC_SERVICES_DISPLAY_H */
