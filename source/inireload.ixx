module;

#include <common.hxx>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "FusionLog.hpp"

export module inireload;

// Ctrl+Shift+F10 reads the ini again while the game runs. Each module with settings it can take on the fly adds a
// reader, called with the file's reader, in the order they were added. What sizes buffers or installs hooks at startup
// is read there only and keeps its value until the game restarts.
namespace IniReload
{
    static std::recursive_mutex mutex; // a reader may add one
    static std::vector<std::function<void(CIniReader&)>> readers;
    static std::atomic<bool> requested{false}; // the debug server's reload, done by the next Poll

    export void Add(std::function<void(CIniReader&)> reader)
    {
        std::lock_guard lock(mutex);
        readers.push_back(std::move(reader));
    }

    // From any thread: the readers run at the next Poll, on the thread that polls, as for the keys.
    export void Request()
    {
        requested = true;
    }

    // Once a frame, from the post fx pass, which runs in the pause menu too.
    export void Poll()
    {
        static bool keyWasDown = false;
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
            (GetAsyncKeyState(VK_F10) & 0x8000);
        const bool pressed = down && !keyWasDown;
        keyWasDown = down;
        const bool asked = requested.exchange(false);
        if (!pressed && !asked)
            return;
        CIniReader iniReader("");
        std::lock_guard lock(mutex);
        for (auto& reader : readers)
            reader(iniReader);
        FusionLog::Write("IniReload", pressed ? "Keys" : "DebugServer", "%s: the ini read again, %zu readers\n",
            pressed ? "Ctrl+Shift+F10" : "Debug server", readers.size());
        if (pressed)
            MessageBeep(MB_OK);
    }
}
