/*
 * The map screen: CARTO tiles laid out around the view centre, aircraft icons rotated to
 * their track and tinted by altitude, drag to pan, +/- to zoom, tap a plane for details.
 *
 * Positions are extrapolated from speed and track between the 10-second data updates,
 * so the planes glide instead of jumping.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "flights.h"
#include "geo.h"
#include "plane_icon.h"
#include "tiles.h"
#include "ui.h"
#include "wifi.h"
#include "wifi_screen.h"

#define SCREEN_W            480
#define SCREEN_H            320
#define TILE_COLS           3           // enough to cover the screen at any offset
#define TILE_ROWS           3
#define MAX_SHOWN           120         // plane icons on screen at once
#define MAX_LABELS          40
#define LABEL_MIN_ZOOM      10
#define HIT_RADIUS          24          // px, for tapping a plane
#define EXTRAPOLATE_MAX_S   60
#define DRAG_THRESHOLD      10          // px of movement before a press counts as a pan

static lv_obj_t *s_screen;
static lv_obj_t *s_tiles[TILE_COLS * TILE_ROWS];
static lv_obj_t *s_home;
static lv_obj_t *s_planes[MAX_SHOWN];
static lv_obj_t *s_labels[MAX_LABELS];
static lv_obj_t *s_status;
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_title;
static lv_obj_t *s_panel_detail;
static lv_obj_t *s_panel_button;
static lv_obj_t *s_panel_button_label;
static lv_obj_t *s_panel_close;

// View: centre in world pixels at s_zoom
static int s_zoom;
static double s_cx, s_cy;
static bool s_view_ready;
static bool s_view_dirty;
static uint32_t s_frame = 10;
static uint32_t s_tiles_seen;
static int s_drag;

static flight_t *s_flights;
static int s_count;
static flights_info_t s_info;
static char s_selected[8];          // hex of the tapped plane, "" for none

typedef struct {
    int16_t x, y;
    int16_t index;                  // into s_flights, or -1 for the followed target
} hit_t;
static hit_t s_hits[MAX_SHOWN];
static int s_hit_count;

/* ---------- view ---------- */

static void view_center(double lat, double lon)
{
    s_cx = geo_lon_to_x(lon, s_zoom);
    s_cy = geo_lat_to_y(lat, s_zoom);
    s_view_dirty = true;
}

static void view_latlon(double *lat, double *lon)
{
    *lat = geo_y_to_lat(s_cy, s_zoom);
    *lon = geo_x_to_lon(s_cx, s_zoom);
}

static void report_area(void)
{
    double lat, lon;
    view_latlon(&lat, &lon);
    // Half the screen diagonal, in nautical miles
    double radius = hypot(SCREEN_W / 2, SCREEN_H / 2) * geo_meters_per_pixel(lat, s_zoom) / 1852.0;
    flights_set_area(lat, lon, radius);
}

static void view_zoom(int delta)
{
    int zoom = s_zoom + delta;
    if (zoom < TILES_ZOOM_MIN || zoom > TILES_ZOOM_MAX) {
        return;
    }
    double lat, lon;
    view_latlon(&lat, &lon);
    s_zoom = zoom;
    view_center(lat, lon);
    report_area();
}

/* ---------- drawing ---------- */

static uint32_t altitude_color(const flight_t *f)
{
    if (f->on_ground) {
        return 0x9E9E9E;
    }
    return f->altitude < 2000 ? 0x66BB6A : f->altitude < 10000 ? 0xD4E157 : f->altitude < 20000 ? 0xFFCA28
         : f->altitude < 30000 ? 0xFFA726 : 0xFF7043;
}

// Where the plane probably is now, from its last position, speed and track.
static void extrapolate(const flight_t *f, double *lat, double *lon)
{
    *lat = f->lat;
    *lon = f->lon;
    double dt = (esp_timer_get_time() - s_info.fetched_us) / 1e6 + f->age;
    if (!f->on_ground && f->speed > 30 && dt > 0) {
        geo_offset(lat, lon, f->track, f->speed * 1852.0 / 3600.0 * fmin(dt, EXTRAPOLATE_MAX_S));
    }
}

static void to_screen(double lat, double lon, int *x, int *y)
{
    double wx = geo_lon_to_x(lon, s_zoom) - (s_cx - SCREEN_W / 2);
    double wy = geo_lat_to_y(lat, s_zoom) - (s_cy - SCREEN_H / 2);
    // Take the shortest way round the antimeridian
    double world = geo_world_size(s_zoom);
    if (wx < -world / 2) {
        wx += world;
    } else if (wx > world / 2) {
        wx -= world;
    }
    *x = (int)lround(wx);
    *y = (int)lround(wy);
}

static void layout_tiles(void)
{
    const double left = s_cx - SCREEN_W / 2;
    const double top = s_cy - SCREEN_H / 2;
    const int n = 1 << s_zoom;
    const int tx0 = (int)floor(left / GEO_TILE_SIZE);
    const int ty0 = (int)floor(top / GEO_TILE_SIZE);
    for (int j = 0; j < TILE_ROWS; j++) {
        for (int i = 0; i < TILE_COLS; i++) {
            lv_obj_t *img = s_tiles[j * TILE_COLS + i];
            int tx = tx0 + i, ty = ty0 + j;
            const lv_image_dsc_t *dsc = ty >= 0 && ty < n ? tiles_get(s_zoom, ((tx % n) + n) % n, ty, s_frame) : NULL;
            lv_obj_set_hidden(img, dsc == NULL);
            if (dsc) {
                // Cache entries get reused, so the same descriptor can hold new pixels
                lv_image_set_src(img, dsc);
                lv_obj_invalidate(img);
                lv_obj_set_pos(img, (int)lround(tx * GEO_TILE_SIZE - left), (int)lround(ty * GEO_TILE_SIZE - top));
            }
        }
    }
    s_frame++;
}

static bool is_target(const flight_t *f)
{
    return (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST) && !strcmp(f->hex, s_info.target.hex);
}

static void place_plane(const flight_t *f, int index, int *labels)
{
    double lat, lon;
    extrapolate(f, &lat, &lon);
    int x, y;
    to_screen(lat, lon, &x, &y);
    if (x < -20 || x > SCREEN_W + 20 || y < -20 || y > SCREEN_H + 20) {
        return;
    }
    bool target = is_target(f);
    bool selected = s_selected[0] && !strcmp(f->hex, s_selected);
    lv_obj_t *img = s_planes[s_hit_count];
    lv_obj_set_pos(img, x - PLANE_ICON_SIZE / 2, y - PLANE_ICON_SIZE / 2);
    lv_image_set_rotation(img, (int32_t)lroundf(f->track * 10));
    lv_obj_set_style_image_recolor(img, lv_color_hex(target ? UI_COLOR_FOLLOW : selected ? 0xFFFFFF : altitude_color(f)), 0);
    lv_obj_set_hidden(img, false);

    if ((s_zoom >= LABEL_MIN_ZOOM || target || selected) && *labels < MAX_LABELS) {
        lv_obj_t *label = s_labels[(*labels)++];
        lv_label_set_text(label, f->callsign[0] ? f->callsign : f->reg[0] ? f->reg : f->hex);
        lv_obj_set_pos(label, x + PLANE_ICON_SIZE / 2, y - 6);
        lv_obj_set_hidden(label, false);
    }
    s_hits[s_hit_count++] = (hit_t){.x = x, .y = y, .index = index};
}

static void layout_planes(void)
{
    s_hit_count = 0;
    int labels = 0;
    bool target_drawn = false;
    for (int i = 0; i < s_count && s_hit_count < MAX_SHOWN; i++) {
        target_drawn |= is_target(&s_flights[i]);
        place_plane(&s_flights[i], i, &labels);
    }
    // The followed plane may be missing from the area list (e.g. while it's "lost")
    if (!target_drawn && (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST) && s_hit_count < MAX_SHOWN) {
        place_plane(&s_info.target, -1, &labels);
    }
    for (int i = s_hit_count; i < MAX_SHOWN; i++) {
        lv_obj_set_hidden(s_planes[i], true);
    }
    for (int i = labels; i < MAX_LABELS; i++) {
        lv_obj_set_hidden(s_labels[i], true);
    }

    if (s_info.home_known) {
        int x, y;
        to_screen(s_info.home_lat, s_info.home_lon, &x, &y);
        lv_obj_set_pos(s_home, x - 6, y - 6);
        lv_obj_set_hidden(s_home, false);
    }
}

/* ---------- status line and details panel ---------- */

static void thousands(char *buf, size_t len, int value)
{
    if (value >= 1000 || value <= -1000) {
        snprintf(buf, len, "%d,%03d", value / 1000, abs(value % 1000));
    } else {
        snprintf(buf, len, "%d", value);
    }
}

static const flight_t *panel_flight(bool *is_followed)
{
    *is_followed = false;
    if (s_selected[0]) {
        for (int i = 0; i < s_count; i++) {
            if (!strcmp(s_flights[i].hex, s_selected)) {
                *is_followed = is_target(&s_flights[i]);
                return &s_flights[i];
            }
        }
    }
    if (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST) {
        *is_followed = true;
        return &s_info.target;
    }
    return NULL;
}

static void update_panel(void)
{
    bool followed;
    const flight_t *f = panel_flight(&followed);
    lv_obj_set_hidden(s_panel, f == NULL);
    if (!f) {
        return;
    }
    lv_label_set_text(s_panel_title, f->callsign[0] ? f->callsign : f->reg[0] ? f->reg : f->hex);

    // LVGL's own printf has no float support, so format with the C library
    char text[128];
    if (f->on_ground) {
        snprintf(text, sizeof(text), "%s  %s   On the ground   %.0f kt", f->type, f->reg, f->speed);
    } else {
        char alt[16], rate[16];
        thousands(alt, sizeof(alt), f->altitude);
        thousands(rate, sizeof(rate), abs(f->vertical_rate));
        snprintf(text, sizeof(text), "%s  %s   %s ft  %s%s fpm  %.0f kt  %03.0f\xC2\xB0",
                 f->type, f->reg, alt,
                 f->vertical_rate > 100 ? LV_SYMBOL_UP " " : f->vertical_rate < -100 ? LV_SYMBOL_DOWN " " : "",
                 rate, f->speed, f->track);
    }
    if (followed && s_info.follow == FOLLOW_LOST) {
        strlcat(text, "   (signal lost)", sizeof(text));
    }
    lv_label_set_text(s_panel_detail, text);
    lv_label_set_text(s_panel_button_label, followed ? "Stop following" : LV_SYMBOL_GPS " Follow");
    lv_obj_set_hidden(s_panel_close, followed && !s_selected[0]);
}

static void update_status(void)
{
    wifi_status_t wifi;
    wifi_get_status(&wifi);
    char text[80];
    if (wifi.state != WIFI_STATE_CONNECTED) {
        snprintf(text, sizeof(text), LV_SYMBOL_WIFI "  %s", wifi.state == WIFI_STATE_CONNECTING ? "Connecting..." : "No Wi-Fi - tap to set up");
    } else if (!s_info.home_known) {
        snprintf(text, sizeof(text), "Finding your location...");
    } else if (s_info.follow == FOLLOW_SEARCHING) {
        snprintf(text, sizeof(text), "Looking for %s...", s_info.follow_label);
    } else if (s_info.follow == FOLLOW_NOT_FOUND) {
        snprintf(text, sizeof(text), "%s: not airborne, or no ADS-B", s_info.follow_label);
    } else if (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST) {
        snprintf(text, sizeof(text), LV_SYMBOL_GPS "  Following %s%s", s_info.follow_label,
                 s_info.follow == FOLLOW_LOST ? " (signal lost)" : "");
    } else {
        int age = s_info.fetched_us ? (int)((esp_timer_get_time() - s_info.fetched_us) / 1000000) : -1;
        snprintf(text, sizeof(text), "%d aircraft%s", s_count, !s_info.ok ? "   (update failed)" : age > 30 ? "   (stale)" : "");
    }
    lv_label_set_text(s_status, text);
}

/* ---------- timers and events ---------- */

static void refresh_data(void)
{
    s_count = flights_get(s_flights, FLIGHTS_MAX, &s_info);

    if (!s_view_ready && s_info.home_known) {
        view_center(s_info.home_lat, s_info.home_lon);
        s_view_ready = true;
        report_area();
    }
    // A followed plane stays in the middle of the map
    if (s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST) {
        double lat, lon;
        extrapolate(&s_info.target, &lat, &lon);
        view_center(lat, lon);
    }
    if (s_info.follow == FOLLOW_NOT_FOUND || s_info.follow == FOLLOW_OFF) {
        // Forget a selection whose plane has left the area
        bool present = false;
        for (int i = 0; i < s_count && s_selected[0] && !present; i++) {
            present = !strcmp(s_flights[i].hex, s_selected);
        }
        if (!present) {
            s_selected[0] = '\0';
        }
    }
}

static void tick_cb(lv_timer_t *timer)
{
    static int ticks;
    bool second = ++ticks % 10 == 0;
    if (second) {
        refresh_data();
        update_status();
        update_panel();
    }
    uint32_t gen = tiles_generation();
    if (s_view_dirty || gen != s_tiles_seen) {
        s_view_dirty = false;
        s_tiles_seen = gen;
        layout_tiles();
        layout_planes();
    } else if (second) {
        layout_planes();
    }
}

static bool following(void)
{
    return s_info.follow == FOLLOW_TRACKING || s_info.follow == FOLLOW_LOST;
}

static void map_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (code == LV_EVENT_PRESSED) {
        s_drag = 0;
    } else if (code == LV_EVENT_PRESSING && !following()) {
        // The map is locked to a followed plane; otherwise drag to pan
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        s_drag += abs(v.x) + abs(v.y);
        if (s_drag >= DRAG_THRESHOLD) {
            s_cx -= v.x;
            s_cy -= v.y;
            s_view_dirty = true;
        }
    } else if (code == LV_EVENT_RELEASED && s_drag >= DRAG_THRESHOLD) {
        report_area();
    } else if (code == LV_EVENT_CLICKED && s_drag < DRAG_THRESHOLD) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int best = -1, best_d = HIT_RADIUS * HIT_RADIUS;
        for (int i = 0; i < s_hit_count; i++) {
            int dx = s_hits[i].x - p.x, dy = s_hits[i].y - p.y;
            if (dx * dx + dy * dy < best_d) {
                best_d = dx * dx + dy * dy;
                best = i;
            }
        }
        const flight_t *f = best < 0 ? NULL : s_hits[best].index < 0 ? &s_info.target : &s_flights[s_hits[best].index];
        strlcpy(s_selected, f ? f->hex : "", sizeof(s_selected));
        update_panel();
        layout_planes();
    }
}

static void zoom_in_cb(lv_event_t *e)
{
    view_zoom(1);
}

static void zoom_out_cb(lv_event_t *e)
{
    view_zoom(-1);
}

static void home_cb(lv_event_t *e)
{
    if (following()) {
        flights_unfollow();
        s_info.follow = FOLLOW_OFF;
    }
    if (s_info.home_known) {
        view_center(s_info.home_lat, s_info.home_lon);
        report_area();
    }
}

static void find_cb(lv_event_t *e)
{
    ui_follow_open();
}

static void status_cb(lv_event_t *e)
{
    wifi_status_t wifi;
    wifi_get_status(&wifi);
    if (wifi.state != WIFI_STATE_CONNECTED) {
        wifi_screen_open();
    }
}

static void panel_button_cb(lv_event_t *e)
{
    bool followed;
    const flight_t *f = panel_flight(&followed);
    if (followed) {
        flights_unfollow();
        s_info.follow = FOLLOW_OFF;
        report_area();
    } else if (f) {
        flights_follow_hex(f->hex, f->callsign[0] ? f->callsign : f->reg);
        s_info.follow = FOLLOW_SEARCHING;
    }
    s_selected[0] = '\0';
    update_panel();
}

static void panel_close_cb(lv_event_t *e)
{
    s_selected[0] = '\0';
    update_panel();
    layout_planes();
}

/* ---------- construction ---------- */

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 48, 48);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return b;
}

static void build_panel(void)
{
    s_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_panel, SCREEN_W - 16, 78);
    lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);

    s_panel_title = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_panel_title, &lv_font_montserrat_24, 0);
    lv_obj_align(s_panel_title, LV_ALIGN_TOP_LEFT, 0, -2);
    s_panel_detail = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_panel_detail, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_align(s_panel_detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_panel_button = lv_button_create(s_panel);
    lv_obj_set_height(s_panel_button, 38);
    lv_obj_align(s_panel_button, LV_ALIGN_TOP_RIGHT, -44, -4);
    lv_obj_add_event_cb(s_panel_button, panel_button_cb, LV_EVENT_CLICKED, NULL);
    s_panel_button_label = lv_label_create(s_panel_button);
    lv_obj_center(s_panel_button_label);

    s_panel_close = lv_button_create(s_panel);
    lv_obj_set_size(s_panel_close, 38, 38);
    lv_obj_align(s_panel_close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(s_panel_close, lv_color_hex(0x30363D), 0);
    lv_obj_add_event_cb(s_panel_close, panel_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(s_panel_close);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);

    lv_obj_set_hidden(s_panel, true);
}

void ui_map_init(int zoom)
{
    s_zoom = zoom;
    s_flights = heap_caps_calloc(FLIGHTS_MAX, sizeof(flight_t), MALLOC_CAP_SPIRAM);

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_text_color(s_screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_screen, &lv_font_montserrat_16, 0);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(s_screen, false);
    lv_obj_add_event_cb(s_screen, map_event_cb, LV_EVENT_ALL, NULL);

    for (int i = 0; i < TILE_COLS * TILE_ROWS; i++) {
        s_tiles[i] = lv_image_create(s_screen);
        lv_obj_set_hidden(s_tiles[i], true);
    }

    s_home = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, 12, 12);
    lv_obj_set_style_radius(s_home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_home, lv_color_hex(0x2F81F7), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_home, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_home, 2, 0);
    lv_obj_set_clickable(s_home, false);
    lv_obj_set_hidden(s_home, true);

    const lv_image_dsc_t *icon = plane_icon_get();
    for (int i = 0; i < MAX_SHOWN; i++) {
        lv_obj_t *img = lv_image_create(s_screen);
        lv_image_set_src(img, icon);
        lv_image_set_pivot(img, PLANE_ICON_SIZE / 2, PLANE_ICON_SIZE / 2);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_obj_set_hidden(img, true);
        s_planes[i] = img;
    }
    for (int i = 0; i < MAX_LABELS; i++) {
        lv_obj_t *label = lv_label_create(s_screen);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xC9D1D9), 0);
        lv_obj_set_hidden(label, true);
        s_labels[i] = label;
    }

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_bg_color(s_status, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(s_status, 8, 0);
    lv_obj_set_style_pad_ver(s_status, 4, 0);
    lv_obj_set_style_radius(s_status, 6, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_clickable(s_status, true);
    lv_obj_add_event_cb(s_status, status_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(s_status, "Starting...");

    add_button(s_screen, LV_SYMBOL_PLUS, 8, zoom_in_cb);
    add_button(s_screen, LV_SYMBOL_MINUS, 62, zoom_out_cb);
    add_button(s_screen, LV_SYMBOL_HOME, 116, home_cb);
    add_button(s_screen, LV_SYMBOL_GPS, 170, find_cb);

    // Required by the tile provider's terms
    lv_obj_t *attribution = lv_label_create(s_screen);
    lv_obj_set_style_text_font(attribution, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(attribution, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_label_set_text(attribution, TILES_ATTRIBUTION);
    lv_obj_align(attribution, LV_ALIGN_BOTTOM_RIGHT, -6, -3);

    build_panel();
    lv_screen_load(s_screen);
    lv_timer_create(tick_cb, 100, NULL);
}

lv_obj_t *ui_map_screen(void)
{
    return s_screen;
}
