#pragma once
#include <cstddef>

struct WifiSetupKeyboard {
    char password[65] = {};
    unsigned length = 0;
    unsigned mode = 0;
    unsigned page = 0;
    void clear();
    bool append(char c);
    void erase();
    bool valid() const;
    const char *mode_name() const;
    unsigned pages() const;
    char key(unsigned index) const;
    void next_mode();
    void next_page();
};
