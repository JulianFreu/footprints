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

// The heat ramp is a data-visualisation scale rather than UI chrome, so it is
// its own thing rather than part of the palette in colors.h: it has to stay
// perceptually ordered from cold to hot, which is a different constraint from
// looking right next to a button.
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

const char *activity_type_label(ActivityType type) {
    switch (type) {
    case Run:
        return "Run";
    case Hike:
        return "Hike";
    case Cycling:
        return "Cycling";
    case Other:
    case ACTIVITY_TYPE_COUNT:
        break;
    }
    return "Other";
}

// Releases the scratch buffer kept between frames.
void tracks_free_scratch(struct application *appl) {
    free(appl->overlay_points);
    appl->overlay_points = NULL;
    appl->overlay_points_capacity = 0;
}

// Drops everything derived from the visible set: the rendered tiles and the
// index they are rendered from. Called when the filters change, when the heat
// is recalculated, and at teardown.
void tracks_invalidate_cache(GpxCollection *collection) {
    tile_cache_free(&collection->track_tile_cache);
    point_index_invalidate(&collection->point_index);
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

    // Every point shares the same heat when the collection has no overlap at
    // all -- a single track, or tracks that never come within HEAT_RADIUS_PIXELS
    // of each other. The span is then zero, and dividing by it produced NaN,
    // which compares false against both clamps and reached the cast as
    // INT_MIN. Collapse that case onto the bottom of the ramp instead.
    const float min_heat = 1.0f;
    const float heat_span = (float)collection->max_heat - min_heat;
    for (int j = from; j < to; j++) {
        const GpxPoint *point = collection->point_index.entries[j].point;

        int tile_x, tile_y, pixel_in_tile_x, pixel_in_tile_y;
        conv_pixel_to_tile_and_offset(point->world_x, point->world_y, MAX_ZOOM, key.zoom,
                                      &tile_x, &tile_y, &pixel_in_tile_x, &pixel_in_tile_y);

        float normalized = (heat_span > 0.0f) ? ((float)point->heat - min_heat) / heat_span : 0.0f;
        if (normalized < 0.0f)
            normalized = 0.0f;
        if (normalized > 1.0f)
            normalized = 1.0f;

        int color_index = (int)(normalized * (HEAT_COLOR_COUNT - 1));
        SDL_Color color = heat_colors[color_index];

        SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);

        SDL_Rect rct = {
            pixel_in_tile_x - TRACK_POINT_SIZE / 2,
            pixel_in_tile_y - TRACK_POINT_SIZE / 2,
            TRACK_POINT_SIZE, TRACK_POINT_SIZE};
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

int find_track_near_click(GpxCollection *collection, int click_world_x, int click_world_y, int current_zoom, int max_pixel_distance) {
    int closest_track_id = -1;
    int64_t closest_distance_squared = max_pixel_distance * max_pixel_distance;

    for (int i = 0; i < collection->total_tracks; i++) {
        if (collection->tracks[i].visible_in_list) {
            GpxTrack *track = &collection->tracks[i];

            for (int j = 0; j < track->total_points; j++) {
                GpxPoint *pt = &track->points[j];

                // The radius is in screen pixels, so the distance has to be too.
                int64_t dx = (pt->world_x - click_world_x) / map_world_per_pixel_at(current_zoom);
                int64_t dy = (pt->world_y - click_world_y) / map_world_per_pixel_at(current_zoom);
                int64_t dist_squared = dx * dx + dy * dy;

                if (0 < dist_squared && dist_squared < closest_distance_squared) {
                    closest_distance_squared = dist_squared;
                    closest_track_id = track->track_id;
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

static SDL_Texture *generate_elevation_profile_texture(SDL_Renderer *renderer, const GpxTrack *track, int width, int height) {
    if (!renderer || track->total_points < 2)
        return NULL;

    SDL_Texture *texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width, height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_Texture *prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, texture);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);
    SDL_RenderClear(renderer);

    float min_elev = track->points[0].elevation;
    float max_elev = track->points[0].elevation;
    for (int i = 1; i < track->total_points; i++) {
        if (track->points[i].elevation < min_elev)
            min_elev = track->points[i].elevation;
        if (track->points[i].elevation > max_elev)
            max_elev = track->points[i].elevation;
    }
    if (max_elev == min_elev)
        max_elev += 1.0f;

    min_elev -= (max_elev - min_elev) / 10;
    max_elev += (max_elev - min_elev) / 10;

    float total_distance_m = track->points[track->total_points - 1].partial_distance;
    if (total_distance_m <= 0.0f) {
        SDL_SetRenderTarget(renderer, prev_target);
        SDL_DestroyTexture(texture);
        return NULL;
    }

    SDL_Point *polygon_points = malloc(sizeof(SDL_Point) * (track->total_points + 2));
    if (!polygon_points) {
        SDL_SetRenderTarget(renderer, prev_target);
        SDL_DestroyTexture(texture);
        return NULL;
    }

    for (int i = 0; i < track->total_points; i++) {
        int x = (int)((track->points[i].partial_distance / total_distance_m) * width);
        int y = height - (int)(((track->points[i].elevation - min_elev) / (max_elev - min_elev)) * height);
        polygon_points[i] = (SDL_Point){x, y};
    }

    // Bottom left and bottom right base points
    polygon_points[track->total_points] = (SDL_Point){polygon_points[track->total_points - 1].x, height};
    polygon_points[track->total_points + 1] = (SDL_Point){polygon_points[0].x, height};

    SDL_Color profile_fill = sdl_color(blue);
    SDL_SetRenderDrawColor(renderer, profile_fill.r, profile_fill.g, profile_fill.b, profile_fill.a);
    SDL_RenderDrawLines(renderer, polygon_points, track->total_points + 2);

    for (int i = 1; i < track->total_points; i++) {
        int x1 = (int)((track->points[i - 1].partial_distance / total_distance_m) * width);
        int y1 = height - (int)(((track->points[i - 1].elevation - min_elev) / (max_elev - min_elev)) * height);
        int x2 = (int)((track->points[i].partial_distance / total_distance_m) * width);
        int y2 = height - (int)(((track->points[i].elevation - min_elev) / (max_elev - min_elev)) * height);

        // Fill the area underneath the segment. When both samples land on the
        // same column there is nothing to interpolate across.
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

    SDL_Color profile_line = sdl_color(dark_blue);
    SDL_SetRenderDrawColor(renderer, profile_line.r, profile_line.g, profile_line.b, profile_line.a);
    SDL_RenderDrawLines(renderer, polygon_points, track->total_points);

    free(polygon_points);
    SDL_SetRenderTarget(renderer, prev_target);
    return texture;
}

// Clay draws images from an SDL_Surface, so the profile drawn on the GPU has to
// be read back into one.
static SDL_Surface *render_elevation_profile_surface(SDL_Renderer *renderer, const GpxTrack *track, int width, int height) {
    SDL_Texture *profile = generate_elevation_profile_texture(renderer, track, width, height);
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

// Regenerates the elevation profile only when the selection actually changes;
// each regeneration is two textures and a full pixel readback.
void update_track_info_graphs(struct application *appl, const GpxCollection *collection) {
    if (appl->selected_track == appl->rendered_overlay_track)
        return;
    appl->rendered_overlay_track = appl->selected_track;

    if (appl->icons.elev_profile) {
        SDL_FreeSurface(appl->icons.elev_profile);
        appl->icons.elev_profile = NULL;
    }

    if (appl->selected_track < 0 || appl->selected_track >= collection->total_tracks)
        return;

    appl->icons.elev_profile = render_elevation_profile_surface(
        appl->renderer, &collection->tracks[appl->selected_track],
        ELEVATION_PROFILE_WIDTH, ELEVATION_PROFILE_HEIGHT);
}