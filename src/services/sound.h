/* sound.h — freedesktop notification-sound playback (docs/14-COMPLETION-PLAN.md W1.3).
 *
 * Lightest available mechanism: resolve a freedesktop sound-theme event name
 * to a file, then spawn `pw-play`/`paplay` detached (fire-and-forget), same
 * fork+setsid+exec pattern already used by services/audio.c and friends. No
 * new library dependency (no canberra, no libpipewire linkage).
 */
#ifndef DC_SERVICES_SOUND_H
#define DC_SERVICES_SOUND_H

#include "services/notifications.h"

/* Play the notification sound for `urgency`, subject to (in order):
 *   - dc_config_current->sounds_enabled (master switch)
 *   - dc_config_current->notif_sound_enabled (per-event toggle)
 *   - dc_config_current->dnd_enabled (do-not-disturb suppresses sound too --
 *     see notifications.c's method_notify call site for why this
 *     intentionally differs from upstream DMS, which only gates the toast)
 *   - a process-wide ~300ms debounce so a notify-send burst doesn't spawn a
 *     player per notification (docs/14 W1.3 acceptance: burst of 5 -> <=2
 *     sounds)
 * Safe to call unconditionally from the Notify() handler; every failure mode
 * (no player binary, no resolvable sound file) degrades silently to a no-op
 * plus at most one warning log line, never a crash. */
void dc_sound_notify(dc_urgency urgency);

#endif
