/* audio.h — default-sink volume/mute via WirePlumber's `wpctl`.
 *
 * A light stand-in until the libpipewire service lands (M3); dependency-free
 * beyond the wpctl binary and good enough for the bar indicator.
 */
#ifndef DC_SERVICES_AUDIO_H
#define DC_SERVICES_AUDIO_H

#include <stdbool.h>

typedef struct dc_audio_info {
    bool available;
    int volume; /* 0-100 */
    bool muted;
} dc_audio_info;

/* Read the default sink's volume + mute. Returns true on success. */
bool dc_audio_read(dc_audio_info *out);

#endif /* DC_SERVICES_AUDIO_H */
