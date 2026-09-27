#include "airlines.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// IATA -> ICAO airline designators for carriers commonly seen in and around the US,
// plus the major international ones.
static const char *const AIRLINES[][2] = {
    {"AA", "AAL"}, {"UA", "UAL"}, {"DL", "DAL"}, {"WN", "SWA"}, {"AS", "ASA"}, {"B6", "JBU"},
    {"NK", "NKS"}, {"F9", "FFT"}, {"G4", "AAY"}, {"HA", "HAL"}, {"SY", "SCX"}, {"MX", "MXY"},
    {"XP", "VXP"}, {"QX", "QXE"}, {"OO", "SKW"}, {"YX", "RPA"}, {"MQ", "ENY"}, {"9E", "EDV"},
    {"OH", "JIA"}, {"YV", "ASH"}, {"ZW", "AWI"}, {"PT", "PDT"}, {"C5", "UCA"}, {"G7", "GJS"},
    {"AC", "ACA"}, {"WS", "WJA"}, {"PD", "POE"}, {"TS", "TSC"}, {"F8", "FLE"},
    {"AM", "AMX"}, {"Y4", "VOI"}, {"VB", "VIV"}, {"4O", "AIJ"}, {"CM", "CMP"}, {"AV", "AVA"},
    {"LA", "LAN"}, {"JJ", "TAM"}, {"G3", "GLO"}, {"AD", "AZU"}, {"AR", "ARG"}, {"CU", "CUB"},
    {"BA", "BAW"}, {"VS", "VIR"}, {"LH", "DLH"}, {"AF", "AFR"}, {"KL", "KLM"}, {"IB", "IBE"},
    {"EI", "EIN"}, {"LX", "SWR"}, {"OS", "AUA"}, {"SN", "BEL"}, {"SK", "SAS"}, {"AY", "FIN"},
    {"TP", "TAP"}, {"AZ", "ITY"}, {"LO", "LOT"}, {"FR", "RYR"}, {"U2", "EZY"}, {"W6", "WZZ"},
    {"DY", "NOZ"}, {"VY", "VLG"}, {"EW", "EWG"}, {"TK", "THY"}, {"EK", "UAE"}, {"QR", "QTR"},
    {"EY", "ETD"}, {"SV", "SVA"}, {"LY", "ELY"}, {"ET", "ETH"}, {"MS", "MSR"}, {"SA", "SAA"},
    {"SQ", "SIA"}, {"CX", "CPA"}, {"JL", "JAL"}, {"NH", "ANA"}, {"KE", "KAL"}, {"OZ", "AAR"},
    {"CI", "CAL"}, {"BR", "EVA"}, {"CA", "CCA"}, {"MU", "CES"}, {"CZ", "CSN"}, {"HU", "CHH"},
    {"PR", "PAL"}, {"VN", "HVN"}, {"TG", "THA"}, {"MH", "MAS"}, {"GA", "GIA"}, {"AI", "AIC"},
    {"QF", "QFA"}, {"VA", "VOZ"}, {"NZ", "ANZ"}, {"FJ", "FJI"},
    {"FX", "FDX"}, {"5X", "UPS"}, {"5Y", "GTI"}, {"K4", "CKS"}, {"PO", "PAC"},
};

bool airlines_to_callsign(const char *input, char *out, size_t len)
{
    // Normalize: upper case, no spaces or dashes
    char clean[16];
    size_t n = 0;
    for (const char *p = input; *p && n < sizeof(clean) - 1; p++) {
        if (!isspace((unsigned char)*p) && *p != '-') {
            clean[n++] = (char)toupper((unsigned char)*p);
        }
    }
    clean[n] = '\0';

    // Flight number: 2-character airline code, then 1-4 digits and maybe a suffix letter
    const char *digits = clean + 2;
    size_t ndigits = strspn(digits, "0123456789");
    bool flight_number = n >= 3 && ndigits >= 1 && ndigits <= 4 && strlen(digits + ndigits) <= 1 &&
                         isalnum((unsigned char)clean[0]) && isalnum((unsigned char)clean[1]) &&
                         !(isdigit((unsigned char)clean[0]) && isdigit((unsigned char)clean[1]));
    if (flight_number) {
        for (size_t i = 0; i < sizeof(AIRLINES) / sizeof(AIRLINES[0]); i++) {
            if (clean[0] == AIRLINES[i][0][0] && clean[1] == AIRLINES[i][0][1]) {
                while (*digits == '0' && digits[1] >= '0' && digits[1] <= '9') {
                    digits++;       // callsigns drop leading zeros: UA0012 -> UAL12
                }
                snprintf(out, len, "%s%s", AIRLINES[i][1], digits);
                return true;
            }
        }
    }
    strlcpy(out, clean, len);
    return false;
}
