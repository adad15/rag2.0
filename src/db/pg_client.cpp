#include "db/pg_client.h"
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
