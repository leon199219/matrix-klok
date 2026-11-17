# Security

## Never publish

- An `include/secrets.h` with your OTA password
- A screenshot or log that shows the OTA password, a Wi-Fi password, or other credentials

Other people copy [`include/secrets.h.example`](include/secrets.h.example) to `include/secrets.h` and set their own password.

`include/secrets.h` is listed in [`.gitignore`](.gitignore). Before you push, check it is not in the commit.

`include/config.h` has pins, layout and timings only. It has no secrets and stays in this repo.

## OTA

The OTA password is a password. Do not share it in issues or screenshots.

If `include/secrets.h` is missing, the firmware still builds, but OTA updates are **not** password protected.

## Wi-Fi

Wi-Fi credentials are entered on the device through the `Matrix-Klok-Setup` access point (WiFiManager). They are not stored in this repo.
