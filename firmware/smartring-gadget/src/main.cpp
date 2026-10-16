// SPDX-License-Identifier: MIT
// SmartRing-Plus minimal Grok Gadgets firmware (USB serial transport).
//
// Hardware facts from firmware/grokbot/components/smartring_plus (BSP):
//   - Backlight: GPIO46, high = on (LEDC in the full firmware; plain GPIO here).
//   - Battery V1: GPIO1 ADC divider; V2 uses AXP2101 over I2C (not read here,
//     the raw ADC millivolts are reported either way as battery_mv_raw).
//
// Commands:
//   backlight.set {"on": bool}  — switch the display backlight.
// State:
//   backlight.on, uptime_s, free_heap, battery_mv_raw.
// Events: none yet (touch CST816S and IMU QMI8658A need their I2C drivers;
// they arrive with the IDF-ported gadget, not this minimal sketch).
#include <GrokSession.h>

constexpr int BacklightPin = 46;
constexpr int BatteryAdcPin = 1;

// Global or static only. Gadget is about 40 KB and overflows the 8 KB loop stack.
grok::Gadget gadget("SmartRing-Plus", "0.1.0", "smartring");

bool backlightOn = false;

void reportState(JsonObject state, void *) {
  state["backlight"]["on"] = backlightOn;
  state["uptime_s"] = millis() / 1000;
  state["free_heap"] = ESP.getFreeHeap();
  state["battery_mv_raw"] = analogReadMilliVolts(BatteryAdcPin);
}

grok::Error setBacklight(JsonObjectConst args, void *) {
  if (args.size() != 1 || !args["on"].is<bool>())
    return {"invalid_arguments", "Expected {\"on\": boolean}"};
  backlightOn = args["on"];
  digitalWrite(BacklightPin, backlightOn ? HIGH : LOW);
  return {};
}

void setup() {
  pinMode(BacklightPin, OUTPUT);
  digitalWrite(BacklightPin, LOW);
  gadget.state(reportState);
  gadget.command("backlight.set", setBacklight, nullptr,
                 R"({"type":"object","description":"Switch the display backlight on or off",)"
                 R"("properties":{"on":{"type":"boolean"}},"required":["on"],)"
                 R"("additionalProperties":false})");
  gadget.begin(); // USB serial at 115200; usb-bridge on the Mac relays to the gateway.
}

void loop() { gadget.loop(); }
