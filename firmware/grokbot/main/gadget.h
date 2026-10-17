// Grok Gadgets protocol 0.1.0 client for the grokbot firmware.
//
// The gadget task owns the USB Serial/JTAG port and the protocol session
// (hello / poll / ack / event / state, LF-delimited JSON). LVGL-side code only
// sees validated typed structs crossing two FreeRTOS queues:
//   commands  gadget -> LVGL:  gadget_command_take() / gadget_command_done()
//   events    LVGL -> gadget:  gadget_emit_*()
// State snapshots (gadget_snapshot_t) are plain values the gadget task may read
// directly; they are refreshed by the LVGL task via gadget_publish_state().
#pragma once
#include <stdint.h>

/* ---- commands (gadget -> LVGL) ---- */

enum class GadgetCmdType : int { SetCharacter, SetBacklight, TriggerAnimation, Poke };

struct GadgetCommand {
    GadgetCmdType type;
    char cmd_id[68];   // protocol id cap is 64 chars ("gw-" + 32 hex typical)
    char name[16];     // SetCharacter / TriggerAnimation argument
    int percent;       // SetBacklight argument
};

/* ---- events (LVGL -> gadget); names are fixed by the hello capabilities ---- */

enum class GadgetEventType : int { Poke, Shake, Switched };

struct GadgetEvent {
    GadgetEventType type;
    float jerk;        // Shake
    char name[16];     // Switched: new character name
    bool remote;       // Poke: true when triggered by a remote poke command
};

/* ---- state snapshot (published by LVGL, read by the gadget task) ---- */

struct GadgetState {
    char character[16];   // current skin name
    char anim[12];        // carousel state name, "gravity" when tilted, "dizzy"
    float gx, gy;         // smoothed tilt (m/s^2)
    int backlight;        // 0..100
    int battery_pct;      // -1 when unknown
    bool battery_charging;
    uint32_t uptime_s;
    uint32_t display_fps;
};

/* ---- gadget task side ---- */

// Start the USB transport + protocol task. Call once from app_main after the
// LVGL pipeline is up.
void gadget_start();

/* ---- LVGL task side (call from frame_cb context only) ---- */

// Copy the next pending command; false when the queue is empty.
bool gadget_command_take(GadgetCommand &out);
// Report the executed command; `ok=false` sends a failed ack with `error`.
void gadget_command_done(const GadgetCommand &cmd, bool ok, const char *error);

void gadget_emit_poke(bool remote);
void gadget_emit_shake(float jerk);
void gadget_emit_switched(const char *character);

// Publish the current snapshot (cheap; call every frame or on change).
void gadget_publish_state(const GadgetState &s);
