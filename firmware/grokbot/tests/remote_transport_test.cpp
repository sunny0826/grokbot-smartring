#include "gadget_endpoint.h"
#include "gadget_ws.h"
#include "esp_transport_ssl.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

struct Frame { int op; bool fin; std::string body; size_t offset = 0; };
static std::deque<Frame> frames;
static int current_opcode, current_length, writes, destroyed, closed, short_write = -1;
static bool current_fin, bundle, connect_fail, payload_timeout, ws_log_disabled;
static std::string connected_host, request_auth, request_headers, sent, pong;
static int connected_port, upgrade_status = 101, connect_result;
static char ssl_handle, ws_handle;
esp_transport_handle_t esp_transport_ssl_init() { return &ssl_handle; }
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t h) { assert(h == &ssl_handle); return &ws_handle; }
int esp_crt_bundle_attach(void *) { return 0; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t, esp_err_t (*)(void *)) { bundle = true; }
void esp_read_mac(uint8_t *mac, int) { memset(mac, 0xab, 6); }
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t, const esp_transport_ws_config_t *cfg)
{
    assert(std::string(cfg->ws_path) == "/v1/control");
    assert(cfg->propagate_control_frames);
    request_auth = cfg->auth; request_headers = cfg->headers;
    return 0;
}
void esp_log_level_set(const char *tag, int level)
{ assert(std::string(tag) == "transport_ws" && level == 0); ws_log_disabled = true; }
int esp_transport_connect(esp_transport_handle_t, const char *host, int port, int)
{ assert(ws_log_disabled); connected_host = host; connected_port = port; return connect_fail ? -1 : connect_result; }
int esp_transport_ws_get_upgrade_request_status(esp_transport_handle_t) { return upgrade_status; }
int esp_transport_close(esp_transport_handle_t) { ++closed; return 0; }
int esp_transport_destroy(esp_transport_handle_t) { ++destroyed; return 0; }
int esp_transport_write(esp_transport_handle_t, const char *buf, int size, int)
{
    ++writes; sent.assign(buf, size);
    memset(const_cast<char *>(buf), 0xa5, size); // IDF may leave input masked after failure
    return short_write < 0 ? size : short_write;
}
int esp_transport_poll_read(esp_transport_handle_t, int) { return !frames.empty(); }
int esp_transport_read(esp_transport_handle_t, char *buf, int capacity, int)
{
    auto &f = frames.front();
    current_length = f.body.size(); current_fin = f.fin; current_opcode = f.op;
    if (payload_timeout) return 0;
    size_t count = std::min({static_cast<size_t>(capacity), f.body.size() - f.offset, size_t(7)});
    memcpy(buf, f.body.data() + f.offset, count); f.offset += count;
    if (f.offset == f.body.size()) frames.pop_front();
    return count;
}
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t) { return current_length; }
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t)
{ return static_cast<ws_transport_opcodes_t>(current_opcode); }
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t) { return current_fin; }
int esp_transport_ws_send_raw(esp_transport_handle_t, ws_transport_opcodes_t op, const char *buf, int count, int)
{ assert(op == (10 | 128)); pong.assign(buf, count); return count; }

static GadgetWifiCred credentials()
{
    GadgetWifiCred c = {};
    strcpy(c.host, "192.168.1.9"); c.port = 8767;
    strcpy(c.remote_host, "devices.example.com"); strcpy(c.token, "abcdefghijklmnop");
    return c;
}
static void open()
{
    frames.clear(); payload_timeout = false; short_write = -1; connect_fail = false;
    upgrade_status = 101; connect_result = 0;
    auto c = credentials(); assert(gadget_ws_open(c));
}
static int receive(std::string &out, size_t capacity = 5)
{
    for (int tries = 0; tries < 1000; ++tries) {
        char buffer[4096]; int n = gadget_ws_read(buffer, capacity, 1);
        if (n < 0) return n;
        if (n > 0) { out.append(buffer, n); if (out.back() == '\n') return out.size(); }
    }
    assert(false); return 0;
}
int main()
{
    assert(gadget_remote_host_valid("")); assert(gadget_remote_host_valid("public.example"));
    for (auto h : {"a", "1.2.3.4", "-bad.example", "a-.example", "a..example", "a.example.",
                   "https://a.example", "a.example:443", "a.example/path", "a.example\r\nX:a"})
        assert(!gadget_remote_host_valid(h));
    assert(!gadget_header_value_valid("abc\r\nHeader: yes"));
    auto c = credentials(); auto remote = gadget_endpoint(c);
    assert(remote.remote && remote.voice_port == 443 && remote.control_port == 443);
    c.remote_host[0] = 0; auto lan = gadget_endpoint(c);
    assert(!lan.remote && lan.voice_port == 8768 && lan.control_port == 8767);

    open(); assert(bundle && connected_host == "devices.example.com" && connected_port == 443);
    assert(request_auth == "Bearer abcdefghijklmnop");
    assert(request_headers == "X-Device-ID: smartring-abababababab\r\n");
    std::string original = "{\"type\":\"poll\"}\n";
    assert(gadget_ws_write(original.data(), original.size())); assert(original == sent);
    int before = writes, freed = destroyed;
    short_write = 3; assert(!gadget_ws_write(original.data(), original.size()));
    assert(writes == before + 1 && destroyed == freed + 2 && original == sent);
    assert(!gadget_ws_write(original.data(), original.size())); assert(writes == before + 1);

    open(); frames.push_back({2, false, "{\"ok\":"});
    frames.push_back({9, true, "keepalive"}); frames.push_back({10, true, "pong"});
    frames.push_back({0, true, "true}\n"}); std::string output;
    assert(receive(output) > 0 && output == "{\"ok\":true}\n" && pong == "keepalive");
    char buffer[8]; assert(gadget_ws_read(buffer, sizeof(buffer), 1) == 0);

    open(); frames.push_back({2, false, "{}\n"}); frames.push_back({9, true, ""});
    frames.push_back({0, true, ""}); output.clear(); assert(receive(output) == 3);
    for (auto f : std::vector<Frame>{{1,true,"{}\n"}, {0,true,"{}\n"}, {2,true,"{}\n{}\n"},
            {2,true,"{}"}, {2,true,std::string(2049,'x')}, {8,true,""}, {9,false,"x"},
            {9,true,std::string(126,'x')}}) {
        open(); frames.push_back(f); output.clear(); assert(receive(output) < 0);
    }
    open(); frames.push_back({2, false, std::string(2048, 'x')});
    frames.push_back({0, true, "\n"}); output.clear(); assert(receive(output) < 0);
    open(); frames.push_back({2, true, "{}\n"}); payload_timeout = true;
    output.clear(); assert(receive(output) < 0);
    open(); gadget_ws_close(); freed = destroyed; gadget_ws_close(); assert(destroyed == freed);
    connect_fail = true; c = credentials(); assert(!gadget_ws_open(c)); assert(destroyed == freed + 2);
    connect_fail = false;
    for (int status : {301, 302, 303, 307, 308}) {
        upgrade_status = status; connect_result = status;
        before = writes; assert(!gadget_ws_open(c)); assert(writes == before);
    }
    connect_result = 0; upgrade_status = 200; assert(!gadget_ws_open(c));
    puts("Remote endpoint, WS binary boundaries, partial payloads/writes, fragmentation, ping/pong, bounds and cleanup PASS");
}
