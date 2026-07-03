/* anim.h — timing + easing for panel animations.
 *
 * Mirrors DankMaterialShell's motion system: Material 3 durations and easing
 * curves (standard OutCubic, emphasized OutQuart, expressive spatial overshoot),
 * scaled by the user's animationSpeed / animationsEnabled config. See
 * docs/10-DESIGN-SYSTEM (motion).
 */
#ifndef DC_CORE_ANIM_H
#define DC_CORE_ANIM_H

#include <stdbool.h>
#include <stdint.h>

/* Base durations in ms (DMS: short=150, medium=300, long=450, expressive=500). */
typedef enum {
    DC_DUR_SHORT = 150,
    DC_DUR_MEDIUM = 300,
    DC_DUR_LONG = 450,
    DC_DUR_EXPRESSIVE = 500,
    /* Dock reveal/hide slide (docs/11-UX-FLOW.md sec.5/7: "Dock slide | 225 ms
     * OutCubic" — deliberately its own value since it doesn't match any of
     * the generic short/medium/long buckets above). */
    DC_DUR_DOCK_SLIDE = 225,
} dc_duration;

typedef enum {
    DC_EASE_LINEAR,
    DC_EASE_STANDARD,        /* OutCubic — general UI motion */
    DC_EASE_EMPHASIZED,      /* OutQuart — larger/important transitions */
    DC_EASE_EMPHASIZED_DECEL,/* enter: decelerate into place */
    DC_EASE_EMPHASIZED_ACCEL,/* exit: accelerate away */
    DC_EASE_EXPRESSIVE,      /* spatial overshoot (spring-like) */
    DC_EASE_EXPRESSIVE_FAST, /* quicker spatial overshoot */
} dc_easing;

/* Evaluate `easing` at linear input t (0..1). May return >1 for overshoot
 * curves. */
float dc_ease(dc_easing easing, float t);

typedef struct dc_anim {
    int64_t start_ms;
    int duration_ms; /* already scaled by config; 0 = instant */
    dc_easing easing;
    bool active;
} dc_anim;

int64_t dc_anim_now_ms(void);

/* Begin an animation now. `base_duration` is a DC_DUR_* value; it is scaled by
 * the active config (animationSpeed, animationsEnabled -> 0). */
void dc_anim_start(dc_anim *a, dc_duration base_duration, dc_easing easing);

/* Eased progress at the current time (0..1, may overshoot >1). Returns 1 for a
 * finished or never-started animation. Clears `active` once complete. */
float dc_anim_progress(dc_anim *a);

/* True while the animation is still running (needs another frame). */
bool dc_anim_active(const dc_anim *a);

#endif /* DC_CORE_ANIM_H */
