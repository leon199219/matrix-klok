#pragma once
// ======================= Matrix-Klok configuratie =======================

// --- Pinnen ---
#define PIN_MATRIX      32
#define PIN_BTN_LEFT    26
#define PIN_BTN_MID     27
#define PIN_BTN_RIGHT   14
#define PIN_SDA         21
#define PIN_SCL         22
#define PIN_SQW         4     // DS3231 SQW (1 Hz), open-drain -> INPUT_PULLUP

// --- Matrix-indeling ---
#define MATRIX_W        32
#define MATRIX_H        8
#define NUM_LEDS        (MATRIX_W * MATRIX_H)
// Pixel 0 linksonder, 8 pixels per kolom, kolommen zigzag (meest gebruikte 8x32 paneel).
// Klopt de test (LINKS ingedrukt houden bij opstarten) niet? Pas deze aan:
#define COLUMN_MAJOR        1   // 1 = pixels lopen per kolom, 0 = per rij
#define SERPENTINE          1   // 1 = elke volgende kolom/rij keert om (zigzag)
#define ORIGIN_BOTTOM_LEFT  0   // 1 = pixel 0 linksonder, 0 = pixel 0 linksboven
#define MIRROR_X            0   // 1 = spiegel horizontaal (pixel 0 rechts)

// --- LED-veiligheid ---
#define LED_VOLTS       5
#define LED_MAX_MA      1500
#define DEFAULT_BRI_IDX 2       // index in BRIGHTNESS_STEPS (20/255)
static const uint8_t BRIGHTNESS_STEPS[] = {5, 10, 20, 35, 60, 100, 150};

// --- Nachtdimmen ---
#define NIGHT_DIM_ENABLED 1
#define NIGHT_START_HOUR  23
#define NIGHT_END_HOUR    7
#define NIGHT_BRIGHTNESS  4
#define NIGHT_NO_SHOWS    1     // geen uurshow 's nachts

// --- Tijd / netwerk ---
#define TZ_INFO         "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER1     "nl.pool.ntp.org"
#define NTP_SERVER2     "pool.ntp.org"
#define NTP_RESYNC_MS   (6UL * 3600UL * 1000UL)
#define HOSTNAME        "matrix-klok"
#define AP_NAME         "Matrix-Klok-Setup"

// --- Gedrag ---
#define SCREEN_TIMEOUT_MS   10000   // terug naar tijd na 10 s
#define SHOW_DURATION_MS    8000    // uurshow-duur
#define LONG_PRESS_MS       800
#define WIFI_RESET_MS       8000    // middelste knop 8 s = wifi wissen
