#include "core/anim.h"

#include "core/config.h"

#include <math.h>
#include <time.h>

/* Cubic Bézier easing y(x) with implicit endpoints (0,0) and (1,1). Solves for
 * the parameter s where the curve's x equals the input t, then returns y(s).
 * Handles DMS's expressive overshoot curves (y may exceed 1). */
static float bezier(float x1, float y1, float x2, float y2, float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;

    /* x(s) as a function of the Bézier parameter s. */
    float s = t; /* initial guess */
    for (int i = 0; i < 8; i++) {
        float u = 1.0f - s;
        float x = 3.0f * u * u * s * x1 + 3.0f * u * s * s * x2 + s * s * s;
        float dx = 3.0f * u * u * (x1) + 6.0f * u * s * (x2 - x1) + 3.0f * s * s * (1.0f - x2);
        if (fabsf(dx) < 1e-6f)
            break;
        float err = x - t;
        if (fabsf(err) < 1e-5f)
            break;
        s -= err / dx;
        if (s < 0.0f)
            s = 0.0f;
        if (s > 1.0f)
            s = 1.0f;
    }
    float u = 1.0f - s;
    return 3.0f * u * u * s * y1 + 3.0f * u * s * s * y2 + s * s * s;
}

float dc_ease(dc_easing easing, float t)
{
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    float inv = 1.0f - t;
    switch (easing) {
    case DC_EASE_LINEAR:
        return t;
    case DC_EASE_STANDARD: /* OutCubic */
        return 1.0f - inv * inv * inv;
    case DC_EASE_EMPHASIZED: /* OutQuart */
        return 1.0f - inv * inv * inv * inv;
    case DC_EASE_EMPHASIZED_DECEL: /* DMS [0.05, 0.7, 0.1, 1] */
        return bezier(0.05f, 0.7f, 0.1f, 1.0f, t);
    case DC_EASE_EMPHASIZED_ACCEL: /* DMS [0.3, 0, 0.8, 0.15] */
        return bezier(0.3f, 0.0f, 0.8f, 0.15f, t);
    case DC_EASE_EXPRESSIVE: /* DMS default spatial [0.38, 1.21, 0.22, 1] */
        return bezier(0.38f, 1.21f, 0.22f, 1.0f, t);
    case DC_EASE_EXPRESSIVE_FAST: /* DMS fast spatial [0.42, 1.67, 0.21, 0.9] */
        return bezier(0.42f, 1.67f, 0.21f, 0.9f, t);
    }
    return t;
}

int64_t dc_anim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void dc_anim_start(dc_anim *a, dc_duration base_duration, dc_easing easing)
{
    const dc_config *cfg = dc_config_current;
    int dur = 0;
    if (cfg->animations_enabled) {
        float speed = cfg->animation_speed > 0.01f ? cfg->animation_speed : 1.0f;
        dur = (int)((float)base_duration / speed + 0.5f);
        if (dur < 1)
            dur = 1;
    }
    a->start_ms = dc_anim_now_ms();
    a->duration_ms = dur;
    a->easing = easing;
    a->active = dur > 0;
}

float dc_anim_progress(dc_anim *a)
{
    if (a->duration_ms <= 0) {
        a->active = false;
        return 1.0f;
    }
    int64_t elapsed = dc_anim_now_ms() - a->start_ms;
    if (elapsed >= a->duration_ms) {
        a->active = false;
        return 1.0f;
    }
    if (elapsed < 0)
        elapsed = 0;
    float t = (float)elapsed / (float)a->duration_ms;
    return dc_ease(a->easing, t);
}

bool dc_anim_active(const dc_anim *a)
{
    return a->active;
}
