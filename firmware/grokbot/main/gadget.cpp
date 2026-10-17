// Grok Gadgets protocol 0.1.0 client — see protocol/0.1.0/README.md in the SDK
// (reference/grok-gadgets-esp32-sdk). LF-delimited JSON over one of two
// transports:
//   USB  — USB Serial/JTAG; the Mac-side `usb-bridge` relays frames to the
//          gateway and supplies the token (default, unprovisioned).
//   Wi-Fi — LAN TLS to the gateway's --lan-listen port with a pinned CA;
//          credentials live in NVS, written by `wifi-provision` over the USB
//          console; the hello then carries the token itself.
//
// Session loop: hello -> poll every 200 ms (15 s idle closes the session).
// A poll response may carry commands; each command is validated here, handed to
// the LVGL task over a queue, and acked with the post-execution state snapshot.
// Reconnect with capped backoff; never resend a physical action under a new ID.
//
// The USB port doubles as the provisioning console: JSON lines with
// type status/provision/wifi_ssid/factory_reset are answered here and never reach the
// session parser (gateway responses have no "type" field, so the two never
// collide).
#include "gadget.h"
#include "gadget_wifi.h"
#include "gadget_endpoint.h"
#include "gadget_clock.h"
#include "voice.h"
#include "wifi_setup.h"
#include "wifi_setup_owner.h"

#include <cstring>
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define FIRMWARE_VERSION "0.9.1-tap-controls"

static constexpr int RX_BUF = 4608;   // console lines may reach 4096; gateway frames 2048
static constexpr int TX_BUF = 2208;
static constexpr uint32_t POLL_MS = 200;
static constexpr uint32_t STATE_MS = 5000;    // state goes stale at 10 s on the gateway
static constexpr size_t DEDUP_LEN = 16;       // bounded per-boot command dedup

static QueueHandle_t s_cmd_q;    // GadgetCommand, gadget -> LVGL
static QueueHandle_t s_ack_q;    // struct Ack, LVGL -> gadget
static QueueHandle_t s_evt_q;    // GadgetEvent, LVGL -> gadget

struct Ack {
    char cmd_id[68];
    bool ok;
    char error[48];
};

static GadgetState s_state;
static SemaphoreHandle_t s_state_mu;
static char s_device_id[24];
static char s_boot_id[24];
static uint32_t s_evt_seq = 0;
static char s_dedup[DEDUP_LEN][68];
static size_t s_dedup_n = 0;

/* ------------------------------------------------ transport ----
 * The session loop below is transport-blind; everything link-specific sits
 * behind these two ops. read() returns bytes, 0 on timeout, <0 when the link
 * is dead and the transport must be reopened (Wi-Fi only; USB never dies). */

struct TransportOps {
    bool (*write)(const char *buf, size_t len);
    int (*read)(char *buf, size_t cap, uint32_t timeout_ms);
};

static TransportOps s_ops;
static bool s_wifi_mode;
static GadgetWifiCred s_cred;
static bool s_tp_dead;   // set by a fatal read; session loop bails out

static bool usb_write(const char *buf, size_t len)
{
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(200)) == (int)len;
}

static int usb_read(char *buf, size_t cap, uint32_t timeout_ms)
{
    return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(timeout_ms < 100 ? timeout_ms : 100));
}

static int tls_read(char *buf, size_t cap, uint32_t timeout_ms)
{
    int n = gadget_wifi_transport_read(buf, cap, timeout_ms);
    if (n < 0) s_tp_dead = true;
    return n < 0 ? 0 : n;
}

/* ------------------------------------------------ public LVGL-side API ---- */

bool gadget_command_take(GadgetCommand &out)
{
    return s_cmd_q && xQueueReceive(s_cmd_q, &out, 0) == pdTRUE;
}

void gadget_command_done(const GadgetCommand &cmd, bool ok, const char *error)
{
    if (!s_ack_q) return;
    Ack a = {};
    strlcpy(a.cmd_id, cmd.cmd_id, sizeof(a.cmd_id));
    a.ok = ok;
    if (error) strlcpy(a.error, error, sizeof(a.error));
    xQueueSend(s_ack_q, &a, 0);  // depth 4; a full queue drops the ack, the
}                                // command then times out honestly at the gateway

static void emit(const GadgetEvent &e)
{
    if (s_evt_q) xQueueSend(s_evt_q, &e, 0);
}

void gadget_emit_poke(bool remote)
{
    GadgetEvent e = {GadgetEventType::Poke, 0.f, {}, remote};
    emit(e);
}

void gadget_emit_shake(float jerk)
{
    GadgetEvent e = {GadgetEventType::Shake, jerk, {}, false};
    emit(e);
}

void gadget_emit_switched(const char *character)
{
    GadgetEvent e = {GadgetEventType::Switched, 0.f, {}, false};
    strlcpy(e.name, character, sizeof(e.name));
    emit(e);
}

void gadget_publish_state(const GadgetState &s)
{
    if (!s_state_mu) return;
    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    s_state = s;
    xSemaphoreGive(s_state_mu);
}

/* ------------------------------------------------ protocol codec ---- */

static void state_json(cJSON *root)
{
    xSemaphoreTake(s_state_mu, portMAX_DELAY);
    GadgetState s = s_state;
    xSemaphoreGive(s_state_mu);

    cJSON *st = cJSON_AddObjectToObject(root, "state");
    cJSON_AddStringToObject(st, "character", s.character);
    cJSON_AddStringToObject(st, "anim", s.anim);
    cJSON *tilt = cJSON_AddObjectToObject(st, "tilt");
    cJSON_AddNumberToObject(tilt, "gx", s.gx);
    cJSON_AddNumberToObject(tilt, "gy", s.gy);
    cJSON_AddNumberToObject(st, "backlight", s.backlight);
    cJSON *bat = cJSON_AddObjectToObject(st, "battery");
    cJSON_AddNumberToObject(bat, "percent", s.battery_pct);
    cJSON_AddBoolToObject(bat, "charging", s.battery_charging);
    cJSON_AddNumberToObject(st, "uptime_s", s.uptime_s);
    // state allows extra fields (additionalProperties: true in the schema)
    cJSON_AddStringToObject(st, "link", s_wifi_mode
        ? (gadget_wifi_link_up() ? "wifi" : "wifi-down") : "usb");
}

// Capability list and schemas, kept terse: the whole hello frame must fit the
// 2048-byte USB limit (TCP would allow 16384, but the frames are shared).
static void add_capabilities(cJSON *dev)
{
    cJSON *caps = cJSON_AddArrayToObject(dev, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("character.set"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("backlight.set"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("animation.trigger"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("poke"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("state"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("poke_event"));  // event name
    cJSON_AddItemToArray(caps, cJSON_CreateString("shake"));       // event name
    cJSON_AddItemToArray(caps, cJSON_CreateString("switched"));    // event name

    cJSON *schemas = cJSON_AddObjectToObject(dev, "capability_schemas");
    auto ev = [](const char *desc) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "object");
        cJSON_AddStringToObject(s, "x-grok-gadgets-kind", "event");
        cJSON_AddStringToObject(s, "description", desc);
        return s;
    };
    cJSON_AddItemToObject(schemas, "character.set", cJSON_Parse(
        R"({"type":"object","description":"Switch the character skin","properties":{"name":{"type":"string","enum":["grokbot","pika","rowlet"]}},"required":["name"],"additionalProperties":false})"));
    cJSON_AddItemToObject(schemas, "backlight.set", cJSON_Parse(
        R"({"type":"object","description":"Set display backlight 0-100","properties":{"percent":{"type":"integer","minimum":0,"maximum":100}},"required":["percent"],"additionalProperties":false})"));
    cJSON_AddItemToObject(schemas, "animation.trigger", cJSON_Parse(
        R"({"type":"object","description":"Play an idle animation now","properties":{"name":{"type":"string","enum":["thinking","happy","notify","orbit","sleepy","dizzy"]}},"required":["name"],"additionalProperties":false})"));
    cJSON_AddItemToObject(schemas, "poke", cJSON_Parse(
        R"({"type":"object","description":"Poke the bot: it jumps against gravity and blinks","properties":{},"additionalProperties":false})"));
    // Touch pokes are reported under the separate event name "poke_event":
    // "poke" alone is the command, and a name is an event only when annotated.
    cJSON_AddItemToObject(schemas, "poke_event", ev("The screen was tapped (or a remote poke landed)"));
    cJSON_AddItemToObject(schemas, "shake", ev("The device was shaken (dizzy spell)"));
    cJSON_AddItemToObject(schemas, "switched", ev("The character was switched by swipe"));
}

/* Send one frame. Returns false when serialization or the transport write fails. */
static bool send_frame(cJSON *root, char *buf, size_t cap)
{
    // cJSON_PrintPreallocated returns a BOOL, not the length.
    bool printed = cJSON_PrintPreallocated(root, buf, (int)cap - 2, false);
    cJSON_Delete(root);
    if (!printed) { s_tp_dead = true; return false; }
    size_t len = strlen(buf);
    buf[len] = '\n';
    bool written = s_ops.write(buf, len + 1);
    if (!written) s_tp_dead = true;
    return written;
}

static bool send_hello(char *buf, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "protocol_version", "0.1.0");
    if (s_wifi_mode)
        cJSON_AddStringToObject(root, "token", s_cred.token);  // over USB the
        // bridge supplies it; the LAN listener authenticates the device itself
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "device_id", s_device_id);
    cJSON_AddStringToObject(dev, "model", "SmartRing-Plus grokbot");
    cJSON_AddStringToObject(dev, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(dev, "boot_id", s_boot_id);
    cJSON_AddBoolToObject(dev, "simulated", false);
    add_capabilities(dev);
    state_json(dev);   // schema: state lives INSIDE the device object
    return send_frame(root, buf, cap);
}

static bool send_simple(const char *type, char *buf, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    return send_frame(root, buf, cap);
}

static bool send_ack(const Ack &a, char *buf, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddStringToObject(root, "command_id", a.cmd_id);
    cJSON_AddStringToObject(root, "status", a.ok ? "executed" : "failed");
    if (!a.ok) {
        cJSON *err = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(err, "code", "invalid_arguments");
        cJSON_AddStringToObject(err, "message", a.error);
    }
    state_json(root);
    return send_frame(root, buf, cap);
}

static bool send_event(const GadgetEvent &e, char *buf, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    char id[32];
    snprintf(id, sizeof(id), "%s-%lu", s_boot_id, (unsigned long)++s_evt_seq);
    cJSON_AddStringToObject(root, "event_id", id);
    cJSON *data = cJSON_CreateObject();
    switch (e.type) {
    case GadgetEventType::Poke:
        // The event uses the dedicated "poke_event" name: "poke" alone is the command.
        cJSON_AddStringToObject(root, "name", "poke_event");
        cJSON_AddBoolToObject(data, "remote", e.remote);
        break;
    case GadgetEventType::Shake:
        cJSON_AddStringToObject(root, "name", "shake");
        cJSON_AddNumberToObject(data, "jerk", e.jerk);
        break;
    case GadgetEventType::Switched:
        cJSON_AddStringToObject(root, "name", "switched");
        cJSON_AddStringToObject(data, "character", e.name);
        break;
    }
    cJSON_AddItemToObject(root, "data", data);
    return send_frame(root, buf, cap);
}

/* ------------------------------------------------ USB console ----
 * `grok-gadgets-gateway wifi-provision` speaks one JSON line per command and
 * expects one JSON line with an `ok` field back. Other lines are status text.
 * The console never prints a stored secret. Replies always go to the USB port,
 * in both modes. */

static void console_reply(bool ok, const char *code, const char *message, bool include_wifi = false)
{
    char buf[1024];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    if (include_wifi) {
        cJSON_AddStringToObject(root, "firmware", FIRMWARE_VERSION);
        // NVS loader serializes with the owner commit; never read mutable s_cred here.
        auto *saved = static_cast<GadgetWifiCred *>(heap_caps_calloc(1, sizeof(GadgetWifiCred), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (saved && gadget_wifi_load(*saved)) {
            cJSON_AddStringToObject(root, "transport", saved->remote_host[0] ? "wss" : "lan");
            cJSON_AddStringToObject(root, "remote_host", saved->remote_host);
            cJSON_AddStringToObject(root, "ssid", saved->ssid);
        }
        heap_caps_free(saved);
        auto setup = wifi_setup_snapshot();
        auto *wifi = cJSON_AddObjectToObject(root, "wifi_setup");
        cJSON_AddNumberToObject(wifi, "phase", static_cast<int>(setup.phase));
        cJSON_AddNumberToObject(wifi, "count", setup.count);
        cJSON_AddNumberToObject(wifi, "error", static_cast<int>(setup.error));
        cJSON_AddNumberToObject(wifi, "method", static_cast<int>(setup.method));
        cJSON_AddNumberToObject(wifi, "phone_phase", static_cast<int>(setup.phone_phase));
        cJSON_AddNumberToObject(wifi, "remaining_seconds", setup.remaining_seconds);
        cJSON_AddBoolToObject(wifi, "command_pending", setup.command_pending);
        cJSON_AddBoolToObject(root, "clock_ready", gadget_clock_ready());
        cJSON_AddStringToObject(root, "transport_error", gadget_wifi_transport_error());
        cJSON_AddStringToObject(root, "link", s_wifi_mode
                               ? (gadget_wifi_link_up() ? "wifi" : "wifi-down") : "usb");
        cJSON *memory = cJSON_AddObjectToObject(root, "memory");
        cJSON_AddNumberToObject(memory, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        cJSON_AddNumberToObject(memory, "min_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        cJSON_AddNumberToObject(memory, "largest_internal", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        auto voice = voice_snapshot();
        cJSON *v = cJSON_AddObjectToObject(root, "voice");
        cJSON_AddNumberToObject(v, "phase", static_cast<int>(voice.phase));
        cJSON_AddNumberToObject(v, "duration_ms", voice.duration_ms);
        cJSON_AddStringToObject(v, "error", voice.error);
        cJSON_AddBoolToObject(v, "unread", voice.unread);
        cJSON_AddNumberToObject(v, "page_index", voice.page_index);
        cJSON_AddNumberToObject(v, "page_count", voice.page_count);
        auto diagnostic = voice_diagnostics();
        cJSON_AddBoolToObject(v, "boot_pressed", diagnostic.boot_pressed);
        cJSON_AddNumberToObject(v, "boot_presses", diagnostic.presses);
        cJSON_AddNumberToObject(v, "boot_queued", diagnostic.queued);
        cJSON_AddNumberToObject(v, "boot_handled", diagnostic.handled);
        cJSON_AddNumberToObject(v, "start_stage", diagnostic.start_stage);
        cJSON_AddNumberToObject(v, "hold_id", voice.hold_id);
    }
    if (!ok) {
        cJSON *err = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(err, "code", code);
        cJSON_AddStringToObject(err, "message", message);
    }
    if (cJSON_PrintPreallocated(root, buf, (int)sizeof(buf) - 2, false)) {
        size_t len = strlen(buf);
        buf[len] = '\n';
        usb_serial_jtag_write_bytes(buf, len + 1, pdMS_TO_TICKS(200));
    }
    cJSON_Delete(root);
}

// One shared reply for "accepted, rebooting into the new mode".
static void console_reply_restart()
{
    console_reply(true, nullptr, nullptr);
    vTaskDelay(pdMS_TO_TICKS(150));   // let the USB FIFO drain
    esp_restart();
}

class ConsoleMutation {
public:
    bool acquired = false;
    ConsoleMutation()
    {
        if (!wifi_setup_usb_acquire()) return;
        if (!voice_settings_pause(operation)) { wifi_setup_usb_release(); return; }
        auto state = voice_snapshot();
        if (state.phase != VoicePhase::Idle && state.phase != VoicePhase::Unavailable) {
            voice_settings_resume(operation); wifi_setup_usb_release(); return;
        }
        int64_t deadline = esp_timer_get_time() / 1000 + 25000;
        while (voice_settings_status(operation) == VoiceSettingsStatus::Pending && esp_timer_get_time() / 1000 < deadline)
            vTaskDelay(pdMS_TO_TICKS(20));
        acquired = voice_settings_status(operation) == VoiceSettingsStatus::Paused;
        if (!acquired) { voice_settings_resume(operation); wifi_setup_usb_release(); }
    }
    ~ConsoleMutation() { if (acquired) { voice_settings_resume(operation); wifi_setup_usb_release(); } }
private:
    static constexpr uint32_t operation = UINT32_MAX;
};

// Returns true when the line was a console command (consumed).
static bool console_handle(cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) return false;
    const char *t = type->valuestring;

    if (!strcmp(t, "status")) {
        console_reply(true, nullptr, nullptr, true);
        return true;
    }
    bool mutation = !strcmp(t, "remote") || !strcmp(t, "wifi_network") || !strcmp(t, "wifi_ssid") ||
                    !strcmp(t, "factory_reset") || !strcmp(t, "provision");
    if (!mutation) return false;
    ConsoleMutation guard;
    if (!guard.acquired) {
        console_reply(false, "busy", "network settings or voice activity in progress");
        return true;
    }
    if (!strcmp(t, "remote")) {
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
        if (!s_wifi_mode || !cJSON_IsString(host) || !gadget_wifi_set_remote(host->valuestring))
            console_reply(false, "invalid_arguments", "requires saved settings and a DNS hostname, or empty host for LAN");
        else console_reply_restart();
        return true;
    }
    if (!strcmp(t, "wifi_network")) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (!s_wifi_mode || !cJSON_IsString(ssid) || !cJSON_IsString(password) ||
            !gadget_wifi_set_network(ssid->valuestring, password->valuestring))
            console_reply(false, "invalid_arguments", "requires saved settings, 1-32 byte SSID and empty or 8-64 byte password");
        else console_reply_restart();
        return true;
    }
    if (!strcmp(t, "wifi_ssid")) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        if (!s_wifi_mode || !cJSON_IsString(ssid) || !ssid->valuestring ||
            !gadget_wifi_set_ssid(ssid->valuestring)) {
            console_reply(false, "invalid_arguments", "requires saved Wi-Fi settings and a 1-32 byte SSID");
        } else {
            console_reply_restart();
        }
        return true;
    }
    if (!strcmp(t, "factory_reset")) {
        gadget_wifi_erase();
        console_reply_restart();   // reboots unprovisioned -> USB mode
        return true;
    }
    if (!strcmp(t, "provision")) {
        auto *c = static_cast<GadgetWifiCred *>(heap_caps_calloc(
            1, sizeof(GadgetWifiCred), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!c) {
            console_reply(false, "no_memory", "cannot allocate provisioning buffer");
            return true;
        }
        auto str = [&](const char *key, char *dst, size_t cap) -> bool {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
            if (!cJSON_IsString(v) || !v->valuestring) return false;
            size_t n = strlen(v->valuestring);
            if (n >= cap) return false;
            memcpy(dst, v->valuestring, n + 1);
            return true;
        };
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
        bool ok = str("ssid", c->ssid, sizeof(c->ssid)) && c->ssid[0] &&
                  str("password", c->pass, sizeof(c->pass)) &&
                  (c->pass[0] == 0 || strlen(c->pass) >= 8) &&
                  str("token", c->token, sizeof(c->token)) && strlen(c->token) >= 16 &&
                  gadget_header_value_valid(c->token) &&
                  str("host", c->host, sizeof(c->host)) && c->host[0] &&
                  cJSON_IsNumber(port) && port->valuedouble >= 1 && port->valuedouble <= 65535 &&
                  str("ca", c->ca, sizeof(c->ca)) && strstr(c->ca, "BEGIN CERTIFICATE");
        if (ok) {
            c->port = (uint16_t)port->valuedouble;
            ok = gadget_wifi_save(*c);
        }
        heap_caps_free(c);
        if (!ok) {
            console_reply(false, "invalid_arguments", "provision fields missing or out of range");
            return true;
        }
        console_reply_restart();   // reboots provisioned -> Wi-Fi mode
        return true;
    }
    return false;
}

/* ------------------------------------------------ command handling ---- */

static bool dedup_seen(const char *cmd_id)
{
    for (size_t i = 0; i < s_dedup_n; ++i)
        if (!strcmp(s_dedup[i], cmd_id)) return true;
    return false;
}

static void dedup_remember(const char *cmd_id)
{
    if (s_dedup_n < DEDUP_LEN) {
        strlcpy(s_dedup[s_dedup_n++], cmd_id, sizeof(s_dedup[0]));
    } else {
        memmove(s_dedup, s_dedup + 1, sizeof(s_dedup) - sizeof(s_dedup[0]));
        strlcpy(s_dedup[DEDUP_LEN - 1], cmd_id, sizeof(s_dedup[0]));
    }
}

/* Validate a delivered command at the protocol boundary; only typed structs
 * cross into the LVGL task. */
static void handle_command(const char *cmd_id, const char *capability, const cJSON *args)
{
    GadgetCommand c = {};
    strlcpy(c.cmd_id, cmd_id, sizeof(c.cmd_id));
    bool valid = true;

    if (!strcmp(capability, "character.set")) {
        c.type = GadgetCmdType::SetCharacter;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
        valid = cJSON_IsString(name) && name->valuestring &&
                strlen(name->valuestring) < sizeof(c.name);
        if (valid) strlcpy(c.name, name->valuestring, sizeof(c.name));
    } else if (!strcmp(capability, "backlight.set")) {
        c.type = GadgetCmdType::SetBacklight;
        const cJSON *pct = cJSON_GetObjectItemCaseSensitive(args, "percent");
        valid = cJSON_IsNumber(pct) && pct->valuedouble == (double)(int)pct->valuedouble &&
                pct->valuedouble >= 0 && pct->valuedouble <= 100;
        if (valid) c.percent = (int)pct->valuedouble;
    } else if (!strcmp(capability, "animation.trigger")) {
        c.type = GadgetCmdType::TriggerAnimation;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
        valid = cJSON_IsString(name) && name->valuestring &&
                strlen(name->valuestring) < sizeof(c.name);
        if (valid) strlcpy(c.name, name->valuestring, sizeof(c.name));
    } else if (!strcmp(capability, "poke")) {
        c.type = GadgetCmdType::Poke;
        valid = true;
    } else {
        valid = false;
    }

    if (valid && !dedup_seen(cmd_id) && xQueueSend(s_cmd_q, &c, 0) == pdTRUE) {
        dedup_remember(cmd_id);
    }
    // Invalid or duplicate: the command stays un-acked and closes at the gateway
    // as timed_out; duplicates already returned their original result there.
}

/* ------------------------------------------------ session loop ----
 * Strictly synchronous over either transport: every request waits for its own
 * response line before anything else is sent (the USB bridge enforces the same
 * shape, and the LAN listener answers one frame per request too). */

// Read lines until one parses as protocol JSON; returns it, or nullptr on
// timeout / dead transport. Console lines are answered and consumed here;
// noise lines (ROM output) are skipped inside the window.
static cJSON *read_response(char *rx, size_t &rx_len, int64_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    for (;;) {
        int64_t left = deadline - esp_timer_get_time() / 1000;
        if (left <= 0 || s_tp_dead || wifi_setup_requested()) return nullptr;
        char *lf = rx_len ? (char *)memchr(rx, '\n', rx_len) : nullptr;
        if (lf) {
            *lf = 0;
            cJSON *resp = cJSON_Parse(rx);
            size_t used = (size_t)(lf - rx) + 1;
            memmove(rx, rx + used, rx_len - used);
            rx_len -= used;
            if (resp) {
                if (!s_wifi_mode && console_handle(resp)) {   // console line: not a session frame
                    cJSON_Delete(resp);
                    continue;
                }
                return resp;
            }
            continue;  // noise line: keep waiting within the same window
        }
        if (rx_len >= RX_BUF - 1) rx_len = 0;  // overlong garbage line: reset
        int n = s_ops.read(rx + rx_len, RX_BUF - 1 - rx_len, (uint32_t)(left < 100 ? left : 100));
        if (n > 0) rx_len += n;
    }
}

// Handle one gateway response. Returns false when the session must be rebuilt.
static bool handle_response(cJSON *resp, bool &session, int64_t &last_response, int64_t now)
{
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(resp, "ok");
    if (cJSON_IsBool(ok) && cJSON_IsTrue(ok)) {
        session = true;
        last_response = now;
        const cJSON *cmds = cJSON_GetObjectItemCaseSensitive(resp, "commands");
        const cJSON *cmd = nullptr;
        cJSON_ArrayForEach(cmd, cmds) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(cmd, "command_id");
            const cJSON *cap = cJSON_GetObjectItemCaseSensitive(cmd, "capability");
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(cmd, "arguments");
            if (cJSON_IsString(id) && cJSON_IsString(cap) && cJSON_IsObject(args))
                handle_command(id->valuestring, cap->valuestring, args);
        }
        return true;
    }
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    const cJSON *code = err ? cJSON_GetObjectItemCaseSensitive(err, "code") : nullptr;
    const char *c = cJSON_IsString(code) ? code->valuestring : "";
    if (!strcmp(c, "unauthorized") || !strcmp(c, "revoked") || !strcmp(c, "stale_session")) {
        if (s_wifi_mode) gadget_wifi_set_transport_error(!strcmp(c, "stale_session") ? "stale_session" : "device_unauthorized");
        return false;
    }
    last_response = now;         // late_ack / unknown_command / busy: session is alive
    return true;
}

// Run hello/poll/ack/event rounds until the session or the transport dies.
static bool session_loop(char *tx, char *rx)
{
    bool answered = false;
    const int response_ms = s_wifi_mode && s_cred.remote_host[0] ? 4000 : 2000;
    size_t rx_len = 0;
    bool session = false;
    int64_t last_poll = 0, last_state = 0;
    int64_t last_response = 0;   // watchdog: a silent gateway means a dead session

    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (s_tp_dead || wifi_setup_requested()) return answered;

        if (!session) {
            // The hello may go nowhere when no host is attached; only an ok
            // response opens the session. Wait synchronously so the response
            // is consumed before any retry — a second hello would replace the
            // session the first one just opened.
            if (!send_hello(tx, TX_BUF)) return answered;
            cJSON *resp = read_response(rx, rx_len, response_ms);
            if (resp) {
                bool ok = handle_response(resp, session, last_response, now);
                cJSON_Delete(resp);
                if (ok && session) {
                    answered = true;
                    last_poll = last_state = now;
                    continue;
                }
            }
            if (s_wifi_mode && !s_tp_dead) gadget_wifi_set_transport_error(resp ? "hello_rejected" : "response_timeout");
            return answered;
        }

        if (now - last_response > 12000) {
            return answered;   // host detached, bridge died or TLS went silent: reopen
        }

        /* TX one frame per round-trip: acks and events first, then the state
         * heartbeat, then the poll that also serves as the idle read. */
        bool sent = false;
        Ack a;
        GadgetEvent e;
        if (xQueueReceive(s_ack_q, &a, 0) == pdTRUE) {
            sent = send_ack(a, tx, TX_BUF);
        } else if (xQueueReceive(s_evt_q, &e, 0) == pdTRUE) {
            sent = send_event(e, tx, TX_BUF);
        } else if (now - last_state >= STATE_MS) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "state");
            state_json(root);
            sent = send_frame(root, tx, TX_BUF);
            last_state = now;
        } else if (now - last_poll >= POLL_MS) {
            sent = send_simple("poll", tx, TX_BUF);
            last_poll = now;
        }

        if (sent) {
            cJSON *resp = read_response(rx, rx_len, response_ms);
            if (resp) {
                if (!handle_response(resp, session, last_response, esp_timer_get_time() / 1000))
                    session = false;
                cJSON_Delete(resp);
                if (!session) return answered;
            } else {
                if (s_wifi_mode && !s_tp_dead) gadget_wifi_set_transport_error("response_timeout");
                return answered;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));   // nothing due yet
        }
    }
}

// In Wi-Fi mode the USB port is console-only; this task answers provisioning
// lines there while the session runs on TLS.
static void console_task(void *)
{
    char *rx = (char *)heap_caps_malloc(RX_BUF, MALLOC_CAP_8BIT);
    configASSERT(rx);
    size_t rx_len = 0;
    for (;;) {
        char *lf = rx_len ? (char *)memchr(rx, '\n', rx_len) : nullptr;
        if (lf) {
            *lf = 0;
            cJSON *line = cJSON_Parse(rx);
            size_t used = (size_t)(lf - rx) + 1;
            memmove(rx, rx + used, rx_len - used);
            rx_len -= used;
            if (line) {
                console_handle(line);
                cJSON_Delete(line);
            }
            continue;
        }
        if (rx_len >= RX_BUF - 1) rx_len = 0;
        int n = usb_serial_jtag_read_bytes(rx + rx_len, RX_BUF - 1 - rx_len, pdMS_TO_TICKS(200));
        if (n > 0) rx_len += n;
    }
}

static void reconnect_wait(uint32_t duration)
{
    int64_t until = esp_timer_get_time() / 1000 + duration;
    while (!wifi_setup_requested() && esp_timer_get_time() / 1000 < until)
        vTaskDelay(pdMS_TO_TICKS(20));
}

static void gadget_task(void *)
{
    char *tx = (char *)heap_caps_malloc(TX_BUF, MALLOC_CAP_8BIT);
    char *rx = (char *)heap_caps_malloc(RX_BUF, MALLOC_CAP_8BIT);
    configASSERT(tx && rx);

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_BUF,
        .rx_buffer_size = RX_BUF,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    s_wifi_mode = gadget_wifi_load(s_cred);
    if (s_wifi_mode)
        xTaskCreate(console_task, "gadget_console", 4096, nullptr, 4, nullptr);

    uint32_t backoff_ms = 1000;
    for (;;) {
        if (wifi_setup_requested()) {
            gadget_wifi_transport_close();
            if (wifi_setup_owner_step(s_cred)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        }
        s_tp_dead = false;
        if (s_wifi_mode) {
            if (!gadget_wifi_transport_open(s_cred)) {
                reconnect_wait(backoff_ms + esp_random() % 251);
                backoff_ms = backoff_ms < 8000 ? backoff_ms * 2 : 15000;
                continue;
            }
            s_ops = {gadget_wifi_transport_write, tls_read};
        } else {
            s_ops = {usb_write, usb_read};
        }
        if (session_loop(tx, rx)) backoff_ms = 1000;
        if (s_wifi_mode) gadget_wifi_transport_close();
        reconnect_wait(backoff_ms + esp_random() % 251);
        backoff_ms = backoff_ms < 8000 ? backoff_ms * 2 : 15000;   // brief pause before re-hello/reopen
    }
}

void gadget_start()
{
    s_cmd_q = xQueueCreate(4, sizeof(GadgetCommand));
    s_ack_q = xQueueCreate(4, sizeof(Ack));
    s_evt_q = xQueueCreate(8, sizeof(GadgetEvent));
    s_state_mu = xSemaphoreCreateMutex();
    configASSERT(s_cmd_q && s_ack_q && s_evt_q && s_state_mu);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);  // stable per chip; matches enrollment
    snprintf(s_device_id, sizeof(s_device_id), "smartring-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_boot_id, sizeof(s_boot_id), "boot-%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());

    xTaskCreate(gadget_task, "gadget", 8192, nullptr, 5, nullptr);
}
