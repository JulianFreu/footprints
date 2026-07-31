#include "../src/fifo.c"

#include "harness.h"

static struct fifo new_fifo(void) {
    struct fifo f = {0};
    pthread_mutex_init(&f.lock, NULL);
    pthread_cond_init(&f.cond, NULL);
    return f;
}

static MapTile tile(int x, int y, int z) {
    return (MapTile){.tile_x = x, .tile_y = y, .zoom = z};
}

void run_fifo_tests(void) {
    struct fifo f = new_fifo();

    SUITE("fifo: starts empty");
    CHECK(fifo_is_empty(&f));
    CHECK(!fifo_is_full(&f));
    MapTile out;
    CHECK(!fifo_read_data(&f, &out));

    SUITE("fifo: round-trips a tile");
    CHECK(fifo_write_data(&f, tile(1, 2, 3)));
    CHECK(!fifo_is_empty(&f));
    CHECK(fifo_read_data(&f, &out));
    CHECK_INT(out.tile_x, 1);
    CHECK_INT(out.tile_y, 2);
    CHECK_INT(out.zoom, 3);
    CHECK(fifo_is_empty(&f));

    SUITE("fifo: preserves order");
    for (int i = 0; i < 5; i++)
        CHECK(fifo_write_data(&f, tile(i, i, 12)));
    for (int i = 0; i < 5; i++) {
        CHECK(fifo_read_data(&f, &out));
        CHECK_INT(out.tile_x, i);
    }

    SUITE("fifo: one slot is left unused so full != empty");
    // The invariant the whole ring depends on: a full queue holds
    // FIFO_DEPTH - 1 entries, so read_p == write_p means empty and nothing
    // else.
    f = new_fifo();
    int written = 0;
    while (fifo_write_data(&f, tile(written, 0, 12)))
        written++;
    CHECK_INT(written, FIFO_DEPTH - 1);
    CHECK(fifo_is_full(&f));
    CHECK(!fifo_is_empty(&f));

    SUITE("fifo: wraps around");
    // Drain and refill several times over, so read_p and write_p each pass the
    // end of the buffer more than once.
    for (int round = 0; round < 4; round++) {
        while (fifo_read_data(&f, &out))
            ;
        CHECK(fifo_is_empty(&f));
        for (int i = 0; i < FIFO_DEPTH - 1; i++)
            CHECK(fifo_write_data(&f, tile(round * 100 + i, 0, 12)));
        CHECK(fifo_is_full(&f));
        for (int i = 0; i < FIFO_DEPTH - 1; i++) {
            CHECK(fifo_read_data(&f, &out));
            CHECK_INT(out.tile_x, round * 100 + i);
        }
    }

    SUITE("fifo: search finds queued tiles only");
    f = new_fifo();
    CHECK(fifo_write_data(&f, tile(10, 20, 12)));
    CHECK(fifo_write_data(&f, tile(11, 20, 12)));
    CHECK(fifo_search_data(&f, tile(10, 20, 12)));
    CHECK(fifo_search_data(&f, tile(11, 20, 12)));
    // Same x/y at a different zoom is a different tile.
    CHECK(!fifo_search_data(&f, tile(10, 20, 13)));
    CHECK(!fifo_search_data(&f, tile(99, 99, 12)));
    // Once read out, a tile is no longer queued.
    CHECK(fifo_read_data(&f, &out));
    CHECK(!fifo_search_data(&f, tile(10, 20, 12)));
}
