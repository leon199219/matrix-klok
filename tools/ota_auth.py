# Leest het OTA-wachtwoord uit include/secrets.h zodat het nergens anders hoeft te staan.
Import("env")
import re, os
p = os.path.join(env.subst("$PROJECT_DIR"), "include", "secrets.h")
if os.path.exists(p):
    m = re.search(r'OTA_PASSWORD\s+"([^"]*)"', open(p).read())
    if m:
        env.Append(UPLOADERFLAGS=["--auth=" + m.group(1)])
