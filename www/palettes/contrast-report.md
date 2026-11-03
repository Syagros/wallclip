# Contrast report (WCAG 2.x, foreground role on `bg`)

Ratios below **4.5** fail AA for normal text (marked `!`). Below 3.0 fail even for large text/UI (`!!`).
Accent hues are often fine for large text, icons, and borders at 3.0+ even when they miss 4.5; this report just tells you which roles need a darker/lighter variant before using them for body text.

| variant | fg | fg_muted | accent | red | orange | yellow | green | cyan | blue | purple | pink | fg_on_surface |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| catppuccin-frappe | 8.06 | 5.55 | 5.6 | 4.65 | 5.8 | 7.62 | 7.1 | 6.41 | 5.34 | 5.6 | 7.52 | 6.19 |
| catppuccin-latte | 7.06 | 4.37 ! | 4.79 | 4.8 | 2.64 !! | 2.31 !! | 2.96 !! | 3.31 ! | 4.34 ! | 4.79 | 2.34 !! | 5.17 |
| catppuccin-macchiato | 9.92 | 6.62 | 6.84 | 5.96 | 7.62 | 10.2 | 9.17 | 8.74 | 6.57 | 6.84 | 9.33 | 7.55 |
| catppuccin-mocha | 11.34 | 7.37 | 8.07 | 7.08 | 9.27 | 12.91 | 11.03 | 11.01 | 7.79 | 8.07 | 10.74 | 8.69 |
| dracula-alucard | 15.89 | 5.56 | 6.02 | 4.85 | 5.58 | 4.8 | 5.96 | 5.78 | 6.02 | 6.02 | 7.33 | 10.7 |
| dracula-dracula | 13.36 | 3.03 ! | 5.9 | 4.53 | 8.36 | 12.74 | 10.38 | 10.29 | 5.9 | 5.9 | 5.97 | 8.59 |
| gruvbox-dark-hard | 11.95 | 5.9 | 6.49 | 4.77 | 6.49 | 9.67 | 7.94 | 7.79 | 6.09 | 5.98 | 5.98 | 6.43 |
| gruvbox-dark-medium | 10.75 | 5.3 | 5.84 | 4.29 ! | 5.84 | 8.69 | 7.14 | 7.01 | 5.48 | 5.37 | 5.37 | 6.43 |
| gruvbox-dark-soft | 9.57 | 4.72 | 5.2 | 3.82 ! | 5.2 | 7.74 | 6.36 | 6.24 | 4.88 | 4.78 | 4.78 | 6.43 |
| gruvbox-light-hard | 10.53 | 4.42 ! | 5.56 | 7.83 | 5.56 | 3.43 ! | 4.42 ! | 4.53 | 6.0 | 6.12 | 6.12 | 6.76 |
| gruvbox-light-medium | 10.22 | 4.29 ! | 5.4 | 7.6 | 5.4 | 3.33 ! | 4.29 ! | 4.4 ! | 5.82 | 5.94 | 5.94 | 6.76 |
| gruvbox-light-soft | 9.23 | 3.87 ! | 4.87 | 6.86 | 4.87 | 3.0 ! | 3.87 ! | 3.97 ! | 5.25 | 5.36 | 5.36 | 6.76 |
| nord-dark | 9.25 | 2.43 !! | 6.24 | 3.05 ! | 4.39 ! | 8.0 | 6.13 | 6.24 | 4.64 | 4.41 ! | 4.41 ! | 6.39 |
| nord-light | 10.84 | 6.4 | 3.5 ! | 3.55 ! | 2.47 !! | 1.35 !! | 1.77 !! | 1.81 !! | 3.5 ! | 2.46 !! | 2.46 !! | 10.26 |
| tokyonight-day | 4.52 | 3.71 ! | 3.11 ! | 3.01 ! | 3.69 ! | 3.75 ! | 4.04 ! | 4.26 ! | 3.11 ! | 4.73 | 3.33 ! | 3.52 ! |
| tokyonight-moon | 10.26 | 3.67 ! | 6.66 | 5.9 | 7.16 | 9.96 | 11.1 | 10.33 | 6.66 | 8.63 | 6.73 | 8.28 |
| tokyonight-night | 10.59 | 4.1 ! | 6.79 | 6.46 | 8.4 | 8.55 | 9.35 | 9.96 | 6.79 | 5.13 | 7.39 | 8.32 |
| tokyonight-storm | 9.02 | 3.49 ! | 5.78 | 5.51 | 7.16 | 7.28 | 7.97 | 8.49 | 5.78 | 4.38 ! | 6.3 | 8.32 |

## Variants needing attention (any role < 4.5)

- **catppuccin-latte**: fg_muted 4.37, orange 2.64, yellow 2.31, green 2.96, cyan 3.31, blue 4.34, pink 2.34
- **dracula-dracula**: fg_muted 3.03
- **gruvbox-dark-medium**: red 4.29
- **gruvbox-dark-soft**: red 3.82
- **gruvbox-light-hard**: fg_muted 4.42, yellow 3.43, green 4.42
- **gruvbox-light-medium**: fg_muted 4.29, yellow 3.33, green 4.29, cyan 4.4
- **gruvbox-light-soft**: fg_muted 3.87, yellow 3.0, green 3.87, cyan 3.97
- **nord-dark**: fg_muted 2.43, red 3.05, orange 4.39, purple 4.41, pink 4.41
- **nord-light**: accent 3.5, red 3.55, orange 2.47, yellow 1.35, green 1.77, cyan 1.81, blue 3.5, purple 2.46, pink 2.46
- **tokyonight-day**: fg_muted 3.71, accent 3.11, red 3.01, orange 3.69, yellow 3.75, green 4.04, cyan 4.26, blue 3.11, pink 3.33, fg_on_surface 3.52
- **tokyonight-moon**: fg_muted 3.67
- **tokyonight-night**: fg_muted 4.1
- **tokyonight-storm**: fg_muted 3.49, purple 4.38
