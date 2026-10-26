module;

#include <common.hxx>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "FusionLog.hpp"

export module inireload;

import settings;

// The settings taken on the fly. Each module with settings it can take while the game runs adds a reader, called with
// the settings' reader, in the order they were added: after a value changed (the menu, the overlay, FusionFixHub, an
// edit of GTAIV.EFLC.FusionFix.cfg), and on Ctrl+Shift+F10, which reads the cfg again. What sizes buffers or installs
// hooks at startup is read there only and keeps its value until the game restarts.
namespace IniReload
{
    static std::recursive_mutex mutex; // a reader may add one
    static std::vector<std::function<void(SettingsReader&)>> readers;
    static std::atomic<bool> requested{false}; // the debug server's reload, done by the next Poll
    static std::atomic<bool> changed{false};   // a value changed, read again by the next Poll

    export void Add(std::function<void(SettingsReader&)> reader)
    {
        std::lock_guard lock(mutex);
        if (readers.empty())
        {
            // On the thread that changed it (the file watcher's for edits of the cfg): the readers run at the next Poll
            for (auto& setting : SettingsPlugin().GetDescription().settings)
                SettingsPlugin().OnChange(setting.id, [] { changed = true; });
        }
        readers.push_back(std::move(reader));
    }

    // From any thread: the cfg is read again and the readers run at the next Poll, on the thread that polls, as for
    // the keys.
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
        if (pressed || asked)
            SettingsPlugin().Reload();
        // Reload's changes come through the change callbacks too
        const bool reread = changed.exchange(false);
        if (!pressed && !asked && !reread)
            return;
        SettingsReader reader;
        std::lock_guard lock(mutex);
        for (auto& read : readers)
            read(reader);
        if (pressed || asked)
        {
            FusionLog::Write("IniReload", pressed ? "Keys" : "DebugServer", "%s: the settings read again, %zu readers\n",
                pressed ? "Ctrl+Shift+F10" : "Debug server", readers.size());
        }
        if (pressed)
            MessageBeep(MB_OK);
    }
}
