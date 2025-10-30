#include "fifo.h"

void fifo_incrementPointer(int *fifo_pointer);

bool fifo_readData(struct fifo *fifo, MapTile *readData)
{
    bool readSuccess = false;
    if (fifo->read_p != fifo->write_p)
    {
        *readData = fifo->tile[fifo->read_p];
        fifo_incrementPointer(&fifo->read_p);
        readSuccess = true;
    }
    return readSuccess;
}

bool fifo_writeData(struct fifo *fifo, MapTile writeData)
{
    pthread_mutex_lock(&fifo->lock);
    bool writeSuccess = false;
    if (!fifo_isFull(fifo))
    {
        fifo->tile[fifo->write_p] = writeData;
        fifo_incrementPointer(&fifo->write_p);
        writeSuccess = true;
        pthread_cond_signal(&fifo->cond); // wake up thread
    }
    pthread_mutex_unlock(&fifo->lock);
    return writeSuccess;
}

bool fifo_searchData(struct fifo *fifo, MapTile searchData)
{
    pthread_mutex_lock(&fifo->lock);
    bool foundData = false;
    int search_p = fifo->read_p;
    while (search_p != fifo->write_p)
    {
        if (fifo->tile[search_p].tile_x == searchData.tile_x &&
            fifo->tile[search_p].tile_y == searchData.tile_y &&
            fifo->tile[search_p].zoom == searchData.zoom)
        {
            foundData = true;
        }
        fifo_incrementPointer(&search_p);
    }
    pthread_mutex_unlock(&fifo->lock);
    return foundData;
}

void fifo_incrementPointer(int *fifo_pointer)
{
    if (*fifo_pointer < FIFO_DEPTH)
    {
        (*fifo_pointer)++;
    }
    else
    {
        *fifo_pointer = 0;
    }
}

bool fifo_isFull(struct fifo *fifo)
{
    // isFull = true when only one space is left
    bool isFull = false;
    if (fifo->write_p == FIFO_DEPTH && fifo->read_p == 0)
    {
        isFull = true;
    }
    else if (fifo->write_p == fifo->read_p - 1)
    {
        isFull = true;
    }
    return isFull;
}

bool fifo_isEmpty(struct fifo *fifo)
{
//    pthread_mutex_lock(&fifo->lock);
//    // isFull = true when only one space is left
//    bool isEmpty = false;
//    if (fifo->write_p == fifo->read_p)
//    {
//        isEmpty = true;
//    }
//    pthread_mutex_unlock(&fifo->lock);
    return fifo->write_p == fifo->read_p;
}