#include "wifi_setup_keyboard.h"
#include <cstring>

namespace {
const char *const alphabets[] = {
    "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789 ",
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
};
}
void WifiSetupKeyboard::clear()
{
    volatile char *p = password;
    for (size_t i = 0; i < sizeof(password); ++i) p[i] = 0;
    length = mode = page = 0;
}
bool WifiSetupKeyboard::append(char c)
{
    if (length == 64 || c < 32 || c > 126) return false;
    password[length++] = c;
    password[length] = 0;
    return true;
}
void WifiSetupKeyboard::erase()
{
    if (length) password[--length] = 0;
}
bool WifiSetupKeyboard::valid() const
{
    if (length >= 8 && length <= 63) return true;
    if (length != 64) return false;
    for (unsigned i = 0; i < length; ++i) {
        const char c = password[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}
const char *WifiSetupKeyboard::mode_name() const
{
    static const char *const names[] = {"abc", "ABC", "123", "符号"};
    return names[mode];
}
unsigned WifiSetupKeyboard::pages() const { return (std::strlen(alphabets[mode]) + 14) / 15; }
char WifiSetupKeyboard::key(unsigned index) const
{
    const size_t offset = page * 15 + index;
    return offset < std::strlen(alphabets[mode]) ? alphabets[mode][offset] : 0;
}
void WifiSetupKeyboard::next_mode() { mode = (mode + 1) % 4; page = 0; }
void WifiSetupKeyboard::next_page() { page = (page + 1) % pages(); }
