#include "map.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // mkdir + stat
#include <unistd.h>

#include <SDL2/SDL_image.h>

#include "fifo.h"
#include "log.h"

// The Stadia Maps key is optional: it is only read when -stadiamaps is passed,
// and the default OpenStreetMap tiles need no key at all. Including the header
// unconditionally meant the build failed without it even for users who would
// never use it.
#if defined(__has_include)
#if __has_include("api_key.h")
#include "api_key.h"
#define HAVE_API_KEY 1
#endif
#endif

#ifndef HAVE_API_KEY
static const char *api_key = NULL;
#endif

bool use_osm_tiles = true;
_Atomic bool download_in_progress;

bool map_has_api_key(void) {
    return api_key != NULL && api_key[0] != '\0';
}

void conv_pixel_to_tile_and_offset(int pixel_x, int pixel_y, int source_zoom, int target_zoom,
                                   int *tile_x, int *tile_y,
                                   int *pixel_in_tile_x, int *pixel_in_tile_y) {
    int zoom_diff = source_zoom - target_zoom;

    // Convert world coordinates to target zoom level by shifting right
    int scaled_x = pixel_x >> zoom_diff;
    int scaled_y = pixel_y >> zoom_diff;

    *tile_x = scaled_x / TILE_SIZE;
    *tile_y = scaled_y / TILE_SIZE;

    *pixel_in_tile_x = scaled_x % TILE_SIZE;
    *pixel_in_tile_y = scaled_y % TILE_SIZE;

    // Ensure offsets are positive
    if (*pixel_in_tile_x < 0)
        *pixel_in_tile_x += TILE_SIZE;
    if (*pixel_in_tile_y < 0)
        *pixel_in_tile_y += TILE_SIZE;
}

static void ensure_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0755);
    }
}

// Callback for curl
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *new_memory = (char *)realloc(mem->memory, mem->size + realsize + 1); // +1 for null termination
    if (new_memory == NULL)
        return 0; // realloc failed

    mem->memory = new_memory;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = '\0';

    return realsize;
}

void *download_tiles(void *arg) {
    struct fifo *download_queue = (struct fifo *)arg;
    MapTile next_tile = {0};

    // One handle for the lifetime of the thread. A fresh easy handle per tile
    // threw away the connection and its TLS session, so every tile paid for a
    // new handshake against the same host.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to initialize CURL\n");
        curl_global_cleanup();
        return NULL;
    }

    while (true) {
        download_in_progress = false;
        pthread_mutex_lock(&download_queue->lock);
        // Wait while the queue is empty, unless we have been asked to stop.
        while (fifo_is_empty(download_queue) && !download_queue->stop)
            pthread_cond_wait(&download_queue->cond, &download_queue->lock);

        if (download_queue->stop) {
            pthread_mutex_unlock(&download_queue->lock);
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            return NULL;
        }

        download_in_progress = true;
        if (!fifo_read_data(download_queue, &next_tile))
            fprintf(stderr, "Something went wrong while reading from download queue\n");

        // Publish what we are about to fetch, still under the lock. The tile is
        // no longer in the queue but its file does not exist yet, so without
        // this the main loop cannot tell it is already being fetched and
        // re-queues it on every frame until the download lands.
        download_queue->tile_in_dl = next_tile;

        pthread_mutex_unlock(&download_queue->lock);

        char tile_path[TILE_PATH_MAX];
        tile_cache_path(tile_path, sizeof(tile_path), next_tile);

        LOG_DEBUG("Start download for: %s\n", tile_path);
        // Ensure that all needed directories exist
        char zoom_dir[TILE_PATH_MAX], x_dir[TILE_PATH_MAX];
        snprintf(zoom_dir, sizeof(zoom_dir), "%s/%d", TILE_CACHE_DIR, next_tile.zoom);
        snprintf(x_dir, sizeof(x_dir), "%s/%d/%d", TILE_CACHE_DIR, next_tile.zoom, next_tile.tile_x);
        ensure_directory(TILE_CACHE_DIR);
        ensure_directory(zoom_dir);
        ensure_directory(x_dir);

        // Start the actual download
        struct MemoryStruct image_data;
        char url[TILE_PATH_MAX];
        struct curl_slist *list = NULL;

        image_data.memory = (char *)malloc(1);
        image_data.size = 0;

        // Options set on the previous tile do not carry over to this one.
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &image_data);

        if (use_osm_tiles) {
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png",
                     next_tile.zoom, next_tile.tile_x, next_tile.tile_y);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, TILE_USER_AGENT);
        } else // use stadiamaps; requires api key
        {
            snprintf(url, sizeof(url), "https://tiles.stadiamaps.com/tiles/stamen_terrain/%d/%d/%d.png",
                     next_tile.zoom, next_tile.tile_x, next_tile.tile_y);
            char auth[TILE_PATH_MAX];
            // main() refuses -stadiamaps without a key, so this cannot be
            // reached with a null one -- but the compiler cannot see that, and
            // a null here would be a format-string crash rather than a
            // failed download.
            snprintf(auth, sizeof(auth), "Authorization: Stadia-Auth %s",
                     map_has_api_key() ? api_key : "");
            list = curl_slist_append(list, auth);

            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(list); /* free the list */

        // Only cache the tile when the transfer actually succeeded. Writing a
        // failed or empty response would make file_exists() report the tile as
        // present, so it would render blank forever and never be retried.
        if (res != CURLE_OK) {
            fprintf(stderr, "Tile download failed (%s): %s\n", tile_path, curl_easy_strerror(res));
        } else if (image_data.size == 0) {
            fprintf(stderr, "Tile download returned no data: %s\n", tile_path);
        } else {
            FILE *f = fopen(tile_path, "wb");
            if (f) {
                fwrite(image_data.memory, 1, image_data.size, f);
                fclose(f);
            } else {
                fprintf(stderr, "Could not write tile to cache: %s\n", tile_path);
            }
        }
        free(image_data.memory);

        // Clear the marker: either the file now exists, or the transfer failed
        // and the tile should be eligible for queueing again.
        pthread_mutex_lock(&download_queue->lock);
        download_queue->tile_in_dl = (MapTile){.tile_x = -1, .tile_y = -1, .zoom = -1};
        pthread_mutex_unlock(&download_queue->lock);
    }
}

// Asks the download worker to return from its wait. The caller joins the
// thread afterwards; the queue's mutex and condvar must outlive that join.
void download_thread_stop(struct fifo *download_queue) {
    pthread_mutex_lock(&download_queue->lock);
    download_queue->stop = true;
    pthread_cond_signal(&download_queue->cond);
    pthread_mutex_unlock(&download_queue->lock);
}

bool tile_key_equal(MapTile a, MapTile b) {
    return a.tile_x == b.tile_x && a.tile_y == b.tile_y && a.zoom == b.zoom;
}

// Hands back the cached texture for `key`, or NULL. Touching the entry marks
// it as the most recently used, which is what keeps eviction honest.
SDL_Texture *tile_cache_lookup(TileTextureCache *cache, MapTile key) {
    for (int i = 0; i < cache->size; i++) {
        if (tile_key_equal(cache->entries[i].key, key)) {
            cache->entries[i].last_used = ++cache->clock;
            return cache->entries[i].texture;
        }
    }
    return NULL;
}

// Drops the least recently used entry. Called only when the cache is full, so
// the linear scan costs nothing next to the texture upload it makes room for.
static void evict_least_recently_used(TileTextureCache *cache) {
    int oldest = 0;
    for (int i = 1; i < cache->size; i++)
        if (cache->entries[i].last_used < cache->entries[oldest].last_used)
            oldest = i;

    SDL_DestroyTexture(cache->entries[oldest].texture);
    cache->entries[oldest] = cache->entries[cache->size - 1];
    cache->size--;
}

// Takes ownership of `texture` on success; the caller destroys it on failure.
bool tile_cache_insert(TileTextureCache *cache, MapTile key, SDL_Texture *texture) {
    // Each entry is a TILE_SIZE-square RGBA texture -- a quarter of a megabyte
    // of video memory. Growing without a bound meant a long panning session
    // consumed as much of it as the session was long.
    if (cache->size >= TILE_CACHE_MAX_ENTRIES)
        evict_least_recently_used(cache);

    if (cache->size >= cache->capacity) {
        int grown_capacity = (cache->capacity == 0) ? 64 : cache->capacity * 2;
        if (grown_capacity > TILE_CACHE_MAX_ENTRIES)
            grown_capacity = TILE_CACHE_MAX_ENTRIES;
        TileTexture *grown = realloc(cache->entries, (size_t)grown_capacity * sizeof(TileTexture));
        if (!grown) {
            // The old array is still valid and still owned by the cache, so a
            // failure here leaves the cache exactly as it was.
            fprintf(stderr, "Could not grow tile cache\n");
            return false;
        }
        cache->entries = grown;
        cache->capacity = grown_capacity;
    }

    cache->entries[cache->size++] = (TileTexture){
        .key = key, .texture = texture, .last_used = ++cache->clock};
    return true;
}

void tile_cache_free(TileTextureCache *cache) {
    for (int i = 0; i < cache->size; i++) {
        if (cache->entries[i].texture)
            SDL_DestroyTexture(cache->entries[i].texture);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->size = 0;
    cache->capacity = 0;
    cache->clock = 0;
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

// The one place the on-disk tile layout is spelled out. The download thread and
// the render loop have to agree on it exactly, or tiles are fetched forever and
// never found.
void tile_cache_path(char *out, size_t size, MapTile tile) {
    snprintf(out, size, "%s/%d/%d/%d.png", TILE_CACHE_DIR, tile.zoom, tile.tile_x, tile.tile_y);
}

// Decodes a tile PNG off disk and hands it to the cache, which takes ownership.
static SDL_Texture *load_tile_texture(struct application *appl, MapTile key, const char *path) {
    SDL_Surface *surface = IMG_Load(path);
    if (!surface)
        return NULL;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(appl->renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture)
        return NULL;

    if (!tile_cache_insert(&appl->tile_cache, key, texture)) {
        SDL_DestroyTexture(texture);
        return NULL;
    }
    return texture;
}

// The tiles covering the window at the current zoom, with where each one lands
// on screen. Returned rather than drawn, so the caller decides what is layered
// over what -- this module used to reach into the track renderer to composite
// the heat overlay itself, which put the layer order in the wrong place and
// made the map depend on the tracks.
int map_visible_tiles(const struct application *appl, VisibleTile *out, int max_tiles) {
    int tiles_x = appl->window_width / TILE_SIZE + 2;
    int tiles_y = appl->window_height / TILE_SIZE + 2;

    int center_tile_x, center_tile_y;
    int tile_offset_x, tile_offset_y;
    conv_pixel_to_tile_and_offset(appl->world_x, appl->world_y, MAX_ZOOM, appl->zoom,
                                  &center_tile_x, &center_tile_y,
                                  &tile_offset_x, &tile_offset_y);

    int tiles_at_zoom = 1 << appl->zoom;
    int count = 0;

    for (int dx = -tiles_x / 2; dx <= tiles_x / 2; dx++) {
        for (int dy = -tiles_y / 2; dy <= tiles_y / 2; dy++) {
            if (count >= max_tiles)
                return count;

            int tile_x = center_tile_x + dx;
            int tile_y = center_tile_y + dy;

            // Off the edge of the world at this zoom.
            if (tile_x < 0 || tile_y < 0 || tile_x >= tiles_at_zoom || tile_y >= tiles_at_zoom)
                continue;

            out[count++] = (VisibleTile){
                .tile = {tile_x, tile_y, appl->zoom},
                .screen_x = (tile_x - center_tile_x) * TILE_SIZE - tile_offset_x + appl->window_width / 2,
                .screen_y = (tile_y - center_tile_y) * TILE_SIZE - tile_offset_y + appl->window_height / 2};
        }
    }
    return count;
}

// Queues a tile for download unless it is already queued or in flight.
static void queue_tile_download(struct application *appl, MapTile tile) {
    pthread_mutex_lock(&appl->download_queue.lock);

    bool already_queued = fifo_search_data(&appl->download_queue, tile);
    bool already_downloading = tile_key_equal(appl->download_queue.tile_in_dl, tile);
    if (!already_queued && !already_downloading)
        fifo_write_data(&appl->download_queue, tile);

    pthread_mutex_unlock(&appl->download_queue.lock);
}

void map_draw_tiles(struct application *appl, const VisibleTile *tiles, int count) {
    for (int i = 0; i < count; i++) {
        MapTile key = tiles[i].tile;

        // Only a tile that is not already in video memory needs the filesystem
        // consulted at all.
        SDL_Texture *texture = tile_cache_lookup(&appl->tile_cache, key);
        if (!texture) {
            char tile_path[TILE_PATH_MAX];
            tile_cache_path(tile_path, sizeof(tile_path), key);

            if (file_exists(tile_path))
                texture = load_tile_texture(appl, key, tile_path);
            else
                queue_tile_download(appl, key);
        }

        if (texture) {
            SDL_Rect dest = {tiles[i].screen_x, tiles[i].screen_y, TILE_SIZE, TILE_SIZE};
            SDL_RenderCopy(appl->renderer, texture, NULL, &dest);
        }
    }
}
