CREATE EXTENSION IF NOT EXISTS vector;

CREATE TABLE wallpapers (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    title           text NOT NULL,
    slug            text NOT NULL UNIQUE,
    image           text NOT NULL UNIQUE,
    thumbnail       text NOT NULL UNIQUE,
    mime_type       text NOT NULL CHECK (mime_type IN ('image/jpeg', 'image/png', 'image/webp', 'image/gif')),
    file_size       integer NOT NULL CHECK (file_size > 0),
    width           integer NOT NULL CHECK (width > 0),
    height          integer NOT NULL CHECK (height > 0),
    checksum        bytea NOT NULL UNIQUE,
    embedding       vector(512),
    download_count  bigint NOT NULL DEFAULT 0 CHECK (download_count >= 0),
    created_at      timestamptz NOT NULL
);

CREATE INDEX idx_wallpapers_created ON wallpapers (created_at DESC);
CREATE INDEX idx_wallpapers_embedding ON wallpapers USING hnsw (embedding vector_cosine_ops);
