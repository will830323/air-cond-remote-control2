#!/usr/bin/env python3
"""Regenerate web_ui.h from index.html after you edit the app's look.
Usage:  python3 make_web_ui.py
"""
from pathlib import Path

here = Path(__file__).resolve().parent
html = (here / "index.html").read_text(encoding="utf-8")
delim = "YORKUI"
assert f"){delim}\"" not in html
(here / "web_ui.h").write_text(
    "// Auto-generated from index.html by make_web_ui.py – do not edit by hand.\n"
    "#pragma once\n#include <Arduino.h>\n"
    f"const char INDEX_HTML[] PROGMEM = R\"{delim}(" + html + f"){delim}\";\n",
    encoding="utf-8",
)
print(f"web_ui.h written ({len(html.encode())} bytes of HTML)")
