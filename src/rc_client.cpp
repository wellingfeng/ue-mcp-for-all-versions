// ue-mcp-for-all-versions — RemoteControl HTTP client implementation.
#include "ue_mcp_for_all_versions/rc_client.hpp"

#include <memory>

// cpp-httplib pulls in winsock on Windows; keep its include local to this TU.
#include <httplib.h>

namespace ue_mcp_for_all_versions {

struct RcClient::Impl {
    std::unique_ptr<httplib::Client> http;
};

RcClient::RcClient(RcConfig config)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)) {}

RcClient::~RcClient() = default;

namespace {
// Parse an httplib response into an RcResult, decoding JSON when present.
RcResult to_result(const httplib::Result& res) {
    if (!res) {
        return RcResult::fail("connection failed: " + httplib::to_string(res.error()));
    }
    RcResult r;
    r.status = res->status;
    r.ok = res->status >= 200 && res->status < 300;
    if (!res->body.empty()) {
        r.body = json::parse(res->body, nullptr, /*allow_exceptions=*/false);
        if (r.body.is_discarded()) {
            r.body = json{{"raw", res->body}};
        }
    }
    if (!r.ok) {
        r.error = "HTTP " + std::to_string(res->status);
    }
    return r;
}
}  // namespace

bool RcClient::connect() {
    attempted_once_ = true;
    last_attempt_ = std::chrono::steady_clock::now();
    for (int port : config_.ports) {
        auto client = std::make_unique<httplib::Client>(config_.host, port);
        client->set_connection_timeout(config_.connect_timeout);
        client->set_read_timeout(config_.read_timeout);
        client->set_keep_alive(true);

        // 4.26+: GET /remote/info answers 200. 4.25 lacks it (404) but the
        // server is still live, so a 404 ALSO proves a reachable RC server.
        auto res = client->Get("/remote/info");
        if (res) {
            impl_->http = std::move(client);
            connected_ = true;
            active_port_ = port;
            ++generation_;  // signal callers to (re)probe capabilities
            return true;
        }
    }
    connected_ = false;
    return false;
}

bool RcClient::ensure_connected() {
    if (connected_ && impl_->http) return true;
    // Throttle: if we recently failed, don't re-probe on every single call.
    if (attempted_once_) {
        auto since = std::chrono::steady_clock::now() - last_attempt_;
        if (since < config_.reconnect_throttle) return false;
    }
    return connect();
}

RcResult RcClient::request(const std::string& verb, const std::string& path,
                           const json& body) {
    if (!ensure_connected()) {
        return RcResult::fail(
            "not connected to a RemoteControl server (is the engine running "
            "with RemoteControl's web server started?)");
    }
    const std::string payload = body.is_null() ? std::string("{}") : body.dump();

    auto send_once = [&]() -> httplib::Result {
        if (verb == "GET") return impl_->http->Get(path.c_str());
        if (verb == "PUT") return impl_->http->Put(path.c_str(), payload, "application/json");
        if (verb == "POST") return impl_->http->Post(path.c_str(), payload, "application/json");
        if (verb == "DELETE") return impl_->http->Delete(path.c_str(), payload, "application/json");
        return httplib::Result(nullptr, httplib::Error::Unknown);
    };

    if (verb != "GET" && verb != "PUT" && verb != "POST" && verb != "DELETE") {
        return RcResult::fail("unsupported HTTP verb: " + verb);
    }

    httplib::Result res = send_once();

    // A null result means the socket dropped — the editor may have been closed,
    // restarted, or the RC server was momentarily not ready right after
    // StartServer. Invalidate, force a fresh reconnect, and retry ONCE so a
    // long-lived server self-heals transparently for the caller.
    if (!res) {
        connected_ = false;
        impl_->http.reset();
        attempted_once_ = false;  // bypass the reconnect throttle for this retry
        if (ensure_connected()) {
            res = send_once();
        }
        if (!res) {
            connected_ = false;
            impl_->http.reset();
            return RcResult::fail("connection lost: " + httplib::to_string(res.error()) +
                                  " (will attempt reconnect on next request)");
        }
    }
    return to_result(res);
}

RcResult RcClient::call_function(const std::string& object_path,
                                 const std::string& function_name,
                                 const json& parameters,
                                 bool generate_transaction) {
    json body = {
        {"ObjectPath", object_path},
        {"FunctionName", function_name},
        {"Parameters", parameters.is_null() ? json::object() : parameters},
        {"GenerateTransaction", generate_transaction},
    };
    return request("PUT", "/remote/object/call", body);
}

RcResult RcClient::get_property(const std::string& object_path,
                                const std::string& property_name) {
    json body = {
        {"ObjectPath", object_path},
        {"PropertyName", property_name},
        {"Access", "READ_ACCESS"},
    };
    return request("PUT", "/remote/object/property", body);
}

RcResult RcClient::set_property(const std::string& object_path,
                                const std::string& property_name,
                                const json& value,
                                bool generate_transaction) {
    json body = {
        {"ObjectPath", object_path},
        {"PropertyName", property_name},
        {"Access", generate_transaction ? "WRITE_TRANSACTION_ACCESS" : "WRITE_ACCESS"},
        {"PropertyValue", {{property_name, value}}},
        {"GenerateTransaction", generate_transaction},
    };
    return request("PUT", "/remote/object/property", body);
}

RcResult RcClient::get_info() { return request("GET", "/remote/info"); }

}  // namespace ue_mcp_for_all_versions
