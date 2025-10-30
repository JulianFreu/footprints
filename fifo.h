#ifndef FIFO_H
#define FIFO_H

#include <pthread.h>
#include "structs.h"
#include "map.h"



bool fifo_readData(struct fifo *fifo, MapTile *readData);
bool fifo_writeData(struct fifo *fifo, MapTile writeData);
bool fifo_searchData(struct fifo *fifo, MapTile searchData);
bool fifo_isEmpty(struct fifo *fifo);
bool fifo_isFull(struct fifo *fifo);

#endif