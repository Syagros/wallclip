#!/usr/bin/env python3
"""Mechanical validation of every palettes/*/*.json. Exits non-zero on any failure."""
import json, re, sys, pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
HEX = re.compile(r"^#(?:[0-9a-f]{6}|[0-9a-f]{8})$")
ROLES = json.loads((ROOT / "index.json").read_text())["role_schema"]
EXPECT_NATIVE = {"catppuccin": 26, "dracula": 12}          # exact upstream counts
errors, files = [], sorted((ROOT / "palettes").glob("*/*.json"))

def walk(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items(): yield from walk(v, f"{path}.{k}")
    elif isinstance(o, list):
        for i, v in enumerate(o): yield from walk(v, f"{path}[{i}]")
    elif isinstance(o, str) and o.startswith("#"):
        yield path, o

# Independent spot-checks: values seen on official pages / READMEs during research
SPOT = {
 "nord-dark":        {"nord0":"#2e3440","nord8":"#88c0d0","nord11":"#bf616a","nord15":"#b48ead"},
 "dracula-dracula":  {"Background":"#282a36","Purple":"#bd93f9","Pink":"#ff79c6"},
 "gruvbox-dark-medium": {"bg0":"#282828","red":"#fb4934","yellow":"#fabd2f"},
 "gruvbox-dark-hard":   {"bg0":"#1d2021"},
 "catppuccin-mocha": {"base":"#1e1e2e"},
 "catppuccin-latte": {"mauve":"#8839ef"},
 "tokyonight-night": {"bg":"#1a1b26","blue":"#7aa2f7"},
}
for f in files:
    d = json.loads(f.read_text()); i = d["id"]
    if f.stem != i: errors.append(f"{f.name}: id mismatch")
    for p, h in walk({k: d[k] for k in d if k not in ("source","notes","role_sources","role_fallbacks")}):
        if not HEX.match(h): errors.append(f"{i}{p}: bad hex {h!r}")
    if list(d["roles"]) != ROLES: errors.append(f"{i}: roles keys/order differ from schema")
    pool = {**d["native"], **d.get("derived", {})}
    for r, src in d["role_sources"].items():
        if src not in pool: errors.append(f"{i}: role {r} -> missing {src}")
        elif pool[src] != d["roles"][r]: errors.append(f"{i}: role {r} != {src}")
    if d["theme"] in EXPECT_NATIVE and len(d["native"]) != EXPECT_NATIVE[d["theme"]]:
        errors.append(f"{i}: native count {len(d['native'])} != {EXPECT_NATIVE[d['theme']]}")
    if d["theme"] == "nord" and len(d["native"]) != 16: errors.append(f"{i}: nord needs 16")
    if d["mode"] not in ("dark", "light"): errors.append(f"{i}: bad mode")
    for k, v in SPOT.get(i, {}).items():
        if d["native"].get(k) != v: errors.append(f"{i}: spot-check {k} {d['native'].get(k)} != {v}")
ids = [json.loads(f.read_text())["id"] for f in files]
if sorted(ids) != sorted(json.loads((ROOT / "index.json").read_text())["variants"]):
    errors.append("index.json variants != files on disk")
print(f"checked {len(files)} files, {sum(len(SPOT.get(i,{})) for i in ids)} spot-checks")
if errors: print("\n".join("FAIL " + e for e in errors)); sys.exit(1)
print("OK")
