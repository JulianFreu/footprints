#ifndef ANIM_H
#define ANIM_H

#include <stdbool.h>

// A scalar that moves toward a target over time, and the one place the frame
// loop learns that something is still moving.
//
// Everything the program animates is one of these: a panel's 0..1 slide, and
// whatever is added next. Ticking one reports whether it moved, and that is
// what raises the redraw request -- so nothing has to be listed in the frame
// loop by hand for its animation to keep running.

typedef enum AnimEase {
    ANIM_LINEAR,
    ANIM_EASE_OUT,    // sin(t * pi/2): quick off the mark, settles at the end
    ANIM_EASE_IN_OUT, // (1 - cos(t * pi)) / 2: settles at both ends
} AnimEase;

typedef struct Anim {
    float from;
    float to;
    float elapsed;  // seconds into the move
    float duration; // seconds the move takes; zero means nothing is running
    AnimEase ease;
} Anim;

// The value right now. A zeroed Anim reads as 0 and is not active, so a struct
// that contains one needs no initialisation of its own.
float anim_value(const Anim *anim);

// Whether the value still has somewhere to go.
bool anim_active(const Anim *anim);

// Where it is headed, which for a settled Anim is where it already is. This is
// what to ask when reversing something: a panel that is a tenth of the way
// open is still an opening panel, and toggling it should shut it.
float anim_target(const Anim *anim);

// Puts the value at `value` immediately, cancelling any move in flight.
void anim_set(Anim *anim, float value);

// Moves from wherever the value is now to `to`, over `duration` seconds.
// Retargeting mid-move starts from the current value, so a panel caught
// half-open does not jump. Asking for the target it already has does nothing,
// which is what lets this be driven from a per-frame condition rather than only
// from the event that changed it.
void anim_to(Anim *anim, float to, float duration, AnimEase ease);

// Advances by `dt` seconds. Returns whether the value moved -- including on the
// step that settles it, so the frame showing the final value is still drawn.
bool anim_tick(Anim *anim, float dt);

// Chases a target that is itself still moving, with no fixed duration: the
// value closes the remaining gap with a half-life of `tau` seconds. A tween
// restarts its clock every time the target shifts, which reads as lag; this
// does not.
float anim_approach(float value, float target, float tau, float dt);

#endif
