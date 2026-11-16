"""Generate src/webui.h from web/index.html.

Runs automatically before every PlatformIO build (extra_scripts in platformio.ini)
and can also be run by hand:  python web/gen_webui.py
"""
import os

try:
    Import("env")  # noqa: F821  (only defined when run by PlatformIO)
    ROOT = env.subst("$PROJECT_DIR")  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(ROOT, "web", "index.html")
DST = os.path.join(ROOT, "src", "webui.h")
DELIM = "rawliteral"

with open(SRC, "r", encoding="utf-8") as f:
    html = f.read()

if ")" + DELIM + '"' in html:
    raise SystemExit("index.html contains the raw string delimiter")

out = (
    "// GENERATED from web/index.html by web/gen_webui.py - edit the HTML, not this file\n"
    "#pragma once\n"
    "#include <pgmspace.h>\n"
    'const char INDEX_HTML[] PROGMEM = R"' + DELIM + "(" + html + ")" + DELIM + '";\n'
)

old = None
if os.path.exists(DST):
    with open(DST, "r", encoding="utf-8") as f:
        old = f.read()
if old != out:
    with open(DST, "w", encoding="utf-8") as f:
        f.write(out)
    print("webui.h regenerated")
