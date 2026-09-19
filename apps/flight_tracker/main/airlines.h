#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * Turn what a person types into the callsign ADS-B broadcasts. A flight number with a
 * known IATA airline code becomes the ICAO callsign ("UA 0123" -> "UAL123"); anything
 * else (an ICAO callsign like "UAL123", a registration like "N12345") is just
 * normalized: upper-cased, spaces removed. Returns true when an airline code was
 * translated.
 *
 * Regional flights flown under a partner's flight number (e.g. a "UA" flight operated
 * by SkyWest) often broadcast the operator's callsign instead, so they won't match.
 */
bool airlines_to_callsign(const char *input, char *out, size_t len);
