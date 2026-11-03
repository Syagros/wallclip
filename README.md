# WallClip (Generic Gallery with CLIP integration)
WallClip is written for serving images using nginx, although it should be usable with other webservers too.

# Features:
* **Search images by describing them.** Type "red sunset over mountains" and get matches by meaning, not filenames or tags. Uses CLIP embeddings and pgvector cosine similarity.
* **Zero JavaScript.** The web UI is plain HTML and CSS served by nginx. Search is a normal GET form, and nginx SSI pulls results from the wallclip socket into the page.
* **Themes without JS.** Default, Default Light, Gruvbox (dark and light), Catppuccin and Tokyo Night. The choice is stored in a cookie set by nginx.
* **Fast, lazy-loaded thumbnail grid.** Results are thumbnails that link to the full-resolution image. The landing page shows a grid of your whole collection.
* **Runs on a Unix socket.** Any web server that can proxy to a Unix socket works (nginx config included). The same socket answers plain HTTP GET requests and a `-q` CLI query.
* **Simple CLI for management.**
  * `wallclip FILE...` adds images: thumbnail, dimensions, MIME type, metadata and embedding, all stored in PostgreSQL.
  * `wallclip -d FILE` removes an image from both the database and the disk.
  * `wallclip -q "text"` searches from the terminal.
* **Swappable model.** Any CLIP model converted to GGUF works. The embedding dimension is read from the model, and `schema.sql` takes an `embed_dim` parameter.
* **Sensible hardening.** The nginx config includes a strict CSP, a search rate limit (10 requests/min per IP), a 10-minute result cache, and `Secure`/`SameSite` cookies. DB credentials are wiped from memory after database authentication.
* **Portable.** Linux, macOS and FreeBSD, with unit tests in CI (pure ai slop I haven't even looked at them properly but they _seem_ to work).


# Depends on:
* [clip.cpp](https://github.com/Syagros/clip.cpp)
* [PostgreSQL](https://www.postgresql.org/) with pgvector extension
* [ImageMagick 7](https://imagemagick.org/)
* libmagic
* OpenSSL
* libpq
* [CLIP model](https://huggingface.co/models?other=clip-cpp-gguf)

Build tools: C compiler, GNU make (`gmake` on FreeBSD), pkg-config, CMake, git

Optional, for the web UI: nginx with the `ssi` and `sub_filter` modules.

# What it does:
* Generates a thumbnail (fits within 640x360, never upscaled) in a `.thumbnails` directory next to the image, using `<MagickWand/MagickWand.h>`, and reads dimensions and MIME type
* Collects file metadata, including the creation time (`statx()` on Linux, `st_birthtime` on macOS/BSD, mtime as a fallback)
* Embeds the image with a CLIP model (512 dimensions for the default model)
* Stores everything in PostgreSQL

## Add images
```sh
wallclip FILE FILE2 ...
```

## Run the server
Without arguments (or with only `-v`) it runs a server on a Unix Domain Socket:
```sh
wallclip
```

## Search
From the command line, with `-q`/`--query`, which prints one thumbnail path per match:
```sh
wallclip -q "sunset"
```
```
/absolute/path/to/.thumbnails/matching-image-1.jpg
/absolute/path/to/.thumbnails/matching-image-2.png
```

Or over plain HTTP `GET` on the same socket. Queries are lowercased and URL-decoded, and results are capped at 100 by default (`WALLCLIP_MAX_MATCHES`):
```sh
curl --unix-socket /run/wallclip/socket "http://localhost/red+sunset"
```
Each line is the full-resolution path, a tab, then the thumbnail path, which lets a web page link the thumbnail to the original. The bundled nginx config uses this to build the search page.

## Delete images
Removes the database row first, then the image and its thumbnail from disk. Repeat `-d` for several files:
```sh
wallclip -d FILE
```

# Usage
```
  wallclip [-v]                  			run server in foreground
  wallclip [-v] FILE...          			add images (space separated)
  wallclip [-v] -d FILE [-d FILE2 ...]      delete images (both, db and fs)
  wallclip [-v] -q, --query STR       		query for images

Options:
  -v, --verbose
  -d, --delete
  -h, --help
```

# Configuration:
Settings are read with the precedence `wallclip.h` defaults < `.env` file < shell environment variables. The `.env` file is read from the current directory, or from the path in `WALLCLIP_ENV_FILE` (shell only). See `.env.example` for all keys:
```sh
cp .env.example .env && chmod 600 .env
```
Database credentials are zeroed from memory as soon as the PostgreSQL connection is authenticated.

## Database
wallclip needs a PostgreSQL database with the pgvector extension and the table from `schema.sql`. The role and database names below match `.env.example`:
```sh
sudo -u postgres createuser --pwprompt wallclip
sudo -u postgres createdb -O wallclip wallclip
sudo -u postgres psql -d wallclip -c 'CREATE EXTENSION IF NOT EXISTS vector'
psql -h localhost -U wallclip -d wallclip -f schema.sql
```
The extension is created as a superuser first, since the `wallclip` role usually can't. Running `schema.sql` as `wallclip` makes it the owner of the table. For a model that isn't 512-dimensional, pass `-v embed_dim=N` (see below).

Then set the connection in `.env`:
```sh
WALLCLIP_DB_NAME=wallclip
WALLCLIP_DB_USER=wallclip
WALLCLIP_DB_PASSWORD=...
# WALLCLIP_DB_HOST=/var/run/postgresql   # socket directory or hostname
# WALLCLIP_DB_PORT=5432
```
Unset or empty values fall back to libpq's defaults and the `PG*` variables (`PGHOST`, `PGPASSFILE`, ...), so peer authentication over the local socket works with only `WALLCLIP_DB_NAME` set.

# Using a different model:
The embedding dimension is read from the model at runtime, but the database column must match it.

1. Convert the model to GGUF for clip.cpp and find its `projection_dim` (in the model's `config.json`).
2. Create a new database with that dimension (default 512):
```sh
createdb wallclip_768
sudo -u postgres psql -d wallclip_768 -c 'CREATE EXTENSION IF NOT EXISTS vector'
psql -d wallclip_768 -v embed_dim=768 -f schema.sql
```
3. Point wallclip at both, in `.env` or the environment. `WALLCLIP_ENV_FILE=.env.new wallclip ...` switches setups without editing `.env`:
```sh
WALLCLIP_MODEL_PATH=/path/to/new-model-of-768-dims.gguf
WALLCLIP_DB_NAME=wallclip_768
```
4. Add the images again, embeddings from different models are not comparable:
```sh
wallclip /path/to/images/*
```
5. Retune `WALLCLIP_SIMILARITY_THRESHOLD`, the default 0.27 was picked for the default model.

Notes:
* An `embed_dim` that doesn't match the model makes Postgres reject inserts and queries.
* pgvector's HNSW index supports at most 2000 dimensions.
* To run the old and new servers side by side, give the new one its own `WALLCLIP_SOCKET_PATH`.

# Licence
GPL-3.0-or-later
