#ifndef TRACK_SORT_H
#define TRACK_SORT_H

#include "gpx_types.h"

// Reorders collection->list_order by `criteria`. Asking for the criteria the
// list is already sorted by reverses it instead, which is what clicking the
// same column header twice does.
void track_sort_by(GpxCollection *collection, AttributeType criteria);

#endif
