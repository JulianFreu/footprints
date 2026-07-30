#ifndef FIFO_H
#define FIFO_H

#include <pthread.h>
#include <stdbool.h>

#include "map_types.h"

// None of these functions lock. The queue is shared between the main loop and
// the tile download thread, so every call must be made with fifo->lock held --
// including the fifo_is_empty() test that guards a pthread_cond_wait().

bool fifo_read_data(struct fifo *fifo, MapTile *readData);
bool fifo_write_data(struct fifo *fifo, MapTile writeData);
bool fifo_search_data(struct fifo *fifo, MapTile searchData);
bool fifo_is_empty(struct fifo *fifo);
bool fifo_is_full(struct fifo *fifo);

#endif
