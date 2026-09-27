#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define FLIGHTS_MAX         250
#define FLIGHTS_REFRESH_SEC 10

typedef struct {
    char hex[8];            // ICAO 24-bit address, the stable identity of an aircraft
    char callsign[10];
    char reg[12];
    char type[6];           // ICAO type designator, e.g. "B38M"
    double lat, lon;
    float track;            // degrees clockwise from north
    int altitude;           // feet (barometric)
    bool on_ground;
    float speed;            // knots over ground
    int vertical_rate;      // feet per minute
    float age;              // seconds since this position was received, at fetch time
} flight_t;

typedef enum {
    FOLLOW_OFF,
    FOLLOW_SEARCHING,       // looking the query up
    FOLLOW_TRACKING,        // `target` is current
    FOLLOW_NOT_FOUND,       // nothing airborne matched the query
    FOLLOW_LOST,            // was tracking, no position for a while; `target` is the last one
} follow_state_t;

typedef struct {
    bool home_known;
    double home_lat, home_lon;
    int count;
    int64_t fetched_us;     // esp_timer time of the data, for extrapolating positions
    bool ok;                // last fetch succeeded
    follow_state_t follow;
    char follow_label[16];  // what the user asked for, e.g. "UA123"
    flight_t target;
} flights_info_t;

/**
 * Start polling adsb.lol (adsb.fi as fallback) every FLIGHTS_REFRESH_SEC. Home is
 * `home_lat`/`home_lon` when `home_known`, otherwise looked up from the IP address.
 * Call after wifi_init().
 */
void flights_start(bool home_known, double home_lat, double home_lon);

// The area to report aircraft for; takes effect at once if it changed a lot.
void flights_set_area(double lat, double lon, double radius_nm);

// Follow what the user typed: a flight number, callsign or registration.
void flights_follow(const char *query);
// Follow the aircraft with this ICAO address (e.g. one tapped on the map).
void flights_follow_hex(const char *hex, const char *label);
void flights_unfollow(void);

// Copy the latest aircraft (up to `max`) and status.
int flights_get(flight_t *out, int max, flights_info_t *info);
