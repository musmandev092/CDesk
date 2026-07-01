#include "services/audio.h"

#include <stdio.h>
#include <string.h>

bool dc_audio_read(dc_audio_info *out)
{
    out->available = false;
    out->volume = 0;
    out->muted = false;

    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (!pipe)
        return false;

    char line[128];
    bool ok = false;
    if (fgets(line, sizeof(line), pipe)) {
        float volume = 0.0f;
        if (sscanf(line, "Volume: %f", &volume) == 1) {
            out->volume = (int)(volume * 100.0f + 0.5f);
            out->available = true;
            ok = true;
        }
        if (strstr(line, "MUTED"))
            out->muted = true;
    }
    pclose(pipe);
    return ok;
}
