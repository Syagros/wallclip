# WallClip (Wallpaper Gallery with CLIP integration) [WiP]
WallClip is written for serving Wallpapers using any web server that can use Unix Domain Sockets. It is currently dependent on a non-standard Linux only header (`_GNU_SOURCE`) but I might make it independent of it if I continue working on it. 

# Depends on:
* [clip.cpp](https://github.com/monatis/clip.cpp)
* [PostgreSQL](https://www.postgresql.org/) with pgvector extension
* [ImageMagick](https://imagemagick.org/)
* libmagic
* OpenSSL
* libpq
* [CLIP model](https://huggingface.co/models?other=clip-cpp-gguf)

# What it does:
* Uses `<MagickWand/MagickWand.h>` to generate a thumbnail, get dimensions and mime type
* Gathers miscellaneous metadata about the file using standard and also a non standard glibc functions (statx() for BTIME in Linux)
* Processes the image using a CLIP model and gets 512-dimensional vector embedding
* Stores everything into a postgresql database.
```sh
wallclip FILE FILE2 ...
```

It has Unix Domain Sockets support, so without any flags, or with only verbose flag it runs with a server socket.
```sh
wallclip
```

It can query the server socket with -q/--query flag
```sh
wallclip -q "sunset"
```
Expected stdout:
```
/absolute/path/to/.thumbnails/matching-image-1.png
/absolute/path/to/.thumbnails/matching-image-2.png
/absolute/path/to/.thumbnails/matching-image-3.png
/absolute/path/to/.thumbnails/matching-image-4.png
/absolute/path/to/.thumbnails/matching-image-5.png
``` 
The server also answers plain HTTP `GET` requests on the same socket (queries are lowercased and URL-decoded, at most 100 results):
```sh
curl --unix-socket /run/wallclip/socket "http://localhost/red+sunset"
```
Why does it return thumbnail paths? Because I wrote it to show thumbnail previews on queries on a webpage.

It can delete image from filesystem and database (WiP)
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

1. Convert the model to GGUF for clip.cpp and find its `projection_dim` (in the model's `config.json`; ViT-B/32 is 512, ViT-L/14 is 768).
2. Create a new database with that dimension (default 512):
```sh
createdb wallpapers_new
psql -d wallpapers_new -v embed_dim=768 -f schema.sql
```
3. Point wallclip at both, in `.env` or the environment. `WALLCLIP_ENV_FILE=.env.new wallclip ...` switches setups without editing `.env`:
```sh
WALLCLIP_MODEL_PATH=/path/to/new-model.gguf
WALLCLIP_DB_NAME=wallpapers_new
```
4. Add the images again, embeddings from different models are not comparable:
```sh
wallclip /path/to/wallpapers/*
```
5. Retune `WALLCLIP_SIMILARITY_THRESHOLD`, the default 0.27 was picked for the default model.

Notes:
* An `embed_dim` that doesn't match the model makes Postgres reject inserts and queries.
* pgvector's HNSW index supports at most 2000 dimensions.
* To run the old and new servers side by side, give the new one its own `WALLCLIP_SOCKET_PATH`.

# TODO:
* Make code portable
* Better slugify()
* To implement -d/--delete: Rewrite the read/write logic of socket functions
