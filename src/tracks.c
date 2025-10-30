#include "tracks.h"

#define HEAT_COLOR_COUNT 32

SDL_Color heat_colors[HEAT_COLOR_COUNT] = {
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

// Helper: draw a filled circle using triangles (smooth joint cap)
static void draw_circle(SDL_Renderer *renderer, float cx, float cy, float radius, SDL_Color color)
{
    const int segments = 24;
    SDL_Vertex verts[segments + 2];
    int indices[segments * 3];

    verts[0].position.x = cx;
    verts[0].position.y = cy;
    verts[0].color = color;
    verts[0].tex_coord.x = 0;
    verts[0].tex_coord.y = 0;

    for (int i = 0; i <= segments; i++)
    {
        float theta = (float)i / segments * 2.0f * (float)M_PI;
        verts[i + 1].position.x = cx + cosf(theta) * radius;
        verts[i + 1].position.y = cy + sinf(theta) * radius;
        verts[i + 1].color = color;
        verts[i + 1].tex_coord.x = 0;
        verts[i + 1].tex_coord.y = 0;

        if (i > 0)
        {
            int idx = (i - 1) * 3;
            indices[idx + 0] = 0;
            indices[idx + 1] = i;
            indices[idx + 2] = i + 1;
        }
    }

    SDL_RenderGeometry(renderer, NULL, verts, segments + 2, indices, segments * 3);
}

// Draw a single thick line segment
static void draw_segment(SDL_Renderer *renderer,
                         float x1, float y1, float x2, float y2,
                         float thickness, SDL_Color color)
{
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

// Draw a smooth, thick polyline connecting many points
void draw_smooth_thick_polyline(SDL_Renderer *renderer,
                                SDL_Point *points, int count,
                                float thickness, SDL_Color color)
{
    if (count < 2)
        return;

    // Enable alpha blending
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < count - 1; i++)
    {
        draw_segment(renderer,
                     points[i].x, points[i].y,
                     points[i + 1].x, points[i + 1].y,
                     thickness, color);

        // Round joint at each point (except first)
        if (i > 0)
        {
            draw_circle(renderer, points[i].x, points[i].y, thickness / 2.0f, color);
        }
    }

    // Round caps at the ends
    draw_circle(renderer, points[0].x, points[0].y, thickness / 2.0f, color);
    draw_circle(renderer, points[count - 1].x, points[count - 1].y, thickness / 2.0f, color);
}

void append_to_track_tile_cache(track_tile_texture_cache *cache, track_tile_texture entry)
{
    if (cache->size >= cache->capacity)
    {
        cache->capacity = cache->capacity == 0 ? 64 : cache->capacity * 2;
        cache->entries = realloc(cache->entries, cache->capacity * sizeof(track_tile_texture));
    }
    cache->entries[cache->size++] = entry;
}

void free_track_tile_cache(track_tile_texture_cache *cache)
{
    for (int i = 0; i < cache->size; i++)
    {
        if (cache->entries[i].texture)
            SDL_DestroyTexture(cache->entries[i].texture);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->size = 0;
    cache->capacity = 0;
}

SDL_Texture *get_or_render_track_tile(struct application *appl, GpxCollection *collection, MapTile key)
{
    // Check if already cached
    for (int i = 0; i < collection->track_tile_cache.size; i++)
    {
        if (tile_key_equal(collection->track_tile_cache.entries[i].key, key))
        {
            return collection->track_tile_cache.entries[i].texture;
        }
    }

    // Sammle Punkte aller Tracks für dieses Tile
    CombinedTilePoints ctp = {
        .key = key,
        .points = NULL,
        .point_count = 0,
        .capacity = 0};

    for (int t = 0; t < collection->total_tracks; t++)
    {
        if (collection->tracks[t].visible_in_list)
        {
            GpxTrack *track = &collection->tracks[t];
            for (int i = 0; i < track->total_points; i++)
            {
                int world_x = track->points[i].world_x;
                int world_y = track->points[i].world_y;
                int tile_x, tile_y, pixel_in_tile_x, pixel_in_tile_y;
                pixelToTileAndOffset(world_x, world_y, MAX_ZOOM, key.zoom, &tile_x, &tile_y, &pixel_in_tile_x, &pixel_in_tile_y);

                if (tile_x == key.tile_x && tile_y == key.tile_y)
                {

                    if (ctp.point_count >= ctp.capacity)
                    {
                        ctp.capacity = ctp.capacity == 0 ? 16 : ctp.capacity * 2;
                        ctp.points = realloc(ctp.points, ctp.capacity * sizeof(HeatPoint));
                    }
                    HeatPoint hp = {
                        .pos = {pixel_in_tile_x, pixel_in_tile_y},
                        .heat = track->points[i].heat};

                    ctp.points[ctp.point_count++] = hp;
                    //                    ctp.points[ctp.point_count++] = (SDL_Point){pixel_in_tile_x, pixel_in_tile_y};
                }
            }
        }
    }

    // Rendern
    SDL_Texture *tex = SDL_CreateTexture(appl->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 256, 256);
    if (!tex)
    {
        free(ctp.points);
        return NULL;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(appl->renderer, tex);
    SDL_SetRenderDrawColor(appl->renderer, 0, 0, 0, 0);
    SDL_RenderClear(appl->renderer);

    int red = 255;
    int green = 0;
    int blue = 0;
    float max_heat = (float)collection->max_heat;
    float min_heat = 1.0;
    for (int j = 0; j < ctp.point_count; j++)
    {
        float heat = (float)ctp.points[j].heat;

        // Normalize heat (replace 0.0 and 1.0 with your actual range if needed)
        float normalized = (heat - min_heat) / (max_heat - min_heat);
        if (normalized < 0.0f)
            normalized = 0.0f;
        if (normalized > 1.0f)
            normalized = 1.0f;

        // Map to index
        int color_index = (int)(normalized * (HEAT_COLOR_COUNT - 1));
        SDL_Color color = heat_colors[color_index];

        SDL_SetRenderDrawColor(appl->renderer, color.r, color.g, color.b, color.a);

        SDL_Rect rct = {
            ctp.points[j].pos.x - 2,
            ctp.points[j].pos.y - 2,
            4, 4};
        SDL_RenderFillRect(appl->renderer, &rct);
    }

    SDL_SetRenderTarget(appl->renderer, NULL);

    // Cachen
    track_tile_texture entry = {
        .key = key,
        .texture = tex,
        .valid = true};
    append_to_track_tile_cache(&collection->track_tile_cache, entry);

    free(ctp.points);
    return tex;
}

int find_track_near_click(GpxCollection *collection, int click_world_x, int click_world_y, int current_zoom, int max_pixel_distance)
{
    int closest_track_id = -1;
    int64_t closest_distance_squared = max_pixel_distance * max_pixel_distance;

    for (int i = 0; i < collection->total_tracks; i++)
    {
        if (collection->tracks[i].visible_in_list)
        {
            GpxTrack *track = &collection->tracks[i];

            for (int j = 0; j < track->total_points; j++)
            {
                GpxPoint *pt = &track->points[j];

                int64_t dx = (pt->world_x - click_world_x) >> (MAX_ZOOM - current_zoom);
                int64_t dy = (pt->world_y - click_world_y) >> (MAX_ZOOM - current_zoom);
                int64_t dist_squared = dx * dx + dy * dy;

                if (0 < dist_squared && dist_squared < closest_distance_squared)
                {
                    closest_distance_squared = dist_squared;
                    closest_track_id = track->track_id;
                }
            }
        }
    }
    return closest_track_id; // -1 wenn kein Track nahe genug war
}

void update_selected_track_overlay(struct application *appl, GpxCollection *collection)
{
    int zoom = appl->zoom;
    if (appl->selected_track < 0 || appl->selected_track >= collection->total_tracks)
    {
        if (appl->selected_track_overlay[zoom])
        {
            SDL_DestroyTexture(appl->selected_track_overlay[zoom]);
            appl->selected_track_overlay[zoom] = NULL;
        }
        return;
    }

    // Falls alte Textur existiert, freigeben
    if (appl->selected_track_overlay[zoom])
    {
        SDL_DestroyTexture(appl->selected_track_overlay[zoom]);
        appl->selected_track_overlay[zoom] = NULL;
    }

    // Neue transparente Textur erstellen
    SDL_Texture *overlay = SDL_CreateTexture(appl->renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             appl->window_width,
                                             appl->window_height);

    if (!overlay)
    {
        SDL_Log("Fehler beim Erstellen der Overlay-Textur: %s", SDL_GetError());
        return;
    }

    SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(appl->renderer, overlay);
    SDL_SetRenderDrawColor(appl->renderer, 0, 0, 0, 0); // Transparent
    SDL_RenderClear(appl->renderer);

    SDL_SetRenderDrawColor(appl->renderer, 255, 255, 0, 255); // Gelb

    // Track finden
    GpxTrack *track = NULL;
    for (int i = 0; i < collection->total_tracks; i++)
    {
        if (collection->tracks[i].track_id == appl->selected_track)
        {
            track = &collection->tracks[i];
            break;
        }
    }
    if (!track)
        return;

    int zoom_factor = 1 << (MAX_ZOOM - zoom);

    SDL_Point pts[track->total_points];
    for (int i = 0; i < track->total_points; i++)
    {
        pts[i].x = ((track->points[i].world_x - appl->world_x) / zoom_factor) + (appl->window_width / 2);
        pts[i].y = ((track->points[i].world_y - appl->world_y) / zoom_factor) + (appl->window_height / 2);
    }
    SDL_Color color = {.a = 255, .r = 255, .g = 255, .b = 0};
    draw_smooth_thick_polyline(appl->renderer, pts, track->total_points, 10.0f, color);

    // Zurück zum normalen Render-Target (Fenster)
    SDL_SetRenderTarget(appl->renderer, NULL);

    appl->selected_track_overlay[zoom] = overlay;
}

SDL_Texture *generate_elevation_profile_texture(SDL_Renderer *renderer, const GpxTrack track, int width, int height)
{
    if (!renderer || track.total_points < 2)
        return NULL;

    SDL_Texture *texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width, height);
    if (!texture)
    {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_Texture *prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, texture);

    // Hintergrund löschen
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0); // Transparent
    SDL_RenderClear(renderer);

    // Höhenbereich ermitteln
    float min_elev = track.points[0].elevation;
    float max_elev = track.points[0].elevation;
    for (int i = 1; i < track.total_points; i++)
    {
        if (track.points[i].elevation < min_elev)
            min_elev = track.points[i].elevation;
        if (track.points[i].elevation > max_elev)
            max_elev = track.points[i].elevation;
    }
    if (max_elev == min_elev)
        max_elev += 1.0f;

    min_elev -= (max_elev - min_elev)/10;
    max_elev += (max_elev - min_elev)/10;

    float total_distance_m = track.points[track.total_points - 1].partial_distance;

    // Punkte fürs Polygon vorbereiten
    SDL_Point *polygon_points = malloc(sizeof(SDL_Point) * (track.total_points + 2));
    if (!polygon_points)
    {
        SDL_SetRenderTarget(renderer, prev_target);
        SDL_DestroyTexture(texture);
        return NULL;
    }

    for (int i = 0; i < track.total_points; i++)
    {
        int x = (int)((track.points[i].partial_distance / total_distance_m) * width);
        int y = height - (int)(((track.points[i].elevation - min_elev) / (max_elev - min_elev)) * height);
        polygon_points[i] = (SDL_Point){x, y};
    }

    // Zwei weitere Punkte: rechts unten & links unten (Basislinie)
    polygon_points[track.total_points] = (SDL_Point){polygon_points[track.total_points - 1].x, height};
    polygon_points[track.total_points + 1] = (SDL_Point){polygon_points[0].x, height};

    // Flächenfüllung zeichnen (z.B. hellblau)
    SDL_SetRenderDrawColor(renderer, 150, 200, 255, 255); // Füllfarbe
    SDL_RenderDrawLines(renderer, polygon_points, track.total_points + 2);

    for (int i = 1; i < track.total_points; i++)
    {
        int x1 = (int)((track.points[i - 1].partial_distance / total_distance_m) * width);
        int y1 = height - (int)(((track.points[i - 1].elevation - min_elev) / (max_elev - min_elev)) * height);
        int x2 = (int)((track.points[i].partial_distance / total_distance_m) * width);
        int y2 = height - (int)(((track.points[i].elevation - min_elev) / (max_elev - min_elev)) * height);

        // Fläche zwischen den Punkten zeichnen als "Dreieck + Rechteck"
        for (int x = x1; x <= x2; x++)
        {
            float t = (float)(x - x1) / (x2 - x1);
            int y = (int)((1 - t) * y1 + t * y2);
            SDL_RenderDrawLine(renderer, x, y, x, height);
        }
    }

    // Höhenprofil-Linie darüber zeichnen
    SDL_SetRenderDrawColor(renderer, 0, 100, 200, 255); // Dunkelblau
    SDL_RenderDrawLines(renderer, polygon_points, track.total_points);

    free(polygon_points);
    SDL_SetRenderTarget(renderer, prev_target);
    return texture;
}

void save_elevation_profile_as_png(SDL_Renderer *renderer, const GpxTrack track, const char *filepath, int width, int height)
{
    // Create a target texture (RGBA)
    SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (!target)
    {
        SDL_Log("Failed to create target texture: %s", SDL_GetError());
        return;
    }

    // Set as render target
    SDL_Texture *prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, target);

    // Clear background
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);
    SDL_RenderClear(renderer);

    // Draw the profile into the current render target (your existing function)
    SDL_Texture *profile_tex = generate_elevation_profile_texture(renderer, track, width, height);
    SDL_RenderCopy(renderer, profile_tex, NULL, NULL);
    SDL_DestroyTexture(profile_tex);

    // Create surface to copy pixels into
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
    {
        SDL_Log("Failed to create surface: %s", SDL_GetError());
        SDL_SetRenderTarget(renderer, prev_target);
        SDL_DestroyTexture(target);
        return;
    }

    // Read pixels from the target texture into the surface
    SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, surface->pixels, surface->pitch);

    // Save surface as PNG
    if (IMG_SavePNG(surface, filepath) != 0)
    {
        SDL_Log("Failed to save PNG: %s", IMG_GetError());
    }

    // Cleanup
    SDL_FreeSurface(surface);
    SDL_SetRenderTarget(renderer, prev_target);
    SDL_DestroyTexture(target);
}

void updateTrackInfoGraphs(struct application *appl, GpxCollection collection)
{
  static int prev_selected_track = -1;
  if (appl->selected_track >= 0 && prev_selected_track != appl->selected_track)
  {
    save_elevation_profile_as_png(appl->renderer, collection.tracks[appl->selected_track], "resources/elev_profile.png", 200, 100);
  }
}