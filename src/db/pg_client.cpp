#include "db/pg_client.h"
#include "ingest/standard_meta.h"   // normalize_standard_code
#include <pqxx/pqxx>

PgClient::PgClient(std::string conninfo) : conninfo_(std::move(conninfo)) {}

void PgClient::apply_schema(const std::string& schema_sql) {
    pqxx::connection c(conninfo_);
    pqxx::work tx(c);
    tx.exec(schema_sql);
    tx.commit();
}

bool PgClient::ping() {
    try {
        pqxx::connection c(conninfo_);
        pqxx::work tx(c);
        auto r = tx.exec("SELECT 1");
        return r.size() == 1;
    } catch (const std::exception&) {
        return false;
    }
}

void PgClient::upsert_standard(const StandardRow& s) {
    pqxx::connection c(conninfo_);
    pqxx::work tx(c);
    tx.exec(
        "INSERT INTO standards(standard_id,standard_no,standard_name,status,file_path) "
        "VALUES($1,$2,$3,$4,$5) ON CONFLICT (standard_id) DO UPDATE SET "
        "standard_no=EXCLUDED.standard_no, standard_name=EXCLUDED.standard_name, "
        "status=EXCLUDED.status, file_path=EXCLUDED.file_path",
        pqxx::params{s.standard_id, s.standard_no, s.standard_name, s.status, s.file_path});
    tx.commit();
}

void PgClient::insert_clause(const ClauseRow& c) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    tx.exec(
        "INSERT INTO clause_nodes(node_id,standard_id,clause_no,title,path,text,page_start) "
        "VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT (node_id) DO UPDATE SET "
        "text=EXCLUDED.text, clause_no=EXCLUDED.clause_no, path=EXCLUDED.path",
        pqxx::params{c.node_id, c.standard_id, c.clause_no, c.title, c.path, c.text, c.page_start});
    tx.commit();
}

std::optional<ClauseRow> PgClient::get_clause(const std::string& node_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec(
        "SELECT node_id,standard_id,clause_no,title,path,text,COALESCE(page_start,0) "
        "FROM clause_nodes WHERE node_id=$1", pqxx::params{node_id});
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    ClauseRow c;
    c.node_id = row[0].c_str(); c.standard_id = row[1].c_str();
    c.clause_no = row[2].c_str(); c.title = row[3].c_str();
    c.path = row[4].c_str(); c.text = row[5].c_str();
    c.page_start = row[6].as<int>();
    return c;
}

std::optional<StandardRow> PgClient::get_standard(const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec(
        "SELECT standard_id,standard_no,standard_name,status,COALESCE(file_path,'') "
        "FROM standards WHERE standard_id=$1", pqxx::params{standard_id});
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    StandardRow s;
    s.standard_id = row[0].c_str(); s.standard_no = row[1].c_str();
    s.standard_name = row[2].c_str(); s.status = row[3].c_str();
    s.file_path = row[4].c_str();
    return s;
}

int PgClient::delete_chunks_by_standard(const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec("DELETE FROM retrieval_chunks WHERE standard_id=$1",
                     pqxx::params{standard_id});
    tx.commit();
    return static_cast<int>(r.affected_rows());
}

void PgClient::insert_chunk(const RetrievalChunkRow& c) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    tx.exec(
        "INSERT INTO retrieval_chunks(chunk_id,node_id,standard_id,chunk_type,"
        "clause_no,method_no,title,path_text,atomic_text,embedding_text,context_text,"
        "captions,formulas,page_start,page_end,has_table,has_formula,has_figure,suspect,bm25_text) "
        "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12::jsonb,$13::jsonb,$14,$15,$16,$17,$18,$19,$20) "
        "ON CONFLICT (chunk_id) DO UPDATE SET "
        "node_id=EXCLUDED.node_id, standard_id=EXCLUDED.standard_id, "
        "chunk_type=EXCLUDED.chunk_type, clause_no=EXCLUDED.clause_no, "
        "method_no=EXCLUDED.method_no, title=EXCLUDED.title, path_text=EXCLUDED.path_text, "
        "atomic_text=EXCLUDED.atomic_text, embedding_text=EXCLUDED.embedding_text, "
        "context_text=EXCLUDED.context_text, captions=EXCLUDED.captions, "
        "formulas=EXCLUDED.formulas, page_start=EXCLUDED.page_start, "
        "page_end=EXCLUDED.page_end, has_table=EXCLUDED.has_table, "
        "has_formula=EXCLUDED.has_formula, has_figure=EXCLUDED.has_figure, "
        "suspect=EXCLUDED.suspect, bm25_text=EXCLUDED.bm25_text",
        pqxx::params{c.chunk_id, c.node_id, c.standard_id, c.chunk_type,
                     c.clause_no, c.method_no, c.title, c.path_text,
                     c.atomic_text, c.embedding_text, c.context_text,
                     c.captions_json, c.formulas_json, c.page_start, c.page_end,
                     c.has_table, c.has_formula, c.has_figure, c.suspect, c.bm25_text});
    tx.commit();
}

std::optional<RetrievalChunkRow> PgClient::get_chunk(const std::string& chunk_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec(
        "SELECT chunk_id,node_id,standard_id,COALESCE(chunk_type,''),"
        "COALESCE(clause_no,''),COALESCE(method_no,''),COALESCE(title,''),"
        "COALESCE(path_text,''),COALESCE(atomic_text,''),COALESCE(embedding_text,''),"
        "COALESCE(context_text,''),COALESCE(captions::text,'[]'),"
        "COALESCE(formulas::text,'[]'),COALESCE(page_start,0),COALESCE(page_end,0),"
        "COALESCE(has_table,FALSE),COALESCE(has_formula,FALSE),"
        "COALESCE(has_figure,FALSE),COALESCE(suspect,''),COALESCE(bm25_text,'') "
        "FROM retrieval_chunks WHERE chunk_id=$1", pqxx::params{chunk_id});
    if (r.empty()) return std::nullopt;
    auto row = r[0];
    RetrievalChunkRow c;
    c.chunk_id = row[0].c_str(); c.node_id = row[1].c_str();
    c.standard_id = row[2].c_str(); c.chunk_type = row[3].c_str();
    c.clause_no = row[4].c_str(); c.method_no = row[5].c_str();
    c.title = row[6].c_str(); c.path_text = row[7].c_str();
    c.atomic_text = row[8].c_str(); c.embedding_text = row[9].c_str();
    c.context_text = row[10].c_str(); c.captions_json = row[11].c_str();
    c.formulas_json = row[12].c_str(); c.page_start = row[13].as<int>();
    c.page_end = row[14].as<int>(); c.has_table = row[15].as<bool>();
    c.has_formula = row[16].as<bool>(); c.has_figure = row[17].as<bool>();
    c.suspect = row[18].c_str();
    c.bm25_text = row[19].c_str();
    return c;
}

std::string PgClient::find_standard_by_code(const std::string& code) {
    // 归一化两侧后做子串匹配，吸收 OCR/文件名/排版的 斜杠·破折号·空格·全角 变体
    //（如 gold "JTG/T 3650-2020" 命中库内文件名退化形 "…(JTGT 3650—2020）"）。现行优先。
    std::string key = normalize_standard_code(code);
    if (key.empty()) return "";
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    auto r = tx.exec("SELECT standard_id, standard_no, (status='现行') AS cur FROM standards");
    std::string best;
    bool best_cur = false;
    for (auto row : r) {
        if (normalize_standard_code(std::string(row[1].c_str())).find(key) == std::string::npos) continue;
        bool cur = row[2].as<bool>();
        if (best.empty() || (cur && !best_cur)) { best = std::string(row[0].c_str()); best_cur = cur; }
    }
    return best;
}

std::vector<RetrievalChunkRow> PgClient::chunks_by_method(const std::string& method_prefix,
                                                          const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    pqxx::result r;
    if (standard_id.empty()) {
        r = tx.exec(
            "SELECT chunk_id,standard_id FROM retrieval_chunks "
            "WHERE method_no LIKE $1 ORDER BY clause_no",
            pqxx::params{method_prefix + "%"});
    } else {
        r = tx.exec(
            "SELECT chunk_id,standard_id FROM retrieval_chunks "
            "WHERE method_no LIKE $1 AND standard_id=$2 ORDER BY clause_no",
            pqxx::params{method_prefix + "%", standard_id});
    }
    std::vector<RetrievalChunkRow> out;
    for (auto row : r) {
        RetrievalChunkRow c;
        c.chunk_id = row[0].c_str();
        c.standard_id = row[1].c_str();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::string> PgClient::chunk_ids_by_clause(const std::string& clause_no,
                                                       const std::string& standard_id) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    pqxx::result r;
    if (standard_id.empty()) {
        r = tx.exec(
            "SELECT chunk_id FROM retrieval_chunks WHERE clause_no=$1",
            pqxx::params{clause_no});
    } else {
        r = tx.exec(
            "SELECT chunk_id FROM retrieval_chunks WHERE clause_no=$1 AND standard_id=$2",
            pqxx::params{clause_no, standard_id});
    }
    std::vector<std::string> out;
    for (auto row : r) out.push_back(row[0].c_str());
    return out;
}

std::vector<RetrievalChunkRow> PgClient::chunks_containing(const std::string& keyword,
                                                           const std::string& status) {
    pqxx::connection cn(conninfo_);
    pqxx::work tx(cn);
    pqxx::result r;
    if (status.empty()) {
        r = tx.exec(
            "SELECT chunk_id,standard_id FROM retrieval_chunks "
            "WHERE embedding_text LIKE $1 ORDER BY chunk_id",
            pqxx::params{"%" + keyword + "%"});
    } else {
        r = tx.exec(
            "SELECT rc.chunk_id,rc.standard_id FROM retrieval_chunks rc "
            "JOIN standards s ON rc.standard_id=s.standard_id "
            "WHERE rc.embedding_text LIKE $1 AND s.status=$2 ORDER BY rc.chunk_id",
            pqxx::params{"%" + keyword + "%", status});
    }
    std::vector<RetrievalChunkRow> out;
    for (auto row : r) {
        RetrievalChunkRow c;
        c.chunk_id = row[0].c_str();
        c.standard_id = row[1].c_str();
        out.push_back(std::move(c));
    }
    return out;
}
