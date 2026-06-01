CREATE TABLE IF NOT EXISTS standards (
    standard_id   TEXT PRIMARY KEY,
    standard_no   TEXT,
    standard_name TEXT,
    status        TEXT DEFAULT '现行',
    file_path     TEXT,
    created_at    TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS clause_nodes (
    node_id     TEXT PRIMARY KEY,
    standard_id TEXT REFERENCES standards(standard_id),
    clause_no   TEXT,
    title       TEXT,
    path        TEXT,
    text        TEXT,
    page_start  INT
);

CREATE INDEX IF NOT EXISTS idx_clause_standard ON clause_nodes(standard_id);
