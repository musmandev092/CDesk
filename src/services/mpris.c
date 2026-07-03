#include "services/mpris.h"

#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DC_MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define DC_MPRIS_PATH "/org/mpris/MediaPlayer2"
#define DC_MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"

static sd_bus *g_user = NULL;
static dc_mpris_info g_cache;
static time_t g_last = 0;

void dc_mpris_init(struct dc_dbus *dbus)
{
    g_user = dbus ? dbus->user : NULL;
}

/* First bus name matching the MPRIS prefix. malloc'd or NULL. */
static char *find_player(void)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_user, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "ListNames", &err, &reply, "");
    if (r < 0) {
        sd_bus_error_free(&err);
        return NULL;
    }

    char *found = NULL;
    sd_bus_message_enter_container(reply, 'a', "s");
    const char *name = NULL;
    while (sd_bus_message_read_basic(reply, 's', &name) > 0) {
        if (strncmp(name, DC_MPRIS_PREFIX, strlen(DC_MPRIS_PREFIX)) == 0) {
            found = strdup(name);
            break;
        }
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);
    return found;
}

/* Parse the Metadata a{sv} property for xesam:title and xesam:artist[0]. */
static void read_metadata(const char *player, dc_mpris_info *info)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(g_user, player, DC_MPRIS_PATH, DC_MPRIS_PLAYER_IFACE, "Metadata",
                               &err, &reply, "a{sv}");
    if (r < 0) {
        sd_bus_error_free(&err);
        return;
    }

    sd_bus_message_enter_container(reply, 'a', "{sv}");
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read_basic(reply, 's', &key);
        if (key && strcmp(key, "xesam:title") == 0) {
            const char *val = NULL;
            sd_bus_message_enter_container(reply, 'v', "s");
            sd_bus_message_read_basic(reply, 's', &val);
            sd_bus_message_exit_container(reply);
            if (val)
                snprintf(info->title, sizeof(info->title), "%s", val);
        } else if (key && strcmp(key, "xesam:artist") == 0) {
            sd_bus_message_enter_container(reply, 'v', "as");
            sd_bus_message_enter_container(reply, 'a', "s");
            const char *artist = NULL;
            if (sd_bus_message_read_basic(reply, 's', &artist) > 0 && artist)
                snprintf(info->artist, sizeof(info->artist), "%s", artist);
            sd_bus_message_exit_container(reply);
            sd_bus_message_exit_container(reply);
        } else if (key && strcmp(key, "mpris:artUrl") == 0) {
            const char *val = NULL;
            sd_bus_message_enter_container(reply, 'v', "s");
            sd_bus_message_read_basic(reply, 's', &val);
            sd_bus_message_exit_container(reply);
            if (val)
                snprintf(info->art_url, sizeof(info->art_url), "%s", val);
        } else if (key && strcmp(key, "mpris:length") == 0) {
            /* Length is a{sv}-boxed int64 ('x') on most players, but some use
             * uint64 ('t'); read whichever the variant actually holds. */
            char type = 0;
            const char *contents = NULL;
            if (sd_bus_message_peek_type(reply, &type, &contents) > 0 && type == 'v' && contents) {
                sd_bus_message_enter_container(reply, 'v', contents);
                if (contents[0] == 't') {
                    uint64_t len = 0;
                    sd_bus_message_read_basic(reply, 't', &len);
                    info->length_us = (int64_t)len;
                } else {
                    int64_t len = 0;
                    sd_bus_message_read_basic(reply, 'x', &len);
                    info->length_us = len;
                }
                sd_bus_message_exit_container(reply);
            } else {
                sd_bus_message_skip(reply, "v");
            }
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply); /* sv */
    }
    sd_bus_message_exit_container(reply); /* a{sv} */
    sd_bus_message_unref(reply);
}

static void refresh(dc_mpris_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!g_user)
        return;

    char *player = find_player();
    if (!player)
        return;
    info->active = true;

    char *status = NULL;
    if (sd_bus_get_property_string(g_user, player, DC_MPRIS_PATH, DC_MPRIS_PLAYER_IFACE,
                                   "PlaybackStatus", NULL, &status) >= 0 &&
        status) {
        info->playing = strcmp(status, "Playing") == 0;
        free(status);
    }

    read_metadata(player, info);

    /* Player.Position is a plain int64 property (microseconds), not part of
     * Metadata (docs/13-POPOUTS-SPEC.md sec.5 progress bar). */
    sd_bus_error perr = SD_BUS_ERROR_NULL;
    sd_bus_message *preply = NULL;
    if (sd_bus_get_property(g_user, player, DC_MPRIS_PATH, DC_MPRIS_PLAYER_IFACE, "Position", &perr,
                            &preply, "x") >= 0) {
        int64_t pos = 0;
        if (sd_bus_message_read_basic(preply, 'x', &pos) > 0)
            info->position_us = pos;
        sd_bus_message_unref(preply);
    } else {
        sd_bus_error_free(&perr);
    }

    free(player);
}

bool dc_mpris_read(dc_mpris_info *out)
{
    /* DANKC_MARQUEE_TEST=1 (debug-only, env-gated — see main.c's DANKC_*_DEMO
     * flags for the same convention): inject a long fake "title • artist" so
     * the bar's media marquee (docs/12-BAR-SPEC.md sec.4 music) can be
     * exercised deterministically on a machine with no live MPRIS player.
     * DANKC_MARQUEE_TEST=paused does the same but with playing=false, so the
     * play (rather than pause) transport glyph can be screenshotted for
     * verification too. Checked once and cached; harmless/no-op when unset. */
    static int test_mode = -1;
    if (test_mode < 0) {
        const char *v = getenv("DANKC_MARQUEE_TEST");
        test_mode = !v ? 0 : (strcmp(v, "paused") == 0 ? 2 : 1);
    }
    if (test_mode) {
        memset(out, 0, sizeof(*out));
        out->active = true;
        out->playing = test_mode == 1;
        snprintf(out->title, sizeof(out->title),
                 "This Is A Deliberately Long Fake Track Title For Marquee Testing");
        snprintf(out->artist, sizeof(out->artist), "A Fake Artist Whose Name Also Runs Long");
        return true;
    }

    time_t now = time(NULL);
    if (g_last == now) {
        *out = g_cache;
        return g_cache.active;
    }
    g_last = now;
    refresh(&g_cache);
    *out = g_cache;
    return g_cache.active;
}

/* Fire-and-forget call of a no-arg/no-reply-value MPRIS Player method on the
 * first player found (docs/12-BAR-SPEC.md sec.4/5: music prev/play/next). */
static void call_player_method(const char *method)
{
    if (!g_user)
        return;
    char *player = find_player();
    if (!player)
        return;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_user, player, DC_MPRIS_PATH, DC_MPRIS_PLAYER_IFACE, method, &err,
                               &reply, "");
    if (r < 0)
        sd_bus_error_free(&err);
    if (reply)
        sd_bus_message_unref(reply);
    free(player);

    /* Force a fresh read on the next dc_mpris_read() instead of serving the
     * pre-click cache for up to a second. */
    g_last = 0;
}

void dc_mpris_play_pause(void)
{
    call_player_method("PlayPause");
}

void dc_mpris_next(void)
{
    call_player_method("Next");
}

void dc_mpris_previous(void)
{
    call_player_method("Previous");
}
