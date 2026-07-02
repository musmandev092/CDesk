#include "services/mpris.h"

#include "services/dbus.h"

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
    free(player);
}

bool dc_mpris_read(dc_mpris_info *out)
{
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
