# theme-palettes

Normalized color palettes for **Gruvbox, Catppuccin, Dracula, Nord, Tokyo Night**: 18 variants, one JSON each.
Every hex value was parsed by `scripts/extract.py` from an upstream file pinned to an exact commit (kept in `raw/`).
No hex value was typed by hand or produced by an LLM.

## Variants

| theme | variants (file ids) | upstream source |
|---|---|---|
| Gruvbox (6) | `gruvbox-{dark,light}-{hard,medium,soft}` | `morhetz/gruvbox` `colors/gruvbox.vim` @ `ef8864b` |
| Catppuccin (4) | `catppuccin-{latte,frappe,macchiato,mocha}` | `catppuccin/palette` `palette.json` @ `07d02aa` |
| Dracula (2) | `dracula-dracula`, `dracula-alucard` | `dracula/dracula-theme` README (OSS tables) @ `8d33e54` |
| Nord (2) | `nord-dark`, `nord-light` (see caveats) | `nordtheme/nord` `src/nord.css` @ `1cef716` |
| Tokyo Night (4) | `tokyonight-{night,storm,moon,day}` | `folke/tokyonight.nvim` `extras/lua/*.lua` @ `cdc07ac` |

Full commit SHAs, licenses and retrieval date are inside every JSON (`source`) and in `index.json`.

## File layout

```
index.json            variant list, role schema, pinned sources
palettes/<theme>/<id>.json
raw/                  pinned upstream files the JSON was parsed from (audit trail)
scripts/extract.py    raw/ -> palettes/   (deterministic; the ONLY hand-written data is ROLE_MAPS)
scripts/validate.py   hex format, role completeness, upstream counts, spot-checks
scripts/contrast.py   WCAG report -> contrast-report.md / contrast.json
```

## JSON shape

```jsonc
{
  "id": "gruvbox-dark-hard", "theme": "gruvbox", "variant": "dark-hard", "mode": "dark",
  "source": { "repo": "...", "commit": "<sha>", "file": "...", "license": "...", "retrieved": "..." },
  "roles":        { "bg": "#1d2021", "bg_alt": "...", ... },   // 15 fixed roles, feed your template
  "role_sources": { "bg": "bg0", ... },                        // which native color each role came from
  "role_fallbacks": { "pink": "gruvbox has no pink; reuses purple" },
  "native":       { ... },                                      // the theme's own names, untouched
  // theme-specific extras: Catppuccin "ansi", Tokyo Night "nested" + "rainbow", Gruvbox "all_accent_sets"
}
```

Roles (identical keys, in this order, in every file):
`bg, bg_alt, surface, border, fg, fg_muted, red, orange, yellow, green, cyan, blue, purple, pink, accent`

Your template only ever reads `roles`, so adding a theme = one new JSON with those 15 keys.

## Please review: the role mapping is my judgment

Hex values are mechanical. Which native color feeds which role (`ROLE_MAPS` in `extract.py`) is an editorial choice, e.g.
Gruvbox `accent` = orange, Catppuccin `accent` = mauve, Tokyo Night `border` = `fg_gutter` (upstream `border` is nearly
invisible against `bg`). Change a mapping, rerun `python3 scripts/extract.py && python3 scripts/validate.py`.

## Caveats

- **Nord has one official palette.** `nord-dark`/`nord-light` are its two documented "ambiances"; `nord-light` is
  **derived by this tooling** (Snow Storm bg, Polar Night text), not a separate upstream palette. Its Aurora/Frost
  accents have very low contrast on Snow Storm (yellow 1.35:1), so treat it as a starting point.
- `derived.nord3_bright` (`#616e88`) is **not in nord.css**. It is the commonly used brighter comment color
  (upstream issue nordtheme/nord#94). It is used for `fg_muted`, and still only reaches 2.43:1 on `nord0`.
- **Dracula**: only the open-source palettes (12 colors each). Dracula PRO variants are paid and not public.
  Dracula has no blue, so `blue` reuses purple; Alucard's "Current Line" is a dark color, so `surface` uses Selection.
- **Gruvbox** light mode uses the `faded_*` accents and dark mode the `bright_*` ones, exactly as `gruvbox.vim` does.
  Gruvbox has no pink (reuses purple). `all_accent_sets` also keeps `neutral_*` for terminal-style use.
- **Tokyo Night** values come from the plugin's generated `extras/lua` files (the computed colors, not hand-listed ones).
- Not included: gruvbox-material, Catppuccin/Tokyo Night forks, anything outside the five themes you named.

## Contrast

See `contrast-report.md`. Roles below 4.5:1 on `bg` are flagged; many accent hues in light variants
(Latte, Tokyo Night Day, Nord light) need darkening before use as body-text color. Fine for large text/borders at 3:1+.
