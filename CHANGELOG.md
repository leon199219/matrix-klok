# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- README in the MakerPulse-CYD house style, with a cover image and product photos.
- SECURITY.md.

## [2026.11.17-v8] - 2026-11-17

Firmware built 16–17 November 2026.

### Added

- WiFi clock firmware for a 30-pin ESP32 DevKit and an 8×32 WS2812B LED matrix.
- WiFiManager setup access point `Matrix-Klok-Setup`.
- NTP time sync every 6 hours, stored in a DS3231 RTC (UTC). The clock keeps time without WiFi. Automatic summer/winter time (Europe/Amsterdam by default).
- ArduinoOTA with an optional password from `include/secrets.h`. Hostname `matrix-klok`, port 3232.
- Web GUI on port 80 (`http://<clock-ip>/` and `http://matrix-klok.local/`): daily display on/off timer, hourly-shows checkbox, restart, setup-AP button, and `GET /api/timer` JSON.
- Four screens: time, time with seconds, date, and DS3231 temperature. Returns to the time after 10 s.
- Nine colour modes and seven brightness steps, with night dimming from 23:00 to 07:00.
- 13 hourly show effects in rotation (VUURWERK, MATRIX, STERREN, REGENBOOG, SNEEUW, PACMAN, BALLEN, VUUR, RUIMTE, ONWEER, TETRIS, SNAKE, HART). An 8-second show plays on the hour. No shows at night or while the display timer is off. The rotation position is saved in NVS.
- Buttons: short and long press (setup AP, brightness, colour, next screen, next show). Hold the middle button for 8 s to clear WiFi credentials. Hold the left button at power-up for the matrix test pattern.
- Matrix orientation options in `include/config.h` (`COLUMN_MAJOR`, `SERPENTINE`, `ORIGIN_BOTTOM_LEFT`, `MIRROR_X`).
- Settings (brightness, colour mode, timer, shows) saved in NVS and kept across a power loss.

### Changed

- VUUR show is toned down: half the update rate, lower flames, no white tips, about 55% brightness.
- OTA is more stable: Wi-Fi modem sleep is off, and the progress bar is redrawn only when it changes.
