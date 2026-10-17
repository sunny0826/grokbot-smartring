#pragma once
#include "voice_receipt.h"
struct GadgetWifiCred;

bool voice_current_scope(VoiceScope &scope);
bool voice_scope_matches(const VoiceScope &scope, const GadgetWifiCred &credentials);

