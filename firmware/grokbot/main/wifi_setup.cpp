#include "wifi_setup.h"
#include "wifi_setup_owner.h"
#include "wifi_setup_driver.h"
#include "wifi_setup_http.h"
#include "voice.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
std::mutex mutex;
std::atomic<unsigned> lease{0}; // 0 free, 1 settings, 2 USB mutation/reboot
WifiSetupSnapshot state;
uint32_t sequence;
WifiSetupNetwork original, candidate;
bool cancel_requested, close_requested, scan_requested, connect_requested;
bool touched, voice_held, original_saved;
int64_t deadline, phone_deadline, success_deadline;
WifiPhoneDisplay phone_display;
bool http_running, phone_booting, cleanup_started, saved_network;
bool phone_active() { return state.method == WifiSetupMethod::Phone && state.phone_phase != WifiPhonePhase::Off; }
void erase(void *memory, size_t size)
{
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(memory);
    while (size--) *bytes++ = 0;
}
uint32_t remaining()
{
    auto left = phone_deadline - wifi_setup_now_ms();
    return phone_active() && left > 0 ? static_cast<uint32_t>((left + 999) / 1000) : 0;
}
void close_phone(WifiSetupError error = WifiSetupError::None)
{
    if (error != WifiSetupError::None) state.error = error;
    if (state.phone_phase == WifiPhonePhase::Closing) return;
    state.phone_phase = WifiPhonePhase::Closing; ++state.revision;
    close_requested = true;
    cancel_requested = scan_requested = connect_requested = false;
}


void phase(WifiSetupPhase next, WifiSetupError error = WifiSetupError::None)
{
    state.phase = next; state.error = error; ++state.revision;
}
void forget_password() { erase(candidate.password, sizeof(candidate.password)); }
void release_voice()
{
    if (voice_held) voice_settings_resume(state.operation);
    voice_held = false;
}
void finish()
{
    release_voice(); forget_password(); erase(&original, sizeof(original));
    erase(&phone_display, sizeof(phone_display));
    state.phone_phase = WifiPhonePhase::Off;
    cancel_requested = close_requested = scan_requested = connect_requested = false;
    touched = original_saved = false;
    bool failed = state.method == WifiSetupMethod::Phone && state.error != WifiSetupError::None && state.error != WifiSetupError::Cancelled;
    phase(saved_network ? WifiSetupPhase::Connected : (failed ? WifiSetupPhase::Failed : WifiSetupPhase::Closed),
          failed && !saved_network ? state.error : WifiSetupError::None);
    lease.store(0);
}
void expire_preparing()
{
    if (state.phase == WifiSetupPhase::Preparing && wifi_setup_now_ms() >= deadline) {
        if (!phone_active()) release_voice();
        scan_requested = connect_requested = false; forget_password();
        phase(WifiSetupPhase::Failed, WifiSetupError::VoiceBusy);
    }
}
bool valid_operation(uint32_t id) { return id && state.operation == id && state.phase != WifiSetupPhase::Closed; }
bool editable() { return state.phase == WifiSetupPhase::Choosing || state.phase == WifiSetupPhase::Failed; }
bool password_valid(WifiSetupSecurity security, const char *password)
{
    if (!password) return false;
    size_t n = strnlen(password, 65);
    if (security == WifiSetupSecurity::Open) return n == 0;
    if (security != WifiSetupSecurity::Personal || n < 8 || n > 64) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = password[i];
        if (c < 32 || c > 126) return false;
        if (n == 64 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
void prepare()
{
    deadline = wifi_setup_now_ms() + 25000;
    voice_held = voice_settings_pause(state.operation);
    phase(voice_held ? WifiSetupPhase::Preparing : WifiSetupPhase::Failed,
          voice_held ? WifiSetupError::None : WifiSetupError::VoiceBusy);
    if (!voice_held) scan_requested = connect_requested = false;
}
void restore(WifiSetupError error)
{
    cancel_requested = scan_requested = connect_requested = false;
    forget_password(); wifi_driver_clear_scan();
    phase(WifiSetupPhase::Restoring, error);
    state.original_connected = false;
    deadline = wifi_setup_now_ms() + 15000;
    if (!wifi_driver_join(original)) deadline = wifi_setup_now_ms();
    if (state.phone_phase == WifiPhonePhase::Starting) close_phone(error);
}
WifiSetupError driver_error(WifiDriverResult result)
{
    if (result == WifiDriverResult::Authentication) return WifiSetupError::Authentication;
    if (result == WifiDriverResult::NotFound) return WifiSetupError::NotFound;
    return WifiSetupError::Internal;
}
}

uint32_t wifi_setup_open(WifiSetupMethod method)
{
    std::lock_guard<std::mutex> lock(mutex);
    unsigned free = 0;
    if (!lease.compare_exchange_strong(free, 1)) return 0;
    state = WifiSetupSnapshot{}; state.operation = ++sequence;
    if (!state.operation) state.operation = ++sequence;
    touched = original_saved = saved_network = false;
    http_running = phone_booting = cleanup_started = false;
    state.method = method;
    state.phone_phase = method == WifiSetupMethod::Phone ? WifiPhonePhase::Starting : WifiPhonePhase::Off;
    phone_deadline = wifi_setup_now_ms() + 300000; success_deadline = 0;
    phone_display = {};
    cancel_requested = close_requested = connect_requested = false;
    scan_requested = true;
    prepare();
    return state.operation;
}
static bool scan_locked(uint32_t operation)
{
    if (!valid_operation(operation) || !editable()) return false;
    scan_requested = true; connect_requested = false; forget_password();
    // Invalidate row identities immediately, before owner starts refreshing.
    ++state.scan_revision; state.count = 0; ++state.revision;
    if (!voice_held) prepare();
    return true;
}
static bool connect_locked(uint32_t operation, uint32_t revision, uint8_t index, const char *password)
{
    if (!valid_operation(operation) || !editable() || scan_requested || connect_requested ||
        revision != state.scan_revision || index >= state.count) return false;
    auto &ap = state.aps[index];
    if (ap.security == WifiSetupSecurity::Unsupported) {
        state.error = WifiSetupError::Unsupported; ++state.revision; return false;
    }
    if (!password_valid(ap.security, password)) {
        state.error = WifiSetupError::InvalidPassword; ++state.revision; return false;
    }
    memcpy(candidate.ssid, ap.ssid, sizeof(candidate.ssid));
    memset(candidate.password, 0, sizeof(candidate.password));
    memcpy(candidate.password, password, strlen(password));
    connect_requested = true; ++state.revision;
    if (!voice_held) prepare();
    return true;
}
static void cancel_locked(uint32_t operation)
{
    if (state.method == WifiSetupMethod::Phone && !phone_active()) return;
    if (!valid_operation(operation) || state.phase == WifiSetupPhase::Saving) return;
    ++state.revision;
    if (phone_active() && state.phase == WifiSetupPhase::Connected) { close_phone(); return; }
    if (state.phase == WifiSetupPhase::Preparing && !touched) {
        if (!phone_active()) release_voice();
        scan_requested = connect_requested = false; forget_password();
        phase(WifiSetupPhase::Failed, WifiSetupError::Cancelled);
    } else if (state.phase == WifiSetupPhase::Connected) finish();
    else cancel_requested = true;
}
static void close_locked(uint32_t operation)
{
    if (!valid_operation(operation) || state.phase == WifiSetupPhase::Saving) return;
    if (state.method == WifiSetupMethod::Phone) {
        if (phone_active()) close_phone(WifiSetupError::Cancelled);
        else { saved_network = false; phase(WifiSetupPhase::Closed); }
        return;
    }
    if (saved_network) saved_network = false;
    if (!touched || state.phase == WifiSetupPhase::Connected || state.phase == WifiSetupPhase::Failed) finish();
    else { close_requested = true; cancel_requested = true; }
}
WifiSetupSnapshot wifi_setup_snapshot()
{
    std::lock_guard<std::mutex> lock(mutex);
    expire_preparing();
    auto view = state;
    view.remaining_seconds = remaining();
    view.command_pending = scan_requested || connect_requested || cancel_requested || close_requested;
    return view;
}
bool wifi_setup_requested()
{
    std::lock_guard<std::mutex> lock(mutex);
    expire_preparing();
    return phone_active() || (state.method == WifiSetupMethod::Screen && state.phase != WifiSetupPhase::Closed && state.phase != WifiSetupPhase::Connected);
}
bool wifi_setup_usb_acquire()
{
    unsigned free = 0;
    return lease.compare_exchange_strong(free, 2);
}
void wifi_setup_usb_release() { unsigned usb = 2; lease.compare_exchange_strong(usb, 0); }

bool wifi_setup_owner_step(GadgetWifiCred &credentials)
{
    std::unique_lock<std::mutex> lock(mutex);
    expire_preparing();
    if (state.method == WifiSetupMethod::Phone && !phone_active()) return false;
    if (state.phone_phase == WifiPhonePhase::Starting && state.phase == WifiSetupPhase::Failed) close_phone(state.error);
    if (phone_active() && state.phase != WifiSetupPhase::Saving &&
        (wifi_setup_now_ms() >= phone_deadline || (success_deadline && wifi_setup_now_ms() >= success_deadline))) close_phone();
    if (state.phone_phase == WifiPhonePhase::Closing) {
        if (http_running) {
            lock.unlock(); bool stopped = wifi_setup_http_stop(); lock.lock();
            if (!stopped) return true;
            http_running = false;
        }
        if (!cleanup_started) {
            cleanup_started = true; phone_booting = false;
            wifi_driver_phone_disable(); forget_password();
            if (!original_saved) { finish(); return false; }
            deadline = wifi_setup_now_ms() + 15000;
            if (!wifi_driver_join(original)) deadline = wifi_setup_now_ms();
        }
        auto result = wifi_driver_poll();
        bool connected = result == WifiDriverResult::Connected && wifi_driver_connected(original);
        if (connected || wifi_setup_now_ms() >= deadline || result == WifiDriverResult::Error ||
            result == WifiDriverResult::Authentication || result == WifiDriverResult::NotFound) {
            if (!wifi_driver_phone_stopped()) {
                deadline = wifi_setup_now_ms() + 15000;
                wifi_driver_join(original); return true;
            }
            state.original_connected = connected; finish(); return false;
        }
        return true;
    }
    if (state.phase == WifiSetupPhase::Closed || state.phase == WifiSetupPhase::Connected) {
        if (phone_active()) {
            wifi_driver_poll();
            if (wifi_driver_phone_overlap() || !wifi_driver_phone_ready()) close_phone();
        }
        return phone_active();
    }
    if (cancel_requested && state.phase != WifiSetupPhase::Restoring) {
        if (touched) restore(WifiSetupError::Cancelled);
        else { if (!phone_active()) release_voice(); forget_password(); phase(WifiSetupPhase::Failed, WifiSetupError::Cancelled); cancel_requested = false; }
    }
    if (state.phase == WifiSetupPhase::Preparing) {
        auto status = voice_settings_status(state.operation);
        if (status == VoiceSettingsStatus::Busy || status == VoiceSettingsStatus::Inactive) {
            if (!phone_active()) release_voice();
        scan_requested = connect_requested = false; forget_password();
            phase(WifiSetupPhase::Failed, WifiSetupError::VoiceBusy); return true;
        }
        if (status != VoiceSettingsStatus::Paused) return true;
        if (!original_saved) {
            memcpy(original.ssid, credentials.ssid, sizeof(original.ssid));
            memcpy(original.password, credentials.pass, sizeof(original.password));
            original_saved = true;
            if (phone_active() && wifi_driver_phone_overlap()) { close_phone(WifiSetupError::Internal); return true; }
        }
        phase(WifiSetupPhase::Choosing);
    }
    if (phone_booting) {
        auto result = wifi_driver_poll();
        if (wifi_driver_phone_overlap()) { close_phone(WifiSetupError::Internal); return true; }
        if (wifi_driver_phone_ready()) {
            phone_booting = false;
            lock.unlock(); bool started = wifi_setup_http_start(state.operation); lock.lock();
            // stop must also be attempted after a partially failed start.
            http_running = true;
            if (!started) close_phone(WifiSetupError::Internal);
            else if (state.phone_phase == WifiPhonePhase::Closing) close_phone();
            else { state.phone_phase = WifiPhonePhase::Serving; ++state.revision; }
        } else if (result == WifiDriverResult::Error || wifi_setup_now_ms() >= deadline) close_phone(WifiSetupError::Internal);
        return true;
    }
    if (editable() && scan_requested) {
        scan_requested = false; touched = true;
        ++state.scan_revision; state.count = 0;
        phase(WifiSetupPhase::Scanning); deadline = wifi_setup_now_ms() + 15000;
        if (!wifi_driver_scan()) restore(WifiSetupError::ScanFailed);
    } else if (editable() && connect_requested) {
        connect_requested = false; touched = true;
        phase(WifiSetupPhase::Connecting); deadline = wifi_setup_now_ms() + 15000;
        if (!wifi_driver_join(candidate)) restore(WifiSetupError::Internal);
    }
    if (state.phase == WifiSetupPhase::Scanning) {
        auto result = wifi_driver_poll();
        if (result == WifiDriverResult::ScanReady) {
            state.count = wifi_driver_results(state.aps, WIFI_SETUP_AP_LIMIT);
            wifi_driver_clear_scan();
            phase(WifiSetupPhase::Choosing, state.count ? WifiSetupError::None : WifiSetupError::NoNetworks);
            if (state.phone_phase == WifiPhonePhase::Starting) {
                if (!wifi_driver_phone_create(phone_display)) close_phone(WifiSetupError::Internal);
                else {
                    phone_booting = true; deadline = wifi_setup_now_ms() + 15000;
                    if (!wifi_driver_join(original)) close_phone(WifiSetupError::Internal);
                }
            }
        } else if (result != WifiDriverResult::Pending || wifi_setup_now_ms() >= deadline) restore(WifiSetupError::ScanFailed);
    } else if (state.phase == WifiSetupPhase::Connecting) {
        auto result = wifi_driver_poll();
        if (result == WifiDriverResult::Connected && wifi_driver_connected(candidate)) {
            // Mutex serializes cancellation against this irreversible boundary.
            if (phone_active() && wifi_driver_phone_overlap()) close_phone();
            phase(WifiSetupPhase::Saving);
            lock.unlock();
            auto saved = gadget_wifi_commit_network(candidate.ssid, candidate.password);
            lock.lock();
            if (saved == GadgetNetworkSave::Saved) {
                memcpy(credentials.ssid, candidate.ssid, sizeof(credentials.ssid));
                memcpy(credentials.pass, candidate.password, sizeof(credentials.pass));
                memcpy(state.connected_ssid, candidate.ssid, sizeof(state.connected_ssid));
                if (phone_active()) {
                    original = candidate; saved_network = true;
                    success_deadline = wifi_setup_now_ms() + 30000;
                }
                forget_password(); phase(WifiSetupPhase::Connected);
                if (phone_active() && wifi_driver_phone_overlap()) close_phone();
                return phone_active();
            }
            restore(saved == GadgetNetworkSave::RolledBack ? WifiSetupError::Storage : WifiSetupError::StorageUncertain);
        } else if (result != WifiDriverResult::Pending && result != WifiDriverResult::Connected) restore(driver_error(result));
        else if (wifi_setup_now_ms() >= deadline) restore(WifiSetupError::Timeout);
    } else if (state.phase == WifiSetupPhase::Restoring) {
        auto result = wifi_driver_poll();
        bool connected = result == WifiDriverResult::Connected && wifi_driver_connected(original);
        if (connected || wifi_setup_now_ms() >= deadline || result == WifiDriverResult::Authentication ||
            result == WifiDriverResult::NotFound || result == WifiDriverResult::Error) {
            state.original_connected = connected;
            touched = false;
            if (close_requested) finish();
            else phase(WifiSetupPhase::Failed, state.error);
        }
    }
    if (state.phone_phase == WifiPhonePhase::Serving && editable() && !scan_requested && !connect_requested) {
        wifi_driver_poll();
        if (!wifi_driver_phone_ready()) close_phone(WifiSetupError::Internal);
    }
    if (phone_active() && wifi_driver_phone_overlap()) close_phone(WifiSetupError::Internal);
    return phone_active() || (state.method == WifiSetupMethod::Screen && state.phase != WifiSetupPhase::Closed && state.phase != WifiSetupPhase::Connected);
}

bool wifi_setup_scan(uint32_t operation)
{
    std::lock_guard<std::mutex> lock(mutex);
    return state.method == WifiSetupMethod::Screen && scan_locked(operation);
}
bool wifi_setup_connect(uint32_t operation, uint32_t revision, uint8_t index, const char *password)
{
    std::lock_guard<std::mutex> lock(mutex);
    return state.method == WifiSetupMethod::Screen && connect_locked(operation, revision, index, password);
}
void wifi_setup_cancel(uint32_t operation)
{
    std::lock_guard<std::mutex> lock(mutex); cancel_locked(operation);
}
void wifi_setup_close(uint32_t operation)
{
    std::lock_guard<std::mutex> lock(mutex); close_locked(operation);
}
WifiPhoneDisplay wifi_setup_phone_display(uint32_t operation)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (operation != state.operation || !phone_active()) return {};
    auto display = phone_display; display.phase = state.phone_phase;
    display.remaining_seconds = remaining(); return display;
}
WifiPhoneReceipt wifi_setup_phone_submit(const WifiPhoneCommand &command)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!phone_active() || state.phone_phase != WifiPhonePhase::Serving ||
        command.operation != state.operation || wifi_setup_now_ms() >= phone_deadline) return WifiPhoneReceipt::Closed;
    if (command.revision != state.revision) return WifiPhoneReceipt::Stale;
    if (state.revision == UINT32_MAX) { close_phone(); return WifiPhoneReceipt::Closed; }
    if (state.phase == WifiSetupPhase::Saving) return WifiPhoneReceipt::Busy;
    if (command.action == WifiPhoneAction::Close) { close_locked(command.operation); return WifiPhoneReceipt::Accepted; }
    if (command.action == WifiPhoneAction::Cancel) {
        if (state.phase == WifiSetupPhase::Restoring || cancel_requested) return WifiPhoneReceipt::Busy;
        cancel_locked(command.operation); return WifiPhoneReceipt::Accepted;
    }
    if (!editable() || scan_requested || connect_requested || cancel_requested || close_requested) return WifiPhoneReceipt::Busy;
    if (command.action == WifiPhoneAction::Scan) {
        scan_locked(command.operation); return WifiPhoneReceipt::Accepted;
    }
    if (command.action != WifiPhoneAction::Connect || command.scan_revision != state.scan_revision || command.index >= state.count ||
        !password_valid(state.aps[command.index].security, command.password)) return WifiPhoneReceipt::Invalid;
    return connect_locked(command.operation, command.scan_revision, command.index, command.password) ?
        WifiPhoneReceipt::Accepted : WifiPhoneReceipt::Busy;
}
