#ifndef MAIN_H
#define MAIN_H

#include "app.h"
#include "filters.h"
#include "gpxParser.h"
#include "gpx_types.h"
#include "heat.h"
#include "map.h"
#include "tracks.h"
#include "ui.h"

bool sdl_initialize(struct application *appl);
bool appl_cleanup(struct application *appl, GpxCollection *collection, int exit_status);
bool handle_events(struct application *appl, GpxCollection *collection);

#endif