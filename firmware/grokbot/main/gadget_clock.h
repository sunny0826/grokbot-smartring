#pragma once
bool gadget_clock_ready();
// Called only by the control task after Wi-Fi acquires an IP address.
bool gadget_clock_wait();
