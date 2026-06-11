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

CREATE TABLE IF NOT EXISTS retrieval_chunks (
    chunk_id       TEXT PRIMARY KEY,
    -- node_id 是对树节点的软引用，特意不加外键：M2c-3 起 clause_nodes 停写，
    -- 新灌标准的 node_id 不会出现在该表中
    node_id        TEXT,
    standard_id    TEXT REFERENCES standards(standard_id),
    chunk_type     TEXT,
    clause_no      TEXT,
    method_no      TEXT,
    title          TEXT,
    path_text      TEXT,
    atomic_text    TEXT,
    embedding_text TEXT,
    context_text   TEXT,
    captions       JSONB DEFAULT '[]',
    formulas       JSONB DEFAULT '[]',
    page_start     INT,
    page_end       INT,
    has_table      BOOLEAN DEFAULT FALSE,
    has_formula    BOOLEAN DEFAULT FALSE,
    has_figure     BOOLEAN DEFAULT FALSE,
    suspect        TEXT
);

CREATE INDEX IF NOT EXISTS idx_chunks_standard ON retrieval_chunks(standard_id);
CREATE INDEX IF NOT EXISTS idx_chunks_node ON retrieval_chunks(node_id);
