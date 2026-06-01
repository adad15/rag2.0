#include "http/http_client.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace http {

static httplib::Headers to_headers(const std::map<std::string, std::string>& h) {
    httplib::Headers out;
    for (auto& kv : h) out.emplace(kv.first, kv.second);
    return out;
}

Response post_json(const std::string& base_url, const std::string& path,
                   const std::string& body,
                   const std::map<std::string, std::string>& headers) {
    httplib::Client cli(base_url.c_str());
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(120, 0);
    cli.enable_server_certificate_verification(false);  // M1 阶段简化；生产应开启
    auto res = cli.Post(path.c_str(), to_headers(headers), body, "application/json");
    Response r;
    if (!res) { r.status = 0; r.error = httplib::to_string(res.error()); return r; }
    r.status = res->status; r.body = res->body; return r;
}

Response get_json(const std::string& base_url, const std::string& path,
                  const std::map<std::string, std::string>& headers) {
    httplib::Client cli(base_url.c_str());
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(60, 0);
    cli.enable_server_certificate_verification(false);
    auto res = cli.Get(path.c_str(), to_headers(headers));
    Response r;
    if (!res) { r.status = 0; r.error = httplib::to_string(res.error()); return r; }
    r.status = res->status; r.body = res->body; return r;
}

}  // namespace http
