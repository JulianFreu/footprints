#include "tracks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>

#include "log.h"

#include "map.h" // the projection, the tile grid and the texture cache
#include "colors.h"
#include "point_index.h"
#include "render_cache.h"
#include "settings.h"
#include "ui.h" // the size the sidebar draws the graphs at

// The heat ramp is a data-visualisation scale rather than UI chrome, so it is
// its own thing rather than part of the palette in colors.h: it has to stay
// perceptually ordered from cold to hot, which is a different constraint from
// looking right next to a button.
//
// Where a given amount of heat lands on it is the settings' business, not this
// file's: heat_normalized answers that, and the ramp is read at whatever
// position it gives back.
#define HEAT_COLOR_COUNT 32

static const SDL_Color heat_colors[HEAT_COLOR_COUNT] = {
    {0, 0, 4, 255}, // dark purple
    {1, 0, 33, 255},
    {12, 1, 57, 255},
    {27, 3, 78, 255},
    {41, 6, 97, 255},
    {60, 9, 113, 255},
    {80, 13, 126, 255},
    {99, 18, 137, 255},
    {120, 24, 144, 255},
    {140, 31, 149, 255},
    {160, 39, 151, 255},
    {180, 48, 150, 255},
    {198, 58, 146, 255},
    {215, 70, 139, 255},
    {229, 84, 129, 255},
    {241, 99, 115, 255},
    {250, 115, 99, 255},
    {255, 133, 81, 255},
    {255, 152, 61, 255},
    {255, 171, 43, 255},
    {255, 190, 28, 255},
    {255, 208, 18, 255},
    {255, 225, 13, 255},
    {255, 239, 15, 255},
    {252, 250, 22, 255},
    {245, 255, 35, 255},
    {230, 255, 54, 255},
    {208, 255, 77, 255},
    {179, 255, 104, 255},
    {144, 255, 135, 255},
    {104, 255, 170, 255},
    {60, 255, 207, 255}};

SDL_Color heat_ramp_color(float normalized) {
    if (normalized < 0.0f)
        normalized = 0.0f;
    if (normalized > 1.0f)
        normalized = 1.0f;
    return heat_colors[(int)(normalized * (HEAT_COLOR_COUNT - 1))];
}

// Triangles on their way to the GPU, handed over in one call rather than one per
// shape. A thick polyline used to be a draw call for every segment and another
// for every joint, so a long track was ten thousand of them -- rebuilt on every
// frame of a pan, because where the camera is decides what the picture looks
// like.
//
// Flushed when it fills rather than sized to the longest track there could be:
// the batch only has to be large enough that the per-call overhead stops
// mattering. File-static because only the main thread draws, and because this is
// far too much to put on the stack.
#define BATCH_MAX_VERTICES 2048
#define BATCH_MAX_INDICES 6144

typedef struct GeometryBatch {
    SDL_Vertex vertices[BATCH_MAX_VERTICES];
    int indices[BATCH_MAX_INDICES];
    int vertex_count;
    int index_count;
} GeometryBatch;

static GeometryBatch batch;

static void batch_flush(SDL_Renderer *renderer) {
    if (batch.index_count == 0)
        return;

    SDL_RenderGeometry(renderer, NULL, batch.vertices, batch.vertex_count,
                       batch.indices, batch.index_count);
    batch.vertex_count = 0;
    batch.index_count = 0;
}

// Room for one more shape, made by sending what is already queued if there is
// not. Every caller reserves before it appends, so the appends themselves need
// no bounds checks.
static void batch_reserve(SDL_Renderer *renderer, int vertices, int indices) {
    if (batch.vertex_count + vertices > BATCH_MAX_VERTICES ||
        batch.index_count + indices > BATCH_MAX_INDICES)
        batch_flush(renderer);
}

static void batch_vertex(float x, float y, SDL_Color color) {
    SDL_Vertex *vertex = &batch.vertices[batch.vertex_count++];
    vertex->position.x = x;
    vertex->position.y = y;
    vertex->color = color;
    vertex->tex_coord.x = 0;
    vertex->tex_coord.y = 0;
}

// How many segments a circle of this radius is worth drawing as. A joint on a
// ten-pixel line was tessellated the same twenty-four ways as a marker twice its
// size; past about two segments per pixel of radius the extra triangles land
// inside each other.
static int circle_segments(float radius) {
    int segments = (int)(radius * 2.0f);
    if (segments < 8)
        segments = 8;
    if (segments > 24)
        segments = 24;
    return segments;
}

// A filled circle as a triangle fan, used to round the joints and caps of a
// thick polyline and to mark a point on the route.
static void batch_circle(SDL_Renderer *renderer, float cx, float cy, float radius,
                         SDL_Color color) {
    const int segments = circle_segments(radius);
    batch_reserve(renderer, segments + 2, segments * 3);

    const int center = batch.vertex_count;
    batch_vertex(cx, cy, color);

    for (int i = 0; i <= segments; i++) {
        float theta = (float)i / segments * 2.0f * (float)M_PI;
        batch_vertex(cx + cosf(theta) * radius, cy + sinf(theta) * radius, color);

        if (i > 0) {
            batch.indices[batch.index_count++] = center;
            batch.indices[batch.index_count++] = center + i;
            batch.indices[batch.index_count++] = center + i + 1;
        }
    }
}

// Two triangles over four corners, wound the same way for every quad below.
static const int quad_indices[6] = {0, 1, 2, 0, 2, 3};

static void batch_quad_indices(int first) {
    for (int i = 0; i < 6; i++)
        batch.indices[batch.index_count++] = first + quad_indices[i];
}

// One segment of a thick polyline, as a quad offset either side of the centre
// line. `dx, dy` is the segment's unit direction, which the caller has already
// worked out to decide whether the joint after it is worth drawing.
static void batch_segment(SDL_Renderer *renderer,
                          float x1, float y1, float x2, float y2,
                          float dx, float dy, float thickness, SDL_Color color) {
    const float ox = -dy * (thickness / 2.0f);
    const float oy = dx * (thickness / 2.0f);

    batch_reserve(renderer, 4, 6);
    const int first = batch.vertex_count;

    batch_vertex(x1 + ox, y1 + oy, color);
    batch_vertex(x1 - ox, y1 - oy, color);
    batch_vertex(x2 - ox, y2 - oy, color);
    batch_vertex(x2 + ox, y2 + oy, color);
    batch_quad_indices(first);
}

// An axis-aligned rectangle. The colour rides on the vertices rather than on the
// renderer, which is what lets a batch hold rectangles of many colours and still
// go out as one call -- the heat tiles are nothing but rectangles, and used to
// set a draw colour before every one of them.
static void batch_rect(SDL_Renderer *renderer, float x, float y, float w, float h,
                       SDL_Color color) {
    batch_reserve(renderer, 4, 6);
    const int first = batch.vertex_count;

    batch_vertex(x, y, color);
    batch_vertex(x + w, y, color);
    batch_vertex(x + w, y + h, color);
    batch_vertex(x, y + h, color);
    batch_quad_indices(first);
}

// How straight a turn has to be for its joint to be left out. Two quads meeting
// at an angle leave a wedge open on the outside of the turn, and that is what the
// circle at the joint is there to fill -- but the wedge is about half-thickness
// times the angle across, so below this it is a fraction of a pixel and the
// circle is a fan of triangles drawn for nothing. A watch recording a fix a
// second spends most of a track going this straight.
#define POLYLINE_STRAIGHT_DOT 0.9995f

static void draw_smooth_thick_polyline(SDL_Renderer *renderer,
                                       SDL_Point *points, int count,
                                       float thickness, SDL_Color color) {
    if (count < 2)
        return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // The direction of the segment before this one, so a joint can be judged
    // from the turn between the two without measuring either of them twice.
    float previous_dx = 0.0f, previous_dy = 0.0f;
    bool have_previous = false;

    for (int i = 0; i < count - 1; i++) {
        const float x1 = (float)points[i].x, y1 = (float)points[i].y;
        const float x2 = (float)points[i + 1].x, y2 = (float)points[i + 1].y;

        float dx = x2 - x1, dy = y2 - y1;
        const float length = sqrtf(dx * dx + dy * dy);
        if (length == 0.0f)
            continue; // nothing to draw, and nothing to join to either
        dx /= length;
        dy /= length;

        batch_segment(renderer, x1, y1, x2, y2, dx, dy, thickness, color);

        // The joint belongs to the corner between this segment and the one
        // before it, so it is drawn now that both directions are known.
        if (have_previous &&
            previous_dx * dx + previous_dy * dy < POLYLINE_STRAIGHT_DOT)
            batch_circle(renderer, x1, y1, thickness / 2.0f, color);

        previous_dx = dx;
        previous_dy = dy;
        have_previous = true;
    }

    // The caps, which are round whatever the line does at the ends.
    batch_circle(renderer, (float)points[0].x, (float)points[0].y,
                 thickness / 2.0f, color);
    batch_circle(renderer, (float)points[count - 1].x, (float)points[count - 1].y,
                 thickness / 2.0f, color);

    batch_flush(renderer);
}

// Releases the scratch buffer kept between frames.
void tracks_free_scratch(struct application *appl) {
    free(appl->overlay_points);
    appl->overlay_points = NULL;
    appl->overlay_points_capacity = 0;
}

// Drops the rendered tiles, which are what the visible set is baked into. The
// index underneath them spans every point and does not care which of them are
// shown, so it survives -- and that is what makes this cheap enough to run
// while the filters are being typed.
void tracks_invalidate_filtered_view(GpxCollection *collection) {
    tile_cache_clear(&collection->track_tile_cache);
}

// Teardown counterpart: also gives back the index's allocation.
void tracks_free_collection_cache(GpxCollection *collection) {
    tile_cache_free(&collection->track_tile_cache);
    point_index_free(&collection->point_index);
}

// The colour stamped on each pixel of the tile being rasterised, plus one so
// that zero means nothing was stamped there. Collapsing the points onto this
// first is what keeps a tile's cost to the pixels it has rather than the points
// that fall on it -- a tile at a low zoom is a million of them, and there are
// only sixty-five thousand places for them to land.
//
// Left zeroed between tiles by whoever wrote to it, so it needs no clearing on
// the way in. File-static because only the main thread draws, and 64 KB is more
// than belongs on the stack.
static uint8_t tile_heat[TILE_SIZE * TILE_SIZE];

// Amounts of heat with a ramp position remembered for them, and the entry that
// means "not worked out yet". Heat is a count of tracks overlapping at a point,
// so this covers any library whose busiest spot has fewer than this many; above
// it the answer is worked out per point, as it always was.
#define HEAT_LUT_MAX 1024
#define HEAT_LUT_NONE 0xFF

// Rasterises one heat tile and puts it in the cache. The cache is consulted by
// the caller rather than here, because it is the caller that has to tell a hit
// from a miss to keep to its budget for the frame.
static SDL_Texture *render_track_tile(struct application *appl, GpxCollection *collection, MapTile key) {
    if (!point_index_ensure(collection))
        return NULL;

    int from, to;
    point_index_tile_range(&collection->point_index, key, &from, &to);

    SDL_Texture *tex = SDL_CreateTexture(appl->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, TILE_SIZE, TILE_SIZE);
    if (!tex)
        return NULL;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(appl->renderer, tex);
    SDL_SetRenderDrawColor(appl->renderer, 0, 0, 0, 0);
    SDL_RenderClear(appl->renderer);
    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);

    // Read once for the whole tile rather than per point: a tile is a million
    // points on a busy map, and neither of these can change while it is being
    // rasterised.
    const int point_size = settings.track_point_size;
    const int max_heat = collection->max_heat;

    // Where each amount of heat lands on the ramp, filled in as it is asked for.
    // heat_normalized is a walk over the setpoints with a divide in it, and a
    // tile has far more points than there are distinct amounts of heat among
    // them. HEAT_LUT_NONE marks an answer not worked out yet.
    uint8_t heat_color[HEAT_LUT_MAX];
    memset(heat_color, HEAT_LUT_NONE, sizeof(heat_color));

    // Which colours ended up on the tile, and the band of rows they landed in.
    // Both are there so the emit below walks what was written rather than the
    // whole tile.
    uint32_t colors_present = 0;
    int first_row = TILE_SIZE, last_row = -1;

    for (int j = from; j < to; j++) {
        const GpxPoint *point = collection->point_index.entries[j].point;

        // The index spans the whole collection, so the filters are applied
        // here rather than by rebuilding it. A hidden point costs one bool.
        if (!collection->tracks[point->track_id].visible_in_list)
            continue;

        int tile_x, tile_y, pixel_in_tile_x, pixel_in_tile_y;
        conv_pixel_to_tile_and_offset(point->world_x, point->world_y, MAX_ZOOM, key.zoom,
                                      &tile_x, &tile_y, &pixel_in_tile_x, &pixel_in_tile_y);

        // Where along the ramp this much heat sits. The settings shape that
        // curve; the result is always within 0..1, so it indexes the ramp
        // without a clamp of its own.
        int color_index;
        if (point->heat >= 0 && point->heat < HEAT_LUT_MAX) {
            if (heat_color[point->heat] == HEAT_LUT_NONE)
                heat_color[point->heat] = (uint8_t)(heat_normalized(&settings, point->heat, max_heat) *
                                                    (HEAT_COLOR_COUNT - 1));
            color_index = heat_color[point->heat];
        } else {
            color_index = (int)(heat_normalized(&settings, point->heat, max_heat) *
                                (HEAT_COLOR_COUNT - 1));
        }

        // Only the hottest point on a pixel can be seen. Every ramp colour is
        // opaque and the square stamped down is the same size for all of them, so
        // two points sharing a centre pixel draw the very same square and only
        // the hotter one can matter. This is what bounds a tile at the number of
        // pixels it has rather than the million points that may fall on it.
        uint8_t *pixel = &tile_heat[pixel_in_tile_y * TILE_SIZE + pixel_in_tile_x];
        if (*pixel < (uint8_t)(color_index + 1)) {
            *pixel = (uint8_t)(color_index + 1);
            colors_present |= 1u << color_index;
            if (pixel_in_tile_y < first_row)
                first_row = pixel_in_tile_y;
            if (pixel_in_tile_y > last_row)
                last_row = pixel_in_tile_y;
        }
    }

    // Coldest first, so a hot pixel is never buried under a cold neighbour's
    // square. The order used to be whichever way the index happened to be
    // sorted, which said nothing at all.
    for (int index = 0; index < HEAT_COLOR_COUNT; index++) {
        if (!(colors_present & (1u << index)))
            continue;

        const SDL_Color color = heat_colors[index];
        const uint8_t stored = (uint8_t)(index + 1);
        for (int y = first_row; y <= last_row; y++) {
            const uint8_t *row = &tile_heat[y * TILE_SIZE];
            for (int x = 0; x < TILE_SIZE; x++) {
                if (row[x] == stored)
                    batch_rect(appl->renderer,
                               (float)(x - point_size / 2), (float)(y - point_size / 2),
                               (float)point_size, (float)point_size, color);
            }
        }
    }
    batch_flush(appl->renderer);

    // Handed back to the next tile as it was found: zeroed. Only the rows that
    // were written have to be put back.
    if (last_row >= first_row)
        memset(&tile_heat[first_row * TILE_SIZE], 0,
               (size_t)(last_row - first_row + 1) * TILE_SIZE);

    SDL_SetRenderTarget(appl->renderer, NULL);

    if (!tile_cache_insert(&collection->track_tile_cache, key, tex)) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    return tex;
}

// Draws the heat overlay over the given tiles. Layer order is the frame loop's
// business; this only knows how to draw its own layer.
void tracks_draw_heat_tiles(struct application *appl, GpxCollection *collection,
                            const VisibleTile *tiles, int count) {
    int renders_left = HEAT_RENDERS_PER_FRAME;

    for (int i = 0; i < count; i++) {
        SDL_Texture *texture = tile_cache_lookup(&collection->track_tile_cache, tiles[i].tile);

        if (!texture) {
            // Rasterising a tile walks its whole slice of the point index, and
            // a frame drawn at half scale asks for four times as many tiles at
            // once -- which would land on the first frame of a zoom, where it
            // shows worst. Rationed like the map's decodes. There is no
            // ancestor to fall back on here, so a tile past the budget is
            // simply absent until the frame that gets to it.
            if (renders_left <= 0) {
                app_request_redraw(appl); // come back for the rest
                continue;
            }

            texture = render_track_tile(appl, collection, tiles[i].tile);
            renders_left--;
            if (!texture)
                continue;
        }

        SDL_FRect dest = {tiles[i].screen_x, tiles[i].screen_y,
                          tiles[i].size, tiles[i].size};
        SDL_RenderCopyF(appl->renderer, texture, NULL, &dest);
    }
}

// The polyline for the selected track, rendered once per zoom level and drawn
// over the whole window.
void tracks_draw_selected_overlay(struct application *appl) {
    if (!appl->selected_track_overlay)
        return;

    // Drawn in screen space for the model's zoom, so it has to be carried by
    // the same transform as the tiles underneath it or the track slides off
    // the map while the picture is catching up.
    SDL_FRect dest = map_transform_rect(appl, 0.0f, 0.0f,
                                        (float)appl->window_width, (float)appl->window_height);
    SDL_RenderCopyF(appl->renderer, appl->selected_track_overlay, NULL, &dest);
}

// The zoom whose tiles are the tightest fit around a click radius: deep enough
// that a tile is small, shallow enough that one still covers the radius, so the
// ring of nine around the click is certain to hold every candidate.
static int index_zoom_for_radius(int current_zoom, int max_pixel_distance) {
    int zoom = current_zoom;
    // A tile at `zoom` spans TILE_SIZE >> (zoom - current_zoom) screen pixels.
    while (zoom < MAX_ZOOM &&
           (TILE_SIZE >> (zoom + 1 - current_zoom)) >= max_pixel_distance)
        zoom++;
    return zoom;
}

const GpxPoint *tracks_point_at_screen(const struct application *appl, GpxCollection *collection,
                                       int screen_x, int screen_y, int max_pixel_distance) {
    // A cursor landing while a zoom is still easing is on a picture the model
    // has already moved past, so it is put back into the model's own space
    // before being unprojected. Done here rather than at each call site: both
    // the click and the heat readout start from the same pixel, and the two
    // must not be able to disagree about what is under it.
    float unscaled_x, unscaled_y;
    map_screen_untransform(appl, (float)screen_x, (float)screen_y, &unscaled_x, &unscaled_y);

    int click_world_x, click_world_y;
    map_screen_to_world(appl, unscaled_x, unscaled_y, &click_world_x, &click_world_y);

    // Asked of the index rather than of every point there is. This used to walk
    // the whole library on each click -- over a million points, with the
    // world-per-pixel divisor recomputed inside the loop -- while the index
    // that answers exactly this question was already built for the tiles.
    if (!point_index_ensure(collection))
        return NULL;

    // The radius is in screen pixels, so the distance has to be too.
    const double world_per_pixel = map_world_per_pixel_at(appl->zoom);
    const GpxPoint *closest = NULL;
    double closest_distance_squared = (double)max_pixel_distance * (double)max_pixel_distance;

    int zoom = index_zoom_for_radius(appl->zoom, max_pixel_distance);
    int centre_x, centre_y, offset_x, offset_y;
    conv_pixel_to_tile_and_offset(click_world_x, click_world_y, MAX_ZOOM, zoom,
                                  &centre_x, &centre_y, &offset_x, &offset_y);

    // The click sits somewhere inside the middle tile, so the ring around it is
    // what carries the rest of the radius however close to an edge it landed.
    for (int tile_y = centre_y - 1; tile_y <= centre_y + 1; tile_y++) {
        for (int tile_x = centre_x - 1; tile_x <= centre_x + 1; tile_x++) {
            if (tile_x < 0 || tile_y < 0)
                continue;

            int from, to;
            point_index_tile_range(&collection->point_index,
                                   (MapTile){.tile_x = tile_x, .tile_y = tile_y, .zoom = zoom},
                                   &from, &to);

            for (int i = from; i < to; i++) {
                const GpxPoint *point = collection->point_index.entries[i].point;
                // The index spans every point, so what is filtered out is
                // skipped here rather than left out of it.
                if (!collection->tracks[point->track_id].visible_in_list)
                    continue;

                double dist_x = (double)(point->world_x - click_world_x) / world_per_pixel;
                double dist_y = (double)(point->world_y - click_world_y) / world_per_pixel;
                double dist_squared = dist_x * dist_x + dist_y * dist_y;

                // Inclusive, so a click landing exactly on a point selects its
                // track. The old test excluded a distance of zero and so threw
                // away the most direct hit there is.
                if (dist_squared <= closest_distance_squared) {
                    closest_distance_squared = dist_squared;
                    closest = point;
                }
            }
        }
    }
    return closest; // NULL when there was no track nearby
}

static bool overlay_key_equal(const OverlayKey *a, const OverlayKey *b) {
    return a->track == b->track && a->world_x == b->world_x && a->world_y == b->world_y &&
           a->zoom == b->zoom && a->window_width == b->window_width &&
           a->window_height == b->window_height;
}

static void overlay_discard(struct application *appl) {
    if (appl->selected_track_overlay) {
        SDL_DestroyTexture(appl->selected_track_overlay);
        appl->selected_track_overlay = NULL;
    }
    appl->overlay_key = (OverlayKey){.track = -1};
}

void update_selected_track_overlay(struct application *appl, GpxCollection *collection) {
    if (appl->selected_track < 0 || appl->selected_track >= collection->total_tracks) {
        overlay_discard(appl);
        return;
    }

    const OverlayKey key = {
        .track = appl->selected_track,
        .world_x = appl->world_x,
        .world_y = appl->world_y,
        .zoom = appl->zoom,
        .window_width = appl->window_width,
        .window_height = appl->window_height};

    // The polyline is only redrawn when the picture it would produce has
    // changed. This used to be rebuilt on every frame the selection was set,
    // which was a window-sized texture allocated and thrown away sixty times a
    // second as soon as anything else kept the loop drawing.
    if (appl->selected_track_overlay && overlay_key_equal(&key, &appl->overlay_key))
        return;

    // A texture cannot be resized, so a window that changed shape needs a new
    // one. Otherwise the existing one is cleared and drawn over.
    if (appl->selected_track_overlay &&
        (key.window_width != appl->overlay_key.window_width ||
         key.window_height != appl->overlay_key.window_height))
        overlay_discard(appl);

    if (!appl->selected_track_overlay) {
        appl->selected_track_overlay = SDL_CreateTexture(appl->renderer,
                                                         SDL_PIXELFORMAT_RGBA8888,
                                                         SDL_TEXTUREACCESS_TARGET,
                                                         appl->window_width,
                                                         appl->window_height);
        if (!appl->selected_track_overlay) {
            SDL_Log("Failed to create overlay texture: %s", SDL_GetError());
            return;
        }
        SDL_SetTextureBlendMode(appl->selected_track_overlay, SDL_BLENDMODE_BLEND);
    }

    SDL_Texture *overlay = appl->selected_track_overlay;
    SDL_SetRenderTarget(appl->renderer, overlay);
    SDL_SetRenderDrawColor(appl->renderer, 0, 0, 0, 0);
    SDL_RenderClear(appl->renderer);

    SDL_SetRenderDrawColor(appl->renderer, 255, 255, 0, 255);

    GpxTrack *track = NULL;
    for (int i = 0; i < collection->total_tracks; i++) {
        if (collection->tracks[i].track_id == appl->selected_track) {
            track = &collection->tracks[i];
            break;
        }
    }
    // A track with no path has nothing to trace: every point of it would
    // project to the same place, drawing a dot on Null Island.
    if (!track || !track->has_path) {
        SDL_SetRenderTarget(appl->renderer, NULL);
        overlay_discard(appl);
        return;
    }

    // This was a stack VLA sized by the track's point count, which puts
    // hundreds of kilobytes on the stack for a long recording.
    if (track->total_points > appl->overlay_points_capacity) {
        SDL_Point *grown = realloc(appl->overlay_points, track->total_points * sizeof(SDL_Point));
        if (!grown) {
            SDL_SetRenderTarget(appl->renderer, NULL);
            overlay_discard(appl);
            return;
        }
        appl->overlay_points = grown;
        appl->overlay_points_capacity = track->total_points;
    }

    // Points landing on a screen pixel already taken by the one before them are
    // dropped. A watch records a fix a second, and at anything but the deepest
    // zoom that is repeatedly the same pixel: the segment between two of them has
    // no length and draws nothing, while still costing a joint. What is left is
    // the same picture from a fraction of the geometry.
    int count = 0;
    for (int i = 0; i < track->total_points; i++) {
        float screen_x, screen_y;
        map_world_to_screen(appl, track->points[i].world_x, track->points[i].world_y,
                            &screen_x, &screen_y);

        const int x = (int)screen_x, y = (int)screen_y;
        if (count > 0 && x == appl->overlay_points[count - 1].x &&
            y == appl->overlay_points[count - 1].y)
            continue;

        appl->overlay_points[count].x = x;
        appl->overlay_points[count].y = y;
        count++;
    }
    SDL_Color color = sdl_color(yellow);
    draw_smooth_thick_polyline(appl->renderer, appl->overlay_points, count, SELECTED_TRACK_THICKNESS, color);

    SDL_SetRenderTarget(appl->renderer, NULL);

    // Recorded only now: every path that gives up above leaves the key alone,
    // so the next frame tries again rather than trusting an empty texture.
    appl->overlay_key = key;
}

// Where one sample sits in the graph's box, vertically. Pace is drawn upside
// down relative to the others: the quickest kilometre belongs at the top, and
// it is the smallest number.
static int series_y(const TrackSeries *series, float value, float min, float max, int height) {
    float fraction = (value - min) / (max - min);
    if (series->invert)
        fraction = 1.0f - fraction;
    return height - (int)(fraction * height);
}

// The rules measuring the graphs, shared by all three kinds rather than tinted
// per series: they are the ruler held against the curve, not a fourth thing
// drawn beside it. Translucent so the fill they cross still reads as filled.
static const Clay_Color series_grid_color = {0x92, 0x83, 0x74, 0x60};

// The value written against a rule, in the rule's own grey. Nearly opaque where
// the rule is barely there: a line only has to be followed across the graph,
// but a number has to be read off a saturated fill.
static const Clay_Color series_label_color = {0x92, 0x83, 0x74, 0xd0};

// One rule's value, at the left edge of the graph and sitting on its line.
// Drawn into whatever render target is current, which is the graph's own
// texture -- the caller is midway through drawing it.
static void draw_grid_label(SDL_Renderer *renderer, TTF_Font *font, const char *text,
                            int rule_y) {
    SDL_Color color = sdl_color(series_label_color);
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface)
        return;

    // Above the rule, or not at all. The range carries only a tenth of its span
    // as headroom either side, so the topmost rule can sit within a line of text
    // of the top edge -- and a label slid down to fit would come to rest against
    // the rule below it, naming the wrong one.
    int y = rule_y - surface->h - TRACK_GRAPH_LABEL_PADDING;
    if (y >= 0) {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture) {
            SDL_Rect where = {TRACK_GRAPH_LABEL_PADDING, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, texture, NULL, &where);
            SDL_DestroyTexture(texture);
        }
    }

    SDL_FreeSurface(surface);
}

static SDL_Texture *generate_series_texture(SDL_Renderer *renderer, TTF_Font *font,
                                            const GpxTrack *track,
                                            const TrackSeries *series, int width, int height,
                                            Clay_Color fill, Clay_Color line) {
    // The x axis of every one of these graphs is the distance covered, which a
    // track with no path has none of: its total is a number its import was
    // told, not a series measured along the way.
    if (!renderer || !track->has_path || track->total_points < 2 || !series->present ||
        series->count != track->total_points)
        return NULL;

    SDL_Texture *texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width, height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_Texture *prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, texture);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);
    SDL_RenderClear(renderer);

    // Not the samples' own range: what a graph is drawn over is the series'
    // business, and it has a floor so that a run held at one pace does not come
    // out as the noise on a flat line stretched to the height of the box.
    float min_value, max_value;
    track_series_range(series, &min_value, &max_value);

    float total_distance_m = track->points[track->total_points - 1].partial_distance;
    if (total_distance_m <= 0.0f) {
        SDL_SetRenderTarget(renderer, prev_target);
        SDL_DestroyTexture(texture);
        return NULL;
    }

    SDL_Color fill_color = sdl_color(fill);
    SDL_SetRenderDrawColor(renderer, fill_color.r, fill_color.g, fill_color.b, fill_color.a);

    // The area under the curve, a column at a time. A sample the series has no
    // value for leaves its columns empty rather than being interpolated across:
    // a heart rate monitor that dropped out for a minute is a gap in the graph,
    // not a straight line at whatever it last read.
    for (int i = 1; i < track->total_points; i++) {
        if (isnan(series->values[i - 1]) || isnan(series->values[i]))
            continue;

        int x1 = (int)((track->points[i - 1].partial_distance / total_distance_m) * width);
        int y1 = series_y(series, series->values[i - 1], min_value, max_value, height);
        int x2 = (int)((track->points[i].partial_distance / total_distance_m) * width);
        int y2 = series_y(series, series->values[i], min_value, max_value, height);

        // When both samples land on the same column there is nothing to
        // interpolate across.
        if (x2 == x1) {
            SDL_RenderDrawLine(renderer, x1, y2, x1, height);
            continue;
        }
        for (int x = x1; x <= x2; x++) {
            float t = (float)(x - x1) / (float)(x2 - x1);
            int y = (int)((1 - t) * y1 + t * y2);
            SDL_RenderDrawLine(renderer, x, y, x, height);
        }
    }

    // The curve itself, over the top of its own fill. Drawn segment by segment
    // for the same reason the fill is: SDL_RenderDrawLines would join the two
    // sides of a gap.
    SDL_Color line_color = sdl_color(line);
    SDL_SetRenderDrawColor(renderer, line_color.r, line_color.g, line_color.b, line_color.a);
    for (int i = 1; i < track->total_points; i++) {
        if (isnan(series->values[i - 1]) || isnan(series->values[i]))
            continue;

        SDL_RenderDrawLine(renderer,
                           (int)((track->points[i - 1].partial_distance / total_distance_m) * width),
                           series_y(series, series->values[i - 1], min_value, max_value, height),
                           (int)((track->points[i].partial_distance / total_distance_m) * width),
                           series_y(series, series->values[i], min_value, max_value, height));
    }

    // The rules across the round values, over both the fill and the curve so
    // that one reads the whole way across rather than only where the graph
    // happens to be empty. Placed through series_y, the same mapping the curve
    // is drawn through, so a rule at 150m meets the curve exactly where it
    // crosses 150m -- and pace, drawn upside down, needs no second thought.
    //
    // The blend mode is set here rather than inherited: nothing else in this
    // function needs one, its colours being opaque, and a translucent rule
    // drawn without it would come out solid.
    float grid[TRACK_SERIES_GRID_MAX];
    int rules = track_series_grid_lines(series->kind, min_value, max_value, height,
                                        grid, TRACK_SERIES_GRID_MAX);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color grid_color = sdl_color(series_grid_color);
    SDL_SetRenderDrawColor(renderer, grid_color.r, grid_color.g, grid_color.b, grid_color.a);

    // A graph with no room for a line of text is left as bare rules rather than
    // being written over: the height has a floor of a few pixels under it, and
    // labels stacked on top of each other say less than none at all.
    bool label = font && height >= 3 * TRACK_GRAPH_LABEL_FONT_SIZE;
    if (label)
        TTF_SetFontSize(font, TRACK_GRAPH_LABEL_FONT_SIZE);

    for (int i = 0; i < rules; i++) {
        int y = series_y(series, grid[i], min_value, max_value, height);
        SDL_RenderDrawLine(renderer, 0, y, width - 1, y);

        // Written from the same y the rule was drawn at, so the number and the
        // line it names cannot come apart -- pace, drawn upside down, ends up
        // counting downwards without anything here knowing that it does.
        if (label) {
            char text[TRACK_SERIES_TEXT_MAX];
            draw_grid_label(renderer, font,
                            track_series_format(series->kind, grid[i], text, sizeof(text)), y);
        }
    }

    SDL_SetRenderTarget(renderer, prev_target);
    return texture;
}

// Clay draws images from an SDL_Surface, so the graph drawn on the GPU has to
// be read back into one.
static SDL_Surface *render_series_surface(SDL_Renderer *renderer, TTF_Font *font,
                                          const GpxTrack *track,
                                          const TrackSeries *series, int width, int height,
                                          Clay_Color fill, Clay_Color line) {
    SDL_Texture *profile = generate_series_texture(renderer, font, track, series, width, height, fill, line);
    if (!profile)
        return NULL;

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        SDL_Log("Failed to create surface: %s", SDL_GetError());
        SDL_DestroyTexture(profile);
        return NULL;
    }

    SDL_Texture *prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, profile);
    SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, surface->pixels, surface->pitch);
    SDL_SetRenderTarget(renderer, prev_target);
    SDL_DestroyTexture(profile);

    return surface;
}

// The two colours each graph is drawn in, indexed by TrackSeriesKind. Three
// hues rather than one, so a glance at the sidebar tells the three apart before
// their labels are read.
static const struct {
    Clay_Color fill;
    Clay_Color line;
} series_colors[TRACK_SERIES_COUNT] = {
    [TRACK_SERIES_ELEVATION] = {blue, dark_blue},
    [TRACK_SERIES_PACE] = {aqua, dark_aqua},
    [TRACK_SERIES_HEART_RATE] = {red, dark_red},
};

// Releases the series and the pictures made from them.
void tracks_free_graphs(struct application *appl) {
    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        track_series_free(&appl->track_series[kind]);
        if (appl->icons.graphs[kind]) {
            // The uploaded copy is keyed by this surface's address, and the next
            // selection's graph may well be handed the same one back.
            render_cache_forget_surface(appl->icons.graphs[kind]);
            SDL_FreeSurface(appl->icons.graphs[kind]);
            appl->icons.graphs[kind] = NULL;
        }
    }
}

// Regenerates the graphs only when the picture they would produce has changed;
// each one is two textures and a full pixel readback.
//
// That is the selection, and the size the sidebar will draw them at -- the
// graphs fill the panel down to its bottom edge, so a window resized taller
// wants taller pictures. Rasterising at the size they are drawn at is what
// keeps them at their own scale instead of scaled into their box; the size
// itself is the layout's to decide, which is why it is asked for rather than
// worked out here. A resize already costs the selected track's whole overlay
// texture, so three graphs alongside it is in keeping.
//
// The series are kept alongside the pictures rather than thrown away: the
// readout under the graphs needs the numbers at the hovered point, and
// rebuilding a pace window per frame to answer that would be a pass over the
// whole track sixty times a second.
void update_track_info_graphs(struct application *appl, const GpxCollection *collection) {
    int graph_width, graph_height;
    ui_sidebar_graph_size(appl, &graph_width, &graph_height);

    if (appl->selected_track == appl->rendered_overlay_track &&
        graph_width == appl->rendered_graph_width &&
        graph_height == appl->rendered_graph_height)
        return;

    appl->rendered_overlay_track = appl->selected_track;
    appl->rendered_graph_width = graph_width;
    appl->rendered_graph_height = graph_height;

    tracks_free_graphs(appl);

    if (appl->selected_track < 0 || appl->selected_track >= collection->total_tracks)
        return;

    const GpxTrack *track = &collection->tracks[appl->selected_track];
    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        if (!track_series_build(track, (TrackSeriesKind)kind, &appl->track_series[kind]))
            continue;

        // A font of its own rather than one the UI is drawing with: this sets a
        // size on it, and a UI font found at a size other than its own would be
        // resized on every element again.
        appl->icons.graphs[kind] = render_series_surface(
            appl->renderer, appl->fonts[UI_FONT_GRAPH_LABEL].font, track,
            &appl->track_series[kind], graph_width, graph_height,
            series_colors[kind].fill, series_colors[kind].line);
    }
}

// The dot on the route, at the point a graph is being hovered over.
//
// Carried by the same transform as the polyline underneath it: both are drawn
// in screen space for the model's zoom, so a marker placed without it would
// slide off the track for as long as a zoom was still easing.
void tracks_draw_point_marker(struct application *appl, const GpxCollection *collection,
                              int track_id, int point_index) {
    if (track_id < 0 || track_id >= collection->total_tracks)
        return;

    const GpxTrack *track = &collection->tracks[track_id];
    // has_path as well as the bounds: a point that never carried coordinates
    // would put the marker on Null Island rather than on the track it belongs
    // to, which is nowhere near where the pointer is.
    if (!track->has_path || point_index < 0 || point_index >= track->total_points)
        return;

    float screen_x, screen_y;
    map_world_to_screen(appl, track->points[point_index].world_x,
                        track->points[point_index].world_y, &screen_x, &screen_y);

    const float radius = SELECTED_TRACK_THICKNESS;
    SDL_FRect dest = map_transform_rect(appl, screen_x - radius, screen_y - radius,
                                        2.0f * radius, 2.0f * radius);

    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);
    batch_circle(appl->renderer, dest.x + dest.w / 2.0f, dest.y + dest.h / 2.0f,
                 dest.w / 2.0f, sdl_color(bg0));
    batch_circle(appl->renderer, dest.x + dest.w / 2.0f, dest.y + dest.h / 2.0f,
                 dest.w / 2.0f - 2.0f, sdl_color(red));
    batch_flush(appl->renderer);
}