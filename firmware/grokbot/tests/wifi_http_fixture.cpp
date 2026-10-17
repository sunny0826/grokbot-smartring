// Browser fixture boundary: production JSON parser/serializer, synthetic radio state.
#include "wifi_setup_http_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    if (argc > 1 && !std::strcmp(argv[1], "parse")) {
        std::string input;
        std::getline(std::cin, input, '\0');
        WifiPhoneCommand command{};
        const bool valid = wifi_http_parse_command(input.data(), input.size(), command);
        wifi_http_wipe(input.data(), input.size());
        if (!valid) return 1;
        std::printf("{\"operation\":%u,\"revision\":%u,\"action\":%d,\"scan_revision\":%u,\"index\":%u}\n",
                    command.operation, command.revision, static_cast<int>(command.action), command.scan_revision, command.index);
        wifi_http_wipe(&command, sizeof(command));
        return 0;
    }
    if (argc != 7) return 2;
    WifiSetupSnapshot s{};
    s.operation = 7; s.revision = std::strtoul(argv[2], nullptr, 10);
    s.scan_revision = std::strtoul(argv[3], nullptr, 10);
    s.remaining_seconds = std::strtoul(argv[4], nullptr, 10);
    s.method = WifiSetupMethod::Phone; s.phone_phase = WifiPhonePhase::Serving;
    s.original_connected = true;
    const std::string phase = argv[1];
    s.phase = phase == "connected" ? WifiSetupPhase::Connected : phase == "failed" ? WifiSetupPhase::Failed :
              phase == "connecting" ? WifiSetupPhase::Connecting : phase == "saving" ? WifiSetupPhase::Saving :
              phase == "scanning" ? WifiSetupPhase::Scanning : WifiSetupPhase::Choosing;
    s.error = s.phase == WifiSetupPhase::Failed ? WifiSetupError::Authentication : WifiSetupError::None;
    s.command_pending = !std::strcmp(argv[5], "1");
    const char *names[] = {"家里的 Wi-Fi", "Guest · 开放网络", "企业网络", "<img src=x onerror=alert(1)>", "\xff\x01"};
    s.count = !std::strcmp(argv[6], "empty") ? 0 : 5;
    for (uint8_t i = 0; i < s.count; ++i) {
        std::strncpy(s.aps[i].ssid, names[i], 32); s.aps[i].rssi = -42 - 9 * i;
        s.aps[i].security = i == 1 ? WifiSetupSecurity::Open : i == 2 ? WifiSetupSecurity::Unsupported : WifiSetupSecurity::Personal;
    }
    if (s.phase == WifiSetupPhase::Connected) std::strcpy(s.connected_ssid, names[0]);
    char output[WIFI_HTTP_STATUS_LIMIT];
    if (!wifi_http_serialize(s, output, sizeof(output))) return 3;
    std::puts(output);
}
