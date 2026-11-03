#!/usr/bin/env python3
"""Generate www/themes.css from the normalized theme palettes.

Reads the pinned palette JSONs (extracted from theme-palettes.tar.gz, kept
in www/palettes/ for reproducibility) and emits one merged light/dark block
per theme family. Light-variant roles below 4.5:1 on bg (per the bundled
contrast report) are darkened until they reach 4.5:1; tokyonight-day's fg is
additionally darkened until it passes on --surface (used as input background).
nord-light is intentionally ignored (single official Nord palette).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PAL = os.path.join(HERE, "palettes")
OUT = os.path.join(HERE, "themes.css")

# role -> CSS variable name (fg/fg_muted map to the stylesheet's --text/--muted)
VAR = {
    "bg": "--bg", "bg_alt": "--bg-alt", "surface": "--surface",
    "border": "--border", "fg": "--text", "fg_muted": "--muted",
    "red": "--red", "orange": "--orange", "yellow": "--yellow",
    "green": "--green", "cyan": "--cyan", "blue": "--blue",
    "purple": "--purple", "pink": "--pink", "accent": "--accent",
}
ORDER = ["bg", "bg_alt", "surface", "border", "fg", "fg_muted",
         "red", "orange", "yellow", "green", "cyan", "blue",
         "purple", "pink", "accent"]

# One dropdown entry per theme family: light palette + dark palette.
FAMILIES = [
    ("gruvbox", "Gruvbox", "gruvbox-light-medium", "gruvbox-dark-medium",
     "morhetz/gruvbox @ ef8864b, MIT/X11"),
    ("catppuccin", "Catppuccin", "catppuccin-latte", "catppuccin-mocha",
     "catppuccin/palette @ 07d02aa, MIT"),
    ("tokyonight", "Tokyo Night", "tokyonight-day", "tokyonight-moon",
     "folke/tokyonight.nvim @ cdc07ac, Apache-2.0"),
    ("dracula", "Dracula", None, "dracula-dracula",
     "dracula/dracula-theme @ 8d33e54, MIT (no light variant)"),
    ("nord", "Nord", None, "nord-dark",
     "nordtheme/nord @ 1cef716, MIT (no light variant)"),
]


def srgb(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(h):
    r, g, b = (int(h[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * srgb(r) + 0.7152 * srgb(g) + 0.0722 * srgb(b)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def darken(hexv, bg, target=4.5):
    """Blend toward black until contrast on bg reaches target."""
    if ratio(hexv, bg) >= target:
        return hexv
    r, g, b = (int(hexv[i:i + 2], 16) for i in (1, 3, 5))
    f = 0.98
    while f > 0:
        cand = "#%02x%02x%02x" % (round(r * f), round(g * f), round(b * f))
        if ratio(cand, bg) >= target:
            return cand
        f -= 0.02
    return hexv


def load(pid):
    with open(os.path.join(PAL, pid + ".json")) as fh:
        return json.load(fh)


def indent(text):
    return "\n".join("  " + line if line else line
                     for line in text.splitlines())


def css_vars(roles):
    lines = ["%s: %s;" % (VAR[k], roles[k]) for k in ORDER]
    # derived: input hover border and translucent header backdrop
    lines.append("--hover: color-mix(in srgb, var(--text) 20%, var(--bg));")
    lines.append("--backdrop: color-mix(in srgb, var(--bg) 85%, transparent);")
    return "\n  ".join(lines)


notes = []
blocks = []
for slug, label, light_id, dark_id, prov in FAMILIES:
    dark = load(dark_id)["roles"]
    if light_id:
        roles = dict(load(light_id)["roles"])
        bg = roles["bg"]
        surface = roles["surface"]
        # darken every role that misses 4.5:1 on bg (fg handled separately,
        # it must pass on both bg and surface)
        for k in ORDER:
            if k in ("bg", "bg_alt", "surface", "border", "fg"):
                continue
            if ratio(roles[k], bg) < 4.5:
                old = roles[k]
                roles[k] = darken(roles[k], bg)
                notes.append("%s: %s %s -> %s (%.2f -> %.2f)"
                             % (light_id, k, old, roles[k],
                                ratio(old, bg), ratio(roles[k], bg)))
        if ratio(roles["fg"], surface) < 4.5:
            old = roles["fg"]
            roles["fg"] = darken(roles["fg"], surface)
            notes.append("%s: fg %s -> %s (on surface %.2f -> %.2f)"
                         % (light_id, old, roles["fg"],
                            ratio(old, surface), ratio(roles["fg"], surface)))
        light = roles
    else:
        light = None

    sel = 'body[data-theme="%s"]' % slug
    dark_rule = "%s {\n  color-scheme: dark;\n  %s\n}" % (sel, css_vars(dark))
    if light:
        light_rule = "%s {\n  color-scheme: light;\n  %s\n}" % (sel, css_vars(light))
        blocks.append("""/* %s — %s */
%s

@media (prefers-color-scheme: light) {
%s
}

%s:has(#theme:checked) {
  color-scheme: light;
  %s
}

@media (prefers-color-scheme: light) {
%s:has(#theme:checked) {
  color-scheme: dark;
  %s
}
}""" % (label, prov, dark_rule, indent(light_rule), sel, css_vars(light),
        sel, css_vars(dark)))
    else:
        # no light variant: the toggle/browser preference cannot invert,
        # keep color-scheme dark in every state
        blocks.append("""/* %s — %s */
%s

/* dark-only theme: the toggle cannot invert, hold color-scheme dark */
%s:has(#theme:checked) {
  color-scheme: dark;
}
@media (prefers-color-scheme: light) {
%s,
%s:has(#theme:checked) {
  color-scheme: dark;
}
}""" % (label, prov, dark_rule, sel, sel, sel))

with open(OUT, "w") as fh:
    fh.write("""/* Theme palettes, generated by gen-themes.py from palettes/*.json —
   do not edit by hand. One merged light/dark block per theme; the
   checkbox toggle and the browser preference pick the mode. Light
   variants use roles darkened to >= 4.5:1 on their bg. */

""")
    fh.write("\n\n".join(blocks))
    fh.write("\n")

print("wrote", OUT)
for n in notes:
    print("darkened:", n)
