#ifndef STRUCTS_H
#define STRUCTS_H

// Umbrella header kept for convenience. Prefer including the specific type
// header a module actually needs:
//
//   config.h        compile-time constants and tunables
//   map_types.h     tiles, texture caches, the download queue
//   gpx_types.h     points, tracks, the track collection
//   filter_types.h  FilterSettings
//   ui_types.h      UI state, fonts, icons, filter id bits
//   heat_types.h    k-d tree and heatmap worker tasks
//   app.h           struct application

#include "app.h"
#include "config.h"
#include "filter_types.h"
#include "gpx_types.h"
#include "heat_types.h"
#include "map_types.h"
#include "ui_types.h"

#endif
