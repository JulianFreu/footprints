#ifndef FIFO_H
#define FIFO_H

#include <pthread.h>
#include "structs.h"
#include "map.h"



bool fifo_read_data(struct fifo *fifo, MapTile *readData);
bool fifo_write_data(struct fifo *fifo, MapTile writeData);
bool fifo_search_data(struct fifo *fifo, MapTile searchData);
bool fifo_is_empty(struct fifo *fifo);
bool fifo_is_full(struct fifo *fifo);

#endif