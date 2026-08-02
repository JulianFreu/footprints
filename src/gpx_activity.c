#include "gpx_activity.h"

// The label lived in tracks.c, which is the rasteriser and needs SDL. Nothing
// about spelling an activity does, and the pure modules that want it -- the
// records table, and its test -- would otherwise have to drag the renderer in
// behind it.

const char *activity_type_label(ActivityType type) {
    switch (type) {
    case Run:
        return "Run";
    case Hike:
        return "Hike";
    case Cycling:
        return "Cycling";
    case Other:
    case ACTIVITY_TYPE_COUNT:
        break;
    }
    return "Other";
}
