#pragma once

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// FusionFix's logs, all in one place and one format: a file a feature, GTAIV.EFLC.FusionFix.<Feature>.log
// next to the plugin, emptied by the first line of a run, each line "[hh:mm:ss.mmm] [Feature.Component] text".
// Safe from any thread. The crash and update reports (crashlog.hpp) keep their own files.
namespace FusionLog
{
    // The folder of the module this code is built into: the plugin's, or the helper's.
    inline const std::filesystem::path& Directory()
    {
        static const std::filesystem::path directory = []
        {
            static const char anchor = 0;
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&anchor), &module);
            wchar_t buffer[MAX_PATH * 2] = {};
            GetModuleFileNameW(module, buffer, DWORD(std::size(buffer)));
            return std::filesystem::path(buffer).parent_path();
        }();
        return directory;
    }

    // A feature's log file, also for what writes a file of its own beside it (binary traces).
    inline std::filesystem::path PathFor(std::string_view feature, std::wstring_view extension = L".log")
    {
        return Directory() / (L"GTAIV.EFLC.FusionFix." + std::wstring(feature.begin(), feature.end()) + std::wstring(extension));
    }

    struct State
    {
        std::mutex mutex;
        std::set<std::string, std::less<>> started; // features written to in this run
    };
    inline State& GetState()
    {
        static State state;
        return state;
    }

    // Writes text, one or more lines, each with the time and the feature and component it comes from.
    inline void WriteText(std::string_view feature, std::string_view component, std::string_view text)
    {
        if (feature.empty() || text.empty())
            return;
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        std::string tag = "[";
        tag += feature;
        if (!component.empty())
        {
            tag += '.';
            tag += component;
        }
        tag += "] ";

        std::string out;
        for (size_t pos = 0; pos < text.size();)
        {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            out += stamp;
            out += tag;
            out += line;
            out += '\n';
            pos = end + 1;
        }

        auto& state = GetState();
        std::lock_guard lock(state.mutex);
        const bool first = state.started.emplace(feature).second;
        FILE* file = nullptr;
        if (_wfopen_s(&file, PathFor(feature).c_str(), first ? L"w" : L"a") || !file)
            return;
        fwrite(out.data(), 1, out.size(), file);
        fclose(file);
    }

    inline std::string FormatV(const char* format, va_list args)
    {
        va_list copy;
        va_copy(copy, args);
        const int size = vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        if (size <= 0)
            return {};
        std::string text(size_t(size), '\0');
        vsnprintf(text.data(), text.size() + 1, format, args);
        return text;
    }

    inline void WriteV(std::string_view feature, std::string_view component, const char* format, va_list args)
    {
        WriteText(feature, component, FormatV(format, args));
    }

    inline void Write(std::string_view feature, std::string_view component, const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteV(feature, component, format, args);
        va_end(args);
    }

    // Lines written together, such as a diagnostics dump or a profiler table: Printf gathers text,
    // Component tags what follows with another component, and the block is written when it goes.
    class Block
    {
    public:
        Block(std::string_view feature, std::string_view component) : feature(feature), component(component) {}
        ~Block()
        {
            Flush();
            for (auto& [part, text] : parts)
                WriteText(feature, part, text);
        }
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;

        void Component(std::string_view next)
        {
            Flush();
            component = next;
        }

        void Printf(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            pending += FormatV(format, args);
            va_end(args);
        }

    private:
        void Flush()
        {
            if (!pending.empty())
                parts.emplace_back(component, std::move(pending));
            pending.clear();
        }

        std::string feature;
        std::string component;
        std::string pending;
        std::vector<std::pair<std::string, std::string>> parts;
    };
}
