/*
 * The plane icon is rasterized once at start-up from a few polygons, with 4x4
 * supersampling for smooth edges, rather than shipping an image file.
 */
#include "plane_icon.h"

#include <stdbool.h>

typedef struct {
    float x, y;
} pt_t;

// Outline in unit coordinates, nose at the top
static const pt_t FUSELAGE[] = {{0.50f, 0.00f}, {0.56f, 0.10f}, {0.56f, 0.86f}, {0.50f, 0.95f}, {0.44f, 0.86f}, {0.44f, 0.10f}};
static const pt_t WINGS[] = {{0.50f, 0.33f}, {0.98f, 0.58f}, {0.98f, 0.65f}, {0.56f, 0.56f},
                             {0.44f, 0.56f}, {0.02f, 0.65f}, {0.02f, 0.58f}};
static const pt_t TAIL[] = {{0.50f, 0.76f}, {0.74f, 0.90f}, {0.74f, 0.96f}, {0.50f, 0.90f},
                            {0.26f, 0.96f}, {0.26f, 0.90f}};

static bool inside(const pt_t *poly, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((poly[i].y > y) != (poly[j].y > y) &&
            x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) {
            in = !in;
        }
    }
    return in;
}

const lv_image_dsc_t *plane_icon_get(void)
{
    static uint8_t pixels[PLANE_ICON_SIZE * PLANE_ICON_SIZE * 4];
    static lv_image_dsc_t dsc;
    if (dsc.data) {
        return &dsc;
    }

    const int ss = 4;
    for (int py = 0; py < PLANE_ICON_SIZE; py++) {
        for (int px = 0; px < PLANE_ICON_SIZE; px++) {
            int hits = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    float x = (px + (sx + 0.5f) / ss) / PLANE_ICON_SIZE;
                    float y = (py + (sy + 0.5f) / ss) / PLANE_ICON_SIZE;
                    hits += inside(FUSELAGE, 6, x, y) || inside(WINGS, 7, x, y) || inside(TAIL, 6, x, y);
                }
            }
            uint8_t *p = &pixels[(py * PLANE_ICON_SIZE + px) * 4];     // B, G, R, A
            p[0] = p[1] = p[2] = 255;
            p[3] = (uint8_t)(hits * 255 / (ss * ss));
        }
    }

    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc.header.w = PLANE_ICON_SIZE;
    dsc.header.h = PLANE_ICON_SIZE;
    dsc.header.stride = PLANE_ICON_SIZE * 4;
    dsc.data_size = sizeof(pixels);
    dsc.data = pixels;
    return &dsc;
}
