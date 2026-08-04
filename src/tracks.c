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
#include "settings.h"

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

// A filled circle as a triangle fan, used to round the joints and caps of a
// thick polyline.
static void draw_circle(SDL_Renderer *renderer, float cx, float cy, float radius, SDL_Color color) {
    const int segments = 24;
    SDL_Vertex verts[segments + 2];
    int indices[segments * 3];

    verts[0].position.x = cx;
    verts[0].position.y = cy;
    verts[0].color = color;
    verts[0].tex_coord.x = 0;
    verts[0].tex_coord.y = 0;

    for (int i = 0; i <= segments; i++) {
        float theta = (float)i / segments * 2.0f * (float)M_PI;
        verts[i + 1].position.x = cx + cosf(theta) * radius;
        verts[i + 1].position.y = cy + sinf(theta) * radius;
        verts[i + 1].color = color;
        verts[i + 1].tex_coord.x = 0;
        verts[i + 1].tex_coord.y = 0;

        if (i > 0) {
            int idx = (i - 1) * 3;
            indices[idx + 0] = 0;
            indices[idx + 1] = i;
            indices[idx + 2] = i + 1;
        }
    }

    SDL_RenderGeometry(renderer, NULL, verts, segments + 2, indices, segments * 3);
}

// One segment of a thick polyline, as a quad offset either side of the centre
// line.
static void draw_segment(SDL_Renderer *renderer,
                         float x1, float y1, float x2, float y2,
                         float thickness, SDL_Color color) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0)
        return;

    dx /= len;
    dy /= len;

    float ox = -dy * (thickness / 2.0f);
    float oy = dx * (thickness / 2.0f);

    SDL_Vertex verts[4];
    SDL_memset(verts, 0, sizeof(verts));

    verts[0].position.x = x1 + ox;
    verts[0].position.y = y1 + oy;
    verts[1].position.x = x1 - ox;
    verts[1].position.y = y1 - oy;
    verts[2].position.x = x2 - ox;
    verts[2].position.y = y2 - oy;
    verts[3].position.x = x2 + ox;
    verts[3].position.y = y2 + oy;

    for (int i = 0; i < 4; i++)
        verts[i].color = color;

    int indices[] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, NULL, verts, 4, indices, 6);
}

static void draw_smooth_thick_polyline(SDL_Renderer *renderer,
                                       SDL_Point *points, int count,
                                       float thickness, SDL_Color color) {
    if (count < 2)
        return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < count - 1; i++) {
        draw_segment(renderer,
                     points[i].x, points[i].y,
                     points[i + 1].x, points[i + 1].y,
                     thickness, color);

        if (i > 0) {
            draw_circle(renderer, points[i].x, points[i].y, thickness / 2.0f, color);
        }
    }

    draw_circle(renderer, points[0].x, points[0].y, thickness / 2.0f, color);
    draw_circle(renderer, points[count - 1].x, points[count - 1].y, thickness / 2.0f, color);
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

    // Read once for the whole tile rather than per point: a tile is a million
    // points on a busy map, and neither of these can change while it is being
    // rasterised.
    const int point_size = settings.track_point_size;
    const int max_heat = collection->max_heat;

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
        float normalized = heat_normalized(&settings, point->heat, max_heat);

        int color_index = (int)(normalized * (HEAT_COLOR_COUNT - 1));
        SDL_Color color = heat_colors[color_index];

        SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);

        SDL_Rect rct = {
            pixel_in_tile_x - point_size / 2,
            pixel_in_tile_y - point_size / 2,
            point_size, point_size};
        SDL_RenderFillRect(appl->renderer, &rct);
    }

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

int find_track_near_click(GpxCollection *collection, int click_world_x, int click_world_y, int current_zoom, int max_pixel_distance) {
    // Asked of the index rather than of every point there is. This used to walk
    // the whole library on each click -- over a million points, with the
    // world-per-pixel divisor recomputed inside the loop -- while the index
    // that answers exactly this question was already built for the tiles.
    if (!point_index_ensure(collection))
        return -1;

    // The radius is in screen pixels, so the distance has to be too.
    const double world_per_pixel = map_world_per_pixel_at(current_zoom);
    int closest_track_id = -1;
    double closest_distance_squared = (double)max_pixel_distance * (double)max_pixel_distance;

    int zoom = index_zoom_for_radius(current_zoom, max_pixel_distance);
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
                    closest_track_id = point->track_id;
                }
            }
        }
    }
    return closest_track_id; // will be -1 when there was no track nearby
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
    if (!track) {
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

    for (int i = 0; i < track->total_points; i++) {
        float screen_x, screen_y;
        map_world_to_screen(appl, track->points[i].world_x, track->points[i].world_y,
                            &screen_x, &screen_y);
        appl->overlay_points[i].x = (int)screen_x;
        appl->overlay_points[i].y = (int)screen_y;
    }
    SDL_Color color = sdl_color(yellow);
    draw_smooth_thick_polyline(appl->renderer, appl->overlay_points, track->total_points, SELECTED_TRACK_THICKNESS, color);

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

static SDL_Texture *generate_series_texture(SDL_Renderer *renderer, const GpxTrack *track,
                                            const TrackSeries *series, int width, int height,
                                            Clay_Color fill, Clay_Color line) {
    if (!renderer || track->total_points < 2 || !series->present ||
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

    SDL_SetRenderTarget(renderer, prev_target);
    return texture;
}

// Clay draws images from an SDL_Surface, so the graph drawn on the GPU has to
// be read back into one.
static SDL_Surface *render_series_surface(SDL_Renderer *renderer, const GpxTrack *track,
                                          const TrackSeries *series, int width, int height,
                                          Clay_Color fill, Clay_Color line) {
    SDL_Texture *profile = generate_series_texture(renderer, track, series, width, height, fill, line);
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
            SDL_FreeSurface(appl->icons.graphs[kind]);
            appl->icons.graphs[kind] = NULL;
        }
    }
}

// Regenerates the graphs only when the selection actually changes; each one is
// two textures and a full pixel readback.
//
// The series are kept alongside the pictures rather than thrown away: the
// readout under the graphs needs the numbers at the hovered point, and
// rebuilding a pace window per frame to answer that would be a pass over the
// whole track sixty times a second.
void update_track_info_graphs(struct application *appl, const GpxCollection *collection) {
    if (appl->selected_track == appl->rendered_overlay_track)
        return;
    appl->rendered_overlay_track = appl->selected_track;

    tracks_free_graphs(appl);

    if (appl->selected_track < 0 || appl->selected_track >= collection->total_tracks)
        return;

    const GpxTrack *track = &collection->tracks[appl->selected_track];
    for (int kind = 0; kind < TRACK_SERIES_COUNT; kind++) {
        if (!track_series_build(track, (TrackSeriesKind)kind, &appl->track_series[kind]))
            continue;

        appl->icons.graphs[kind] = render_series_surface(
            appl->renderer, track, &appl->track_series[kind],
            TRACK_GRAPH_WIDTH, TRACK_GRAPH_HEIGHT,
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
    if (point_index < 0 || point_index >= track->total_points)
        return;

    float screen_x, screen_y;
    map_world_to_screen(appl, track->points[point_index].world_x,
                        track->points[point_index].world_y, &screen_x, &screen_y);

    const float radius = SELECTED_TRACK_THICKNESS;
    SDL_FRect dest = map_transform_rect(appl, screen_x - radius, screen_y - radius,
                                        2.0f * radius, 2.0f * radius);

    SDL_SetRenderDrawBlendMode(appl->renderer, SDL_BLENDMODE_BLEND);
    draw_circle(appl->renderer, dest.x + dest.w / 2.0f, dest.y + dest.h / 2.0f,
                dest.w / 2.0f, sdl_color(bg0));
    draw_circle(appl->renderer, dest.x + dest.w / 2.0f, dest.y + dest.h / 2.0f,
                dest.w / 2.0f - 2.0f, sdl_color(red));
}