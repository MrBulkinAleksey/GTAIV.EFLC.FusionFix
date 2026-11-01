#pragma once
#include <cstdint>

// A slot's output copy precedes the next input copy into that same slot on the
// game's queue. That output copy waits for the helper's completion fence; a CPU
// acknowledgement only confirms submission. Reconfiguration flushes discarded
// epochs before releasing their shared resources.
namespace UpscalerRing
{
    struct Entry
    {
        uint32_t slot = 0;
        uint64_t id = 0, value = 0;
    };
    class State
    {
        uint64_t epoch = 1;
        uint32_t next = 0;
        Entry previous{};
        bool reset = true;
    public:
        void Reset() { ++epoch; previous = {}; reset = true; }
        bool NeedsReset() const { return reset; }
        uint64_t Epoch() const { return epoch; }
        uint32_t Next() const { return next; }
        bool HasPrevious() const { return previous.value != 0; }
        Entry Previous() const { return previous; }
        void Push(uint32_t slot, uint64_t id, uint64_t value)
        {
            previous = { slot, id, value };
            reset = false;
            next = (slot + 1) % 3;
        }
    };
}
