#pragma once
#include <cstddef>
#include <cstring>

// A message is released to the JSON reader only after its final frame and LF.
class GadgetWsMessage {
public:
    enum class Result { More, Message, Ping, Pong, Closed, Invalid };
    static constexpr size_t Max = 2048;
    char data[Max]{};
    char control[125]{};
    size_t size = 0, control_size = 0;
    bool in_frame() const { return remaining != 0; }
    void reset() { size = control_size = remaining = 0; fragmented = false; }

    Result append(unsigned opcode, bool fin, int payload_size, const char *chunk, size_t count)
    {
        if (!remaining) {
            if (payload_size < 0) return Result::Invalid;
            frame_opcode = opcode;
            frame_fin = fin;
            remaining = static_cast<size_t>(payload_size);
            if (opcode & 8) {
                if (!fin || remaining > sizeof(control) || (opcode != 8 && opcode != 9 && opcode != 10))
                    return Result::Invalid;
                control_size = 0;
            } else {
                if ((opcode != 0 && opcode != 2) || (opcode == 0) != fragmented ||
                    remaining > Max - size) return Result::Invalid;
            }
        }
        if (count > remaining) return Result::Invalid;
        if (frame_opcode & 8) {
            memcpy(control + control_size, chunk, count);
            control_size += count;
        } else {
            memcpy(data + size, chunk, count);
            size += count;
        }
        remaining -= count;
        if (remaining) return Result::More;
        if (frame_opcode == 8) return Result::Closed;
        if (frame_opcode == 9) return Result::Ping;
        if (frame_opcode == 10) return Result::Pong;
        fragmented = !frame_fin;
        if (!frame_fin) return Result::More;
        if (size < 2 || data[size - 1] != '\n' || memchr(data, '\n', size - 1) ||
            memchr(data, '\0', size) || memchr(data, '\r', size)) return Result::Invalid;
        return Result::Message;
    }
private:
    size_t remaining = 0;
    unsigned frame_opcode = 0;
    bool frame_fin = false, fragmented = false;
};
