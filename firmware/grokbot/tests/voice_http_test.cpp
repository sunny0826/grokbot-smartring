#include "voice_http.h"
#include "voice_transport.h"
#include "voice_timing.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

const char *rid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int64_t length = -2;
    bool complete = true, open_ok = true, eof = false;
    std::function<void()> during_read;
};
struct HttpClient {
    esp_http_client_config_t config;
    std::string url;
    std::map<std::string, std::string> headers;
    Response response;
    size_t read = 0;
    bool connected = false;
};
struct Request { std::string url; std::map<std::string, std::string> headers; };
std::deque<Response> responses;
std::vector<Request> requests;
GadgetWifiCred credentials{};
esp_event_handler_t wifi_handler;
int connects = 0, live = 0, cleaned = 0;
int64_t now = 1000000;
bool online = true, clock_ready = true, wrong_scope = false;
std::vector<VoiceTimingStage> timings;

bool gadget_clock_ready() { return clock_ready; }
bool gadget_wifi_load(GadgetWifiCred &out) { out = credentials; return true; }
bool voice_scope_matches(const VoiceScope &, const GadgetWifiCred &) { return !wrong_scope; }
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *) { return online ? ESP_OK : -1; }
void esp_read_mac(uint8_t *mac, int) { memset(mac, 1, 6); }
int esp_crt_bundle_attach(void *) { return ESP_OK; }
int64_t esp_timer_get_time() { return now; }
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, esp_event_handler_t handler, void *) {
    wifi_handler = handler; return ESP_OK;
}
void voice_timing_mark(const char *id, VoiceTimingStage stage) { assert(!strcmp(id, rid)); timings.push_back(stage); }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    ++live;
    auto *client = new HttpClient;
    client->config = *config;
    client->url = config->url;
    return client;
}
esp_err_t esp_http_client_cleanup(HttpClient *client) {
    // The SDK retains cert_pem; its storage must remain valid until cleanup.
    if (client->config.cert_pem) assert(!strcmp(client->config.cert_pem, "saved-ca"));
    --live; ++cleaned; delete client; return ESP_OK;
}
esp_err_t esp_http_client_set_header(HttpClient *c, const char *k, const char *v) { c->headers[k] = v; return ESP_OK; }
esp_err_t esp_http_client_delete_header(HttpClient *c, const char *k) { return c->headers.erase(k) ? ESP_OK : ESP_ERR_NOT_FOUND; }
esp_err_t esp_http_client_set_url(HttpClient *c, const char *url) { c->url = url; return ESP_OK; }
esp_err_t esp_http_client_set_method(HttpClient *c, esp_http_client_method_t method) { c->config.method = method; return ESP_OK; }
esp_err_t esp_http_client_open(HttpClient *c, int) {
    assert(!responses.empty());
    if (c->url.find("/response") != std::string::npos)
        assert(c->headers.count("Connection") && c->headers.at("Connection") == "keep-alive");
    requests.push_back({c->url, c->headers});
    c->response = std::move(responses.front()); responses.pop_front(); c->read = 0;
    if (!c->response.open_ok) return -1;
    if (!c->connected) { ++connects; c->connected = true; }
    return ESP_OK;
}
int esp_http_client_write(HttpClient *, const char *, int size) { return size; }
int64_t esp_http_client_fetch_headers(HttpClient *c) {
    for (auto &header : c->response.headers) {
        esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, header.first.data(), header.second.data(), c->config.user_data};
        c->config.event_handler(&event);
    }
    return c->response.length == -2 ? int64_t(c->response.body.size()) : c->response.length;
}
int esp_http_client_get_status_code(HttpClient *c) { return c->response.status; }
int esp_http_client_read_response(HttpClient *c, char *body, int size) {
    if (c->response.during_read) c->response.during_read();
    size_t n = std::min(size_t(size), c->response.body.size());
    memcpy(body, c->response.body.data(), n); c->read = n;
    if (c->response.eof) {
        esp_http_client_event_t event{HTTP_EVENT_DISCONNECTED, nullptr, nullptr, c->config.user_data};
        c->config.event_handler(&event);
        c->connected = false;
    }
    return n;
}
bool esp_http_client_is_complete_data_received(HttpClient *c) { return c->response.complete && c->read == c->response.body.size(); }

Response json(const char *state = "stored", int pages = 0) {
    Response r;
    r.headers = {{"Content-Type", "application/json"}, {"Connection", "keep-alive"}};
    r.body = std::string("{\"request_id\":\"") + rid + "\",\"status\":\"" + state + "\",\"page_count\":" + std::to_string(pages) + ",\"read\":false}";
    return r;
}
void put_le(std::string &data, int offset, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) data[offset + i] = char(value >> (8 * i));
}
Response page() {
    Response r;
    r.headers = {{"Content-Type", "application/octet-stream"}, {"Connection", "keep-alive"},
                 {"X-Voice-Request-ID", rid}, {"X-Voice-Read", "1"}};
    r.body.resize(VOICE_PAGE_HEADER + VOICE_PAGE_BYTES);
    memcpy(r.body.data(), "VPG1", 4);
    put_le(r.body, 4, VOICE_PAGE_WIDTH, 2); put_le(r.body, 6, VOICE_PAGE_HEIGHT, 2); put_le(r.body, 10, 2, 2);
    uint32_t crc = 0xffffffff;
    for (size_t i = VOICE_PAGE_HEADER; i < r.body.size(); ++i) {
        crc ^= uint8_t(r.body[i]);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    put_le(r.body, 12, crc ^ 0xffffffffu, 4);
    return r;
}
void reset() {
    voice_http_invalidate(); assert(live == 0); assert(responses.empty());
    requests.clear(); timings.clear(); connects = cleaned = 0;
    online = clock_ready = true; wrong_scope = false;
    credentials = {}; strcpy(credentials.host, "gateway.local"); credentials.port = 8767;
    strcpy(credentials.ca, "saved-ca"); strcpy(credentials.token, "token");
}
VoiceResponse run(Response response, VoicePhase after = VoicePhase::Stored) {
    responses.push_back(std::move(response)); return voice_response(rid, after, nullptr);
}
void check_bad(Response response) {
    reset(); assert(!run(std::move(response)).reply.ok); assert(live == 0);
    assert(run(json()).reply.ok); assert(connects == 2);
}

int main() {
    reset();
    for (int i = 0; i < 3; ++i) assert(run(json()).reply.ok);
    assert(connects == 1);
    auto result = run(page());
    assert(result.reply.ok && result.reply.phase == VoicePhase::Replied && result.reply.page_count == 2 && result.reply.read && result.page);
    free(result.page); assert(connects == 1); assert(live == 0); assert(timings.size() == 2);
    assert(run(json(), VoicePhase::Idle).reply.ok);
    assert(!requests.back().headers.count("X-Voice-After"));
    assert(!requests.back().headers.count("Content-Type"));
    assert(!requests.back().headers.count("X-Voice-Read-Exclude"));
    assert(run(json(), VoicePhase::Waiting).reply.ok); assert(requests.back().headers.at("X-Voice-After") == "ready");

    reset(); assert(run(json()).reply.ok);
    // Scope deliberately stays the same as endpoint, network and config change.
    strcpy(credentials.host, "second.local"); assert(run(json()).reply.ok); assert(connects == 2);
    ++credentials.port; assert(run(json()).reply.ok); assert(connects == 3);
    strcpy(credentials.remote_host, "remote.example.com"); assert(run(json()).reply.ok); assert(connects == 4);
    voice_http_invalidate(); assert(live == 0); assert(run(json()).reply.ok); assert(connects == 5);
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr);
    assert(live == 0); assert(run(json()).reply.ok); assert(connects == 6);
    online = false; assert(!voice_response(rid, VoicePhase::Stored, nullptr).reply.ok); assert(live == 0);
    online = true; assert(run(json()).reply.ok); assert(connects == 7);
    VoiceScope scope{}; wrong_scope = true;
    assert(!voice_response(rid, VoicePhase::Stored, &scope).reply.ok); assert(live == 0); wrong_scope = false;

    reset();
    {
        VoiceHttpSession dirty("/v1/recordings/test/response", nullptr);
        assert(dirty.http && dirty.http->client);
        esp_http_client_set_header(dirty.http->client, "Content-Type", "audio/wav");
        esp_http_client_set_header(dirty.http->client, "X-Voice-Read-Exclude", "old");
        esp_http_client_set_header(dirty.http->client, "X-Voice-After", "claimed");
        dirty.http->keep_alive = true; dirty.accepted = true;
    }
    assert(run(json(), VoicePhase::Idle).reply.ok);
    assert(!requests.back().headers.count("Content-Type"));
    assert(!requests.back().headers.count("X-Voice-Read-Exclude"));
    assert(!requests.back().headers.count("X-Voice-After"));
    auto missing_type = json(); missing_type.headers.erase(missing_type.headers.begin());
    assert(!run(missing_type).reply.ok); assert(live == 0);
    reset(); auto invalidating = json(); invalidating.during_read = [] { voice_http_invalidate(); assert(live == 1); };
    assert(run(invalidating).reply.ok); assert(live == 0); assert(run(json()).reply.ok); assert(connects == 2);
    reset(); auto reconnecting = json(); reconnecting.during_read = [] {
        wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); assert(live == 1);
    };
    assert(run(reconnecting).reply.ok); assert(live == 0); assert(run(json()).reply.ok); assert(connects == 2);
    reset(); assert(run(json("failed")).reply.ok); assert(live == 0);
    reset(); auto overlap = json(); overlap.during_read = [] { assert(!voice_response(rid, VoicePhase::Stored, nullptr).reply.ok); };
    assert(run(overlap).reply.ok); assert(connects == 1);

    reset(); assert(run(json()).reply.ok); now += 2500000; assert(run(json()).reply.ok); assert(connects == 2);
    reset(); for (int i = 0; i < 65; ++i) assert(run(json()).reply.ok); assert(connects == 2);
    reset(); for (int i = 0; i < 32; ++i) { assert(run(json()).reply.ok); now += 1000000; } assert(connects == 2);

    reset(); auto close = json(); close.headers[1].second = "close";
    assert(run(close).reply.ok); assert(live == 0); assert(run(json()).reply.ok); assert(connects == 2);
    reset(); auto eof = json(); eof.eof = true; assert(run(eof).reply.ok); assert(live == 0);
    reset(); auto no_keepalive = json(); no_keepalive.headers.pop_back(); assert(run(no_keepalive).reply.ok); assert(live == 0);
    reset(); assert(run(json()).reply.ok); auto failed_open = json(); failed_open.open_ok = false;
    responses = {failed_open, json()};
    assert(voice_response(rid, VoicePhase::Stored, nullptr).reply.ok); assert(connects == 2);
    responses = {failed_open, failed_open};
    assert(!voice_response(rid, VoicePhase::Stored, nullptr).reply.ok); assert(responses.empty() && live == 0);
    assert(run(json()).reply.ok);
    auto interrupted = json(); interrupted.complete = false;
    responses = {interrupted, json()};
    assert(voice_response(rid, VoicePhase::Stored, nullptr).reply.ok); assert(responses.empty());
    for (bool declared : {true, false}) {
        reset(); assert(run(json()).reply.ok);
        auto oversized = json(); oversized.body.resize(1100, ' ');
        if (!declared) oversized.length = 0;
        assert(!run(oversized).reply.ok && responses.empty() && live == 0);
        assert(requests.size() == 2);
    }
    auto bad = json(); bad.complete = false; check_bad(bad);
    bad = json(); bad.length = -1; check_bad(bad);
    bad = json(); bad.status = 401; check_bad(bad);
    bad = json(); bad.body = "{}"; check_bad(bad);
    bad = json(); bad.headers.push_back({"Content-Type", "application/json"}); check_bad(bad);
    bad = page(); bad.headers.erase(bad.headers.begin() + 2); check_bad(bad);
    bad = page(); bad.headers.push_back({"X-Voice-Request-ID", rid}); check_bad(bad);
    bad = page(); bad.headers[2].second[0] = 'b'; check_bad(bad);
    bad = page(); bad.headers.pop_back(); check_bad(bad);
    bad = page(); bad.headers.push_back({"X-Voice-Read", "1"}); check_bad(bad);
    bad = page(); bad.headers[3].second = "true"; check_bad(bad);
    bad = page(); bad.body.back() ^= 1; check_bad(bad);
    bad = page(); bad.body[8] = 1; check_bad(bad);
    bad = page(); bad.body[10] = 9; check_bad(bad);
    bad = page(); bad.length = 0; check_bad(bad);
    bad = page(); bad.body.resize(12); bad.length = VOICE_PAGE_HEADER + VOICE_PAGE_BYTES; check_bad(bad);
    bad = page(); bad.body.push_back('!'); bad.length = VOICE_PAGE_HEADER + VOICE_PAGE_BYTES; check_bad(bad);
    bad = page(); bad.complete = false; check_bad(bad);
    reset(); assert(run(json()).reply.ok); bad = page(); bad.headers.pop_back();
    assert(!run(bad).reply.ok); assert(live == 0); // metadata cannot leak from a prior page

    // Legacy capability belongs to this configuration, and never retries uploads.
    for (int status : {404, 405, 501}) {
        reset(); Response unsupported; unsupported.status = status;
        responses = {unsupported, json()}; assert(voice_response(rid, VoicePhase::Stored, nullptr).reply.ok);
        responses = {json("replied", 2), page()}; result = voice_response(rid, VoicePhase::Stored, nullptr);
        assert(result.reply.ok && result.page); free(result.page);
        assert(requests.size() == 4); assert(requests[2].url.find("/response") == std::string::npos);
        voice_http_invalidate(); assert(run(json()).reply.ok); assert(requests.back().url.find("/response") != std::string::npos);
    }
    reset(); Response unsupported; unsupported.status = 404;
    bad = page(); bad.body.back() ^= 1;
    responses = {unsupported, json("replied", 2), bad}; result = voice_response(rid, VoicePhase::Stored, nullptr);
    assert(!result.reply.ok && !result.page);
    strcpy(credentials.host, "new.local"); assert(run(json()).reply.ok); assert(requests.back().url.find("/response") != std::string::npos);
    reset(); assert(run(json()).reply.ok);
    strcpy(credentials.token, "updated-token"); assert(run(json()).reply.ok);
    assert(connects == 2 && requests.back().headers.at("Authorization") == "Bearer updated-token");
    reset(); responses.push_back(json()); int16_t audio[8]{};
    assert(voice_exchange(VoiceRequest::Upload, rid, audio, 8).ok);
    assert(timings.size() == 2 && timings[0] == VoiceTimingStage::Connected && timings[1] == VoiceTimingStage::UploadConfirmed);
    assert(requests.size() == 1); assert(live == 0);
    reset(); voice_http_invalidate();
    puts("voice HTTP: reuse, invalidation, metadata, binary validation and legacy fallback passed");
}
