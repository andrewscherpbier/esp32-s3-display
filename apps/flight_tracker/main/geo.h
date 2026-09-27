#pragma once

/*
 * Web Mercator ("slippy map") maths, as used by OpenStreetMap-style tile servers. "World
 * pixels" at zoom z run 0..256*2^z east-west and north-south; tile (x, y) covers world
 * pixels [x*256, x*256+256).
 */

#include <math.h>

#define GEO_TILE_SIZE 256

static inline double geo_world_size(int zoom)
{
    return (double)GEO_TILE_SIZE * (double)(1 << zoom);
}

static inline double geo_lon_to_x(double lon, int zoom)
{
    return (lon + 180.0) / 360.0 * geo_world_size(zoom);
}

static inline double geo_lat_to_y(double lat, int zoom)
{
    double s = sin(lat * M_PI / 180.0);
    return (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * geo_world_size(zoom);
}

static inline double geo_x_to_lon(double x, int zoom)
{
    return x / geo_world_size(zoom) * 360.0 - 180.0;
}

static inline double geo_y_to_lat(double y, int zoom)
{
    double n = M_PI - 2.0 * M_PI * y / geo_world_size(zoom);
    return 180.0 / M_PI * atan(sinh(n));
}

static inline double geo_meters_per_pixel(double lat, int zoom)
{
    return 156543.03392 * cos(lat * M_PI / 180.0) / (double)(1 << zoom);
}

// Move (lat, lon) `meters` along `bearing` (degrees clockwise from north); fine for the
// short distances a plane covers between updates.
static inline void geo_offset(double *lat, double *lon, double bearing, double meters)
{
    double b = bearing * M_PI / 180.0;
    *lat += meters * cos(b) / 111320.0;
    *lon += meters * sin(b) / (111320.0 * cos(*lat * M_PI / 180.0));
}
