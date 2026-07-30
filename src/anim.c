#include "anim.h"

#include <math.h>

// Depends on nothing but libm, which is what lets the easing and the timing
// arithmetic be tested without a window.

static const float ANIM_PI = 3.14159265358979323846f;

// The ends are returned rather than computed. A panel left at 0.9999 by a
// rounding error would read as still moving and hold a redraw request open for
// as long as the program ran.
static float anim_ease(float t, AnimEase kind) {
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;

    switch (kind) {
    case ANIM_EASE_OUT:
        return sinf(t * ANIM_PI * 0.5f);
    case ANIM_EASE_IN_OUT:
        return 0.5f * (1.0f - cosf(t * ANIM_PI));
    case ANIM_LINEAR:
    default:
        return t;
    }
}

float anim_value(const Anim *anim) {
    if (anim->duration <= 0.0f || anim->elapsed >= anim->duration)
        return anim->to;

    return anim->from +
           (anim->to - anim->from) * anim_ease(anim->elapsed / anim->duration, anim->ease);
}

bool anim_active(const Anim *anim) {
    return anim->duration > 0.0f && anim->elapsed < anim->duration;
}

float anim_target(const Anim *anim) {
    return anim->to;
}

void anim_set(Anim *anim, float value) {
    anim->from = value;
    anim->to = value;
    anim->elapsed = 0.0f;
    anim->duration = 0.0f;
}

void anim_to(Anim *anim, float to, float duration, AnimEase ease) {
    // A settled Anim's value is its target, so comparing targets is enough to
    // recognise the call that asks for what is already happening. Without this
    // a per-frame caller would restart the move on every frame and it would
    // never arrive.
    if (anim->to == to)
        return;

    if (duration <= 0.0f) {
        anim_set(anim, to);
        return;
    }

    anim->from = anim_value(anim);
    anim->to = to;
    anim->elapsed = 0.0f;
    anim->duration = duration;
    anim->ease = ease;
}

bool anim_tick(Anim *anim, float dt) {
    if (!anim_active(anim))
        return false;

    anim->elapsed += dt;
    if (anim->elapsed > anim->duration)
        anim->elapsed = anim->duration;

    return true;
}

float anim_approach(float value, float target, float tau, float dt) {
    if (tau <= 0.0f || dt <= 0.0f)
        return tau <= 0.0f ? target : value;

    // Half-life form, so the result depends on how much time passed and not on
    // how it was divided into frames.
    return value + (target - value) * (1.0f - powf(0.5f, dt / tau));
}
