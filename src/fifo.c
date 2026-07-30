#include "fifo.h"

// Advance a ring index, wrapping at the end of the buffer. Kept as the single
// place wraparound is expressed so read_p, write_p and the search cursor cannot
// drift apart.
static int fifo_next(int index) {
    return (index + 1) % FIFO_DEPTH;
}

bool fifo_read_data(struct fifo *fifo, MapTile *readData) {
    bool readSuccess = false;
    if (fifo->read_p != fifo->write_p) {
        *readData = fifo->tile[fifo->read_p];
        fifo->read_p = fifo_next(fifo->read_p);
        readSuccess = true;
    }
    return readSuccess;
}

bool fifo_write_data(struct fifo *fifo, MapTile writeData) {
    //    pthread_mutex_lock(&fifo->lock);
    bool writeSuccess = false;
    if (!fifo_is_full(fifo)) {
        fifo->tile[fifo->write_p] = writeData;
        fifo->write_p = fifo_next(fifo->write_p);
        writeSuccess = true;
        pthread_cond_signal(&fifo->cond); // wake up thread
    }
    //pthread_mutex_unlock(&fifo->lock);
    return writeSuccess;
}

bool fifo_search_data(struct fifo *fifo, MapTile searchData) {
    //pthread_mutex_lock(&fifo->lock);
    bool foundData = false;
    int search_p = fifo->read_p;
    while (search_p != fifo->write_p) {
        if (fifo->tile[search_p].tile_x == searchData.tile_x &&
            fifo->tile[search_p].tile_y == searchData.tile_y &&
            fifo->tile[search_p].zoom == searchData.zoom) {
            foundData = true;
            break;
        }
        search_p = fifo_next(search_p);
    }
    //pthread_mutex_unlock(&fifo->lock);
    return foundData;
}

bool fifo_is_full(struct fifo *fifo) {
    // One slot is deliberately left unused so that a full buffer stays
    // distinguishable from an empty one (both would otherwise have
    // read_p == write_p).
    return fifo_next(fifo->write_p) == fifo->read_p;
}

bool fifo_is_empty(struct fifo *fifo) {
    return fifo->write_p == fifo->read_p;
}