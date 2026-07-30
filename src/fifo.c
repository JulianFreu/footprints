#include "fifo.h"

// Advance a ring index, wrapping at the end of the buffer. Kept as the single
// place wraparound is expressed so read_p, write_p and the search cursor cannot
// drift apart.
static int fifo_next(int index) {
    return (index + 1) % FIFO_DEPTH;
}

bool fifo_read_data(struct fifo *fifo, MapTile *read_data) {
    bool read_success = false;
    if (fifo->read_p != fifo->write_p) {
        *read_data = fifo->tile[fifo->read_p];
        fifo->read_p = fifo_next(fifo->read_p);
        read_success = true;
    }
    return read_success;
}

bool fifo_write_data(struct fifo *fifo, MapTile write_data) {
    bool write_success = false;
    if (!fifo_is_full(fifo)) {
        fifo->tile[fifo->write_p] = write_data;
        fifo->write_p = fifo_next(fifo->write_p);
        write_success = true;
        pthread_cond_signal(&fifo->cond); // wake up thread
    }
    return write_success;
}

bool fifo_search_data(struct fifo *fifo, MapTile search_data) {
    bool found_data = false;
    int search_p = fifo->read_p;
    while (search_p != fifo->write_p) {
        if (fifo->tile[search_p].tile_x == search_data.tile_x &&
            fifo->tile[search_p].tile_y == search_data.tile_y &&
            fifo->tile[search_p].zoom == search_data.zoom) {
            found_data = true;
            break;
        }
        search_p = fifo_next(search_p);
    }
    return found_data;
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