module;

#include <common.hxx>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <d3dx9.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "FusionLog.hpp"

#pragma comment(lib, "ws2_32.lib")

export module debugserver;

import common;
import comvars;
import d3dx9_43;
import inireload;

// [DEBUG] DebugServerPort: a server on 127.0.0.1 that a tool (tools/gta4-live-mcp in the recomp's repository) reaches
// through an SSH tunnel, to look into the running game from another machine: memory (read, search, signatures, and
// writes with DebugServerAllowWrite), the loaded modules, the ini (read, write and read again as Ctrl+Shift+F10 does)
// and the logs. One client at a time; a request is a JSON object on one line ({"id": 1, "cmd": "read", ...}), the
// answer one line ({"id": 1, "ok": true, ...}), the same protocol as the recomp's debug server. Bytes travel as hex in
// memory order (little-endian here), addresses as "0x..." strings.
namespace DebugServer
{
    constexpr size_t kMaxReadBytes = 1u << 20;
    constexpr size_t kMaxRequestBytes = 4u << 20;
    constexpr size_t kMaxMatches = 1000;
    constexpr size_t kMaxLogLines = 5000;

    static bool allowWrite = false;

    // ---- requests: a flat JSON object of strings, numbers and booleans ----------------------------------------------

    class Request
    {
    public:
        explicit Request(std::string_view text)
        {
            size_t at = 0;
            Skip(text, at);
            Expect(text, at, '{');
            Skip(text, at);
            if (Peek(text, at) == '}')
                return;
            for (;;)
            {
                Skip(text, at);
                std::string key = String(text, at);
                Skip(text, at);
                Expect(text, at, ':');
                Skip(text, at);
                values[key] = Value(text, at);
                Skip(text, at);
                if (Peek(text, at) == ',')
                {
                    ++at;
                    continue;
                }
                Expect(text, at, '}');
                return;
            }
        }

        bool Has(const std::string& key) const { return values.contains(key); }
        std::string Get(const std::string& key) const
        {
            auto it = values.find(key);
            if (it == values.end())
                throw std::runtime_error("missing \"" + key + "\"");
            return it->second;
        }
        std::string Get(const std::string& key, std::string fallback) const { return Has(key) ? Get(key) : fallback; }
        uint64_t Number(const std::string& key) const { return std::stoull(Get(key), nullptr, 0); }
        uint64_t Number(const std::string& key, uint64_t fallback) const { return Has(key) ? Number(key) : fallback; }
        // The id as it came, to echo (a number or a string, here kept as JSON text).
        std::string idJson = "null";

    private:
        std::map<std::string, std::string> values;

        static char Peek(std::string_view text, size_t at) { return at < text.size() ? text[at] : '\0'; }
        static void Skip(std::string_view text, size_t& at)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
                ++at;
        }
        static void Expect(std::string_view text, size_t& at, char c)
        {
            if (Peek(text, at) != c)
                throw std::runtime_error(std::string("bad request: expected '") + c + "'");
            ++at;
        }
        static std::string String(std::string_view text, size_t& at)
        {
            Expect(text, at, '"');
            std::string out;
            while (at < text.size() && text[at] != '"')
            {
                char c = text[at++];
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                char e = Peek(text, at++);
                switch (e)
                {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u':
                {
                    // (keys and values here are ASCII: a \u escape keeps its low byte)
                    out += char(std::stoul(std::string(text.substr(at, 4)), nullptr, 16) & 0xFF);
                    at += 4;
                    break;
                }
                default: out += e; break;
                }
            }
            Expect(text, at, '"');
            return out;
        }
        std::string Value(std::string_view text, size_t& at)
        {
            if (Peek(text, at) == '"')
                return String(text, at);
            size_t begin = at;
            while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ' ')
                ++at;
            return std::string(text.substr(begin, at - begin));
        }

    };

    Request Parse(std::string_view text)
    {
        Request request(text);
        if (request.Has("id"))
        {
            const std::string id = request.Get("id");
            const bool numeric = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return isdigit(uint8_t(c)); });
            request.idJson = numeric ? id : "\"" + id + "\"";
        }
        return request;
    }

    // ---- answers ----------------------------------------------------------------------------------------------------

    std::string Quote(std::string_view text)
    {
        std::string out = "\"";
        for (char c : text)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20)
                {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", uint8_t(c));
                    out += buffer;
                }
                else if (uint8_t(c) >= 0x80)
                    out += '?'; // (logs and paths may hold other code pages; JSON wants UTF-8)
                else
                    out += c;
            }
        }
        return out + "\"";
    }

    std::string Address(uintptr_t address)
    {
        char buffer[24];
        snprintf(buffer, sizeof(buffer), "\"0x%08X\"", unsigned(address));
        return buffer;
    }

    // Fields of an answer object, each already JSON.
    using Fields = std::vector<std::pair<std::string, std::string>>;

    std::string Array(const std::vector<std::string>& items)
    {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i)
            out += (i ? "," : "") + items[i];
        return out + "]";
    }

    std::string Object(const Fields& fields)
    {
        std::string out = "{";
        for (size_t i = 0; i < fields.size(); ++i)
            out += (i ? "," : "") + Quote(fields[i].first) + ":" + fields[i].second;
        return out + "}";
    }

    std::string ToHex(const uint8_t* bytes, size_t size)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string text(size * 2, '0');
        for (size_t i = 0; i < size; ++i)
        {
            text[i * 2] = digits[bytes[i] >> 4];
            text[i * 2 + 1] = digits[bytes[i] & 0xF];
        }
        return text;
    }

    std::vector<uint8_t> FromHex(const std::string& text)
    {
        if (text.size() % 2)
            throw std::runtime_error("hex has an odd number of digits");
        std::vector<uint8_t> bytes(text.size() / 2);
        for (size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = uint8_t(std::stoul(text.substr(i * 2, 2), nullptr, 16));
        return bytes;
    }

    // ---- memory -----------------------------------------------------------------------------------------------------

    bool Readable(DWORD protect)
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        return protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY);
    }

    bool Writable(DWORD protect)
    {
        if (protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        return protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    }

    // Whether all of [address, address + size) is committed with the access.
    bool RangeAccessible(uintptr_t address, size_t size, bool write)
    {
        if (!size || address + size < address)
            return false;
        for (uintptr_t at = address; at < address + size;)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
                !(write ? Writable(info.Protect) : Readable(info.Protect)))
                return false;
            at = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        return true;
    }

    // A copy that a page freed meanwhile turns into a failure, not a crash (no C++ objects here: __try).
    bool SafeCopy(void* destination, const void* source, size_t size)
    {
        __try
        {
            memcpy(destination, source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // The pattern's matches in [first, last), up to `limit` in `out`; false if the memory went away.
    bool SafeSearch(const uint8_t* first, const uint8_t* last, const uint8_t* pattern, size_t size, uintptr_t* out,
        size_t limit, size_t* found)
    {
        __try
        {
            for (const uint8_t* at = first; at + size <= last && *found < limit; ++at)
            {
                at = static_cast<const uint8_t*>(memchr(at, pattern[0], size_t(last - at)));
                if (!at || at + size > last)
                    break;
                if (!memcmp(at, pattern, size))
                    out[(*found)++] = reinterpret_cast<uintptr_t>(at);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string Base64(const uint8_t* bytes, size_t size)
    {
        static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        for (size_t i = 0; i < size; i += 3)
        {
            const uint32_t chunk = uint32_t(bytes[i]) << 16 | (i + 1 < size ? uint32_t(bytes[i + 1]) << 8 : 0) |
                (i + 2 < size ? uint32_t(bytes[i + 2]) : 0);
            out += table[chunk >> 18 & 63];
            out += table[chunk >> 12 & 63];
            out += i + 1 < size ? table[chunk >> 6 & 63] : '=';
            out += i + 2 < size ? table[chunk & 63] : '=';
        }
        return out;
    }

    // ---- screenshots: asked for by the server's thread, taken on the render thread before the frame is presented

    struct Capture
    {
        std::mutex mutex;
        std::condition_variable done;
        std::atomic<bool> pending{false};
        bool finished = false;
        uint32_t maxWidth = 0;
        std::vector<uint8_t> png;
        uint32_t width = 0, height = 0;
        std::string error;
    };
    static Capture capture;

    // The back buffer, scaled to at most maxWidth wide (which also resolves MSAA), into system memory, as PNG.
    void TakeScreenshot()
    {
        if (!capture.pending)
            return;
        std::string error;
        std::vector<uint8_t> png;
        UINT width = 0, height = 0;
        IDirect3DSurface9* backBuffer = nullptr;
        IDirect3DSurface9* target = nullptr;
        IDirect3DSurface9* memory = nullptr;
        ID3DXBuffer* file = nullptr;
        auto* device = RageDirect3DDevice9::GetRuntimeDevice();
        using SaveFn = decltype(&D3DXSaveSurfaceToFileInMemory);
        auto save = reinterpret_cast<SaveFn>(GetProcAddress(GetD3DX9_43DLL(), "D3DXSaveSurfaceToFileInMemory"));
        D3DSURFACE_DESC desc{};
        if (!device || !save)
            error = "no device or no d3dx9_43";
        else if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || FAILED(backBuffer->GetDesc(&desc)))
            error = "no back buffer";
        else
        {
            width = desc.Width;
            height = desc.Height;
            if (capture.maxWidth && width > capture.maxWidth)
            {
                height = std::max<UINT>(1, UINT(uint64_t(height) * capture.maxWidth / width));
                width = capture.maxWidth;
            }
            // (8-bit first; the back buffer's own format when the device cannot convert to it)
            for (D3DFORMAT format : {D3DFMT_X8R8G8B8, desc.Format})
            {
                if (SUCCEEDED(device->CreateRenderTarget(width, height, format, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)) &&
                    SUCCEEDED(device->StretchRect(backBuffer, nullptr, target, nullptr, D3DTEXF_LINEAR)) &&
                    SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, format, D3DPOOL_SYSTEMMEM, &memory, nullptr)) &&
                    SUCCEEDED(device->GetRenderTargetData(target, memory)))
                    break;
                if (memory) memory->Release(), memory = nullptr;
                if (target) target->Release(), target = nullptr;
            }
            if (!memory)
                error = "the back buffer could not be copied";
            else if (FAILED(save(&file, D3DXIFF_PNG, memory, nullptr, nullptr)) || !file)
                error = "PNG encoding failed";
            else
            {
                const auto* bytes = static_cast<const uint8_t*>(file->GetBufferPointer());
                png.assign(bytes, bytes + file->GetBufferSize());
            }
        }
        for (IUnknown* object : std::initializer_list<IUnknown*>{file, memory, target, backBuffer})
            if (object)
                object->Release();
        {
            std::lock_guard lock(capture.mutex);
            capture.png = std::move(png);
            capture.width = width;
            capture.height = height;
            capture.error = std::move(error);
            capture.finished = true;
            capture.pending = false;
        }
        capture.done.notify_all();
    }

    std::string Screenshot(const Request& request)
    {
        std::unique_lock lock(capture.mutex);
        capture.finished = false;
        capture.maxWidth = uint32_t(request.Number("max_width", 1920));
        capture.pending = true;
        // (the game presents in its menus and while paused too; a minimized game does not)
        if (!capture.done.wait_for(lock, std::chrono::seconds(10), [] { return capture.finished; }))
        {
            capture.pending = false;
            throw std::runtime_error("no frame was presented within 10 s (is the game minimized?)");
        }
        if (!capture.error.empty())
            throw std::runtime_error(capture.error);
        const auto path = FusionLog::PathFor("Screenshot", L".png");
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(capture.png.data()), std::streamsize(capture.png.size()));
        return Object({{"mime", Quote("image/png")}, {"width", std::to_string(capture.width)},
            {"height", std::to_string(capture.height)}, {"file", Quote(path.string())},
            {"data", Quote(Base64(capture.png.data(), capture.png.size()))}});
    }

    std::string Info()
    {
        HMODULE game = GetModuleHandleW(nullptr);
        HMODULE plugin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Info), &plugin);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(game, exe, MAX_PATH);
        return Object({
            {"title", Quote("GTA IV (PC, FusionFix)")},
            {"exe", Quote(std::filesystem::path(exe).filename().string())},
            {"exe_base", Address(reinterpret_cast<uintptr_t>(game))},
            {"plugin_base", Address(reinterpret_cast<uintptr_t>(plugin))},
            {"pid", std::to_string(GetCurrentProcessId())},
            {"pointer_size", std::to_string(sizeof(void*))},
            {"endianness", Quote("little")},
            {"allow_write", allowWrite ? "true" : "false"},
            {"ini", Quote(CIniReader("").GetIniPath().string())},
            {"logs_dir", Quote(FusionLog::Directory().string())},
        });
    }

    std::string Modules()
    {
        std::vector<std::string> modules;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            throw std::runtime_error("no module snapshot");
        MODULEENTRY32W entry{sizeof(entry)};
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
        {
            char name[MAX_MODULE_NAME32 + 1] = {};
            WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
            modules.push_back(Object({{"name", Quote(name)}, {"base", Address(reinterpret_cast<uintptr_t>(entry.modBaseAddr))},
                {"size", std::to_string(entry.modBaseSize)}}));
        }
        CloseHandle(snapshot);
        return Object({{"modules", Array(modules)}});
    }

    std::string Read(const Request& request)
    {
        const uintptr_t address = uintptr_t(request.Number("addr"));
        const size_t size = size_t(request.Number("size"));
        if (!size || size > kMaxReadBytes)
            throw std::runtime_error("size is 1..1048576 bytes");
        if (!RangeAccessible(address, size, false))
            throw std::runtime_error("the range is not all committed readable");
        std::vector<uint8_t> bytes(size);
        if (!SafeCopy(bytes.data(), reinterpret_cast<const void*>(address), size))
            throw std::runtime_error("the memory went away while reading");
        return Object({{"hex", Quote(ToHex(bytes.data(), size))}});
    }

    // Data and, with "force", code: a page that is readable but not writable is opened for the copy and closed again.
    std::string Write(const Request& request)
    {
        if (!allowWrite)
            throw std::runtime_error("writes are off ([DEBUG] DebugServerAllowWrite)");
        const uintptr_t address = uintptr_t(request.Number("addr"));
        const std::vector<uint8_t> bytes = FromHex(request.Get("hex"));
        if (bytes.empty() || bytes.size() > kMaxReadBytes)
            throw std::runtime_error("1..1048576 bytes");
        const bool force = request.Get("force", "false") == "true";
        DWORD oldProtect = 0;
        const bool opened = !RangeAccessible(address, bytes.size(), true);
        if (opened)
        {
            if (!force || !RangeAccessible(address, bytes.size(), false))
                throw std::runtime_error("the range is not all committed writable (\"force\": true opens read-only pages)");
            if (!VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
                throw std::runtime_error("VirtualProtect failed");
        }
        const bool copied = SafeCopy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
        if (opened)
        {
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(address), bytes.size(), oldProtect, &ignored);
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size());
        if (!copied)
            throw std::runtime_error("the memory went away while writing");
        FusionLog::Write("DebugServer", "Write", "%zu bytes at 0x%08X%s\n", bytes.size(), unsigned(address),
            opened ? " (protection opened)" : "");
        return Object({{"written", std::to_string(bytes.size())}});
    }

    // The bytes in committed readable memory of [start, end), each region searched whole.
    std::string Find(const Request& request)
    {
        const std::vector<uint8_t> pattern = FromHex(request.Get("hex"));
        if (pattern.empty())
            throw std::runtime_error("an empty pattern");
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const uintptr_t start = uintptr_t(request.Number("start", uintptr_t(system.lpMinimumApplicationAddress)));
        const uintptr_t end = uintptr_t(request.Number("end", uintptr_t(system.lpMaximumApplicationAddress)));
        const size_t limit = std::min<size_t>(size_t(request.Number("max_results", 100)), kMaxMatches);
        std::vector<uintptr_t> found(limit);
        size_t count = 0;
        for (uintptr_t at = start; at < end && count < limit;)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)))
                break;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (info.State == MEM_COMMIT && Readable(info.Protect))
            {
                const auto* first = reinterpret_cast<const uint8_t*>(std::max(at, uintptr_t(info.BaseAddress)));
                const auto* last = reinterpret_cast<const uint8_t*>(std::min(regionEnd, end));
                SafeSearch(first, last, pattern.data(), pattern.size(), found.data(), limit, &count);
            }
            if (regionEnd <= at)
                break;
            at = regionEnd;
        }
        std::vector<std::string> matches;
        for (size_t i = 0; i < count; ++i)
            matches.push_back(Address(found[i]));
        return Object({{"matches", Array(matches)}, {"truncated", count >= limit ? "true" : "false"}});
    }

    // A signature as the hooks write them ("8B 0D ? ? ? ? 85 C9"), in a module (the game's exe unless named).
    std::string Pattern(const Request& request)
    {
        const std::string module = request.Get("module", "");
        HMODULE handle = module.empty() ? GetModuleHandleW(nullptr) : GetModuleHandleA(module.c_str());
        if (!handle)
            throw std::runtime_error("no module " + module);
        const std::string signature = request.Get("pattern");
        auto pattern = hook::make_module_pattern(handle, signature);
        const size_t limit = std::min<size_t>(size_t(request.Number("max_results", 100)), kMaxMatches);
        std::vector<std::string> matches;
        for (size_t i = 0; i < pattern.size() && i < limit; ++i)
            matches.push_back(Address(reinterpret_cast<uintptr_t>(pattern.get(i).get<void>())));
        return Object({{"matches", Array(matches)}, {"total", std::to_string(pattern.size())}});
    }

    std::string IniGet(const Request& request)
    {
        CIniReader ini("");
        const std::string section = request.Get("section"), key = request.Get("key");
        return Object({{"section", Quote(section)}, {"key", Quote(key)}, {"value", Quote(ini.ReadString(section, key, ""))}});
    }

    // Writes the value to the ini and has it read again by the next frame, as Ctrl+Shift+F10 would.
    std::string IniSet(const Request& request)
    {
        CIniReader ini("");
        const std::string section = request.Get("section"), key = request.Get("key"), value = request.Get("value");
        ini.WriteString(section, key, value, true);
        IniReload::Request();
        FusionLog::Write("DebugServer", "Ini", "[%s] %s = %s\n", section.c_str(), key.c_str(), value.c_str());
        return Object({{"section", Quote(section)}, {"key", Quote(key)}, {"value", Quote(ini.ReadString(section, key, ""))},
            {"note", Quote("read again at the next frame; what is set up at startup needs a restart")}});
    }

    // The last lines of a feature's log ("feature": "NightShadows"), or of the newest log.
    std::string LogTail(const Request& request)
    {
        const size_t wanted = std::min<size_t>(size_t(request.Number("lines", 200)), kMaxLogLines);
        std::filesystem::path path;
        if (request.Has("feature"))
            path = FusionLog::PathFor(request.Get("feature"));
        else
        {
            std::filesystem::file_time_type newest{};
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(FusionLog::Directory(), error))
            {
                const auto name = entry.path().filename().string();
                if (entry.path().extension() != ".log" || !name.starts_with("GTAIV.EFLC.FusionFix."))
                    continue;
                const auto time = entry.last_write_time(error);
                if (!error && (path.empty() || time > newest))
                {
                    path = entry.path();
                    newest = time;
                }
            }
        }
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (path.empty() || !file)
            throw std::runtime_error("no log " + path.string());
        const std::streamoff size = file.tellg();
        const std::streamoff from = std::max<std::streamoff>(0, size - (8 << 20));
        file.seekg(from);
        std::string text(size_t(size - from), '\0');
        file.read(text.data(), std::streamsize(text.size()));
        std::vector<std::string> lines;
        for (size_t begin = 0; begin < text.size();)
        {
            size_t newline = text.find('\n', begin);
            if (newline == std::string::npos)
                newline = text.size();
            std::string line = text.substr(begin, newline - begin);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(Quote(line));
            begin = newline + 1;
        }
        if (lines.size() > wanted)
            lines.erase(lines.begin(), lines.end() - wanted);
        return Object({{"file", Quote(path.string())}, {"lines", Array(lines)}});
    }

    std::string Handle(const Request& request)
    {
        const std::string command = request.Get("cmd");
        if (command == "ping") return Object({{"pong", "true"}});
        if (command == "info") return Info();
        if (command == "modules") return Modules();
        if (command == "read") return Read(request);
        if (command == "write") return Write(request);
        if (command == "find") return Find(request);
        if (command == "pattern") return Pattern(request);
        if (command == "ini_get") return IniGet(request);
        if (command == "ini_set") return IniSet(request);
        if (command == "log_tail") return LogTail(request);
        if (command == "screenshot") return Screenshot(request);
        throw std::runtime_error("unknown command '" + command + "'");
    }

    // The answer with "ok" and the request's "id" put in front of its fields.
    std::string Answer(const std::string& line)
    {
        std::string id = "null";
        try
        {
            const Request request = Parse(line);
            id = request.idJson;
            std::string body = Handle(request);
            return "{\"ok\":true,\"id\":" + id + (body.size() > 2 ? "," + body.substr(1) : "}");
        }
        catch (const std::exception& exception)
        {
            return "{\"ok\":false,\"id\":" + id + ",\"error\":" + Quote(exception.what()) + "}";
        }
    }

    void Serve(SOCKET client)
    {
        std::string pending;
        std::vector<char> buffer(64 << 10);
        for (;;)
        {
            const int received = recv(client, buffer.data(), int(buffer.size()), 0);
            if (received <= 0)
                return;
            pending.append(buffer.data(), size_t(received));
            if (pending.size() > kMaxRequestBytes)
                return;
            for (size_t newline; (newline = pending.find('\n')) != std::string::npos;)
            {
                std::string reply = Answer(pending.substr(0, newline)) + "\n";
                pending.erase(0, newline + 1);
                for (size_t sent = 0; sent < reply.size();)
                {
                    const int result = send(client, reply.data() + sent, int(std::min<size_t>(reply.size() - sent, 1 << 20)), 0);
                    if (result <= 0)
                        return;
                    sent += size_t(result);
                }
            }
        }
    }

    void Start(int port)
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            FusionLog::Write("DebugServer", "", "WSAStartup failed\n");
            return;
        }
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(uint16_t(port));
        // Loopback only: the tool reaches it through an SSH tunnel, never the network.
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, 1) != 0)
        {
            FusionLog::Write("DebugServer", "", "cannot listen on 127.0.0.1:%d\n", port);
            if (listener != INVALID_SOCKET)
                closesocket(listener);
            return;
        }
        FusionLog::Write("DebugServer", "", "listening on 127.0.0.1:%d (writes %s)\n", port, allowWrite ? "on" : "off");
        std::thread([listener]
        {
            for (;;)
            {
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    continue;
                FusionLog::Write("DebugServer", "", "client connected\n");
                Serve(client);
                closesocket(client);
                FusionLog::Write("DebugServer", "", "client disconnected\n");
            }
        }).detach();
    }
}

class DebugServerModule
{
public:
    DebugServerModule()
    {
        FusionFix::onInitEvent() += []()
        {
            CIniReader iniReader("");
            const int port = iniReader.ReadInteger("DEBUG", "DebugServerPort", 0);
            DebugServer::allowWrite = iniReader.ReadInteger("DEBUG", "DebugServerAllowWrite", 0) != 0;
            if (port > 0 && port < 65536)
            {
                FusionFix::onBeforePresent() += []() { DebugServer::TakeScreenshot(); };
                DebugServer::Start(port);
            }
        };
    }
} DebugServerModule;
