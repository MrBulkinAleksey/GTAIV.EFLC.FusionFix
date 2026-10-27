module;

#include <common.hxx>
#include <Zydis.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3dx9.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <asi_settings/asi_settings.h>
#include "FusionLog.hpp"

#pragma comment(lib, "ws2_32.lib")

export module debugserver;

import common;
import comvars;
import d3dx9_43;
import framecapture;
import inireload;
import natives;
import postfxprofiler;
import profiler;
import settings;

// [DEBUG] DebugServerPort: a server on 127.0.0.1 that a tool (tools/gta4-live-mcp in the recomp's repository) reaches
// through an SSH tunnel, to look into the running game from another machine: memory (read, search, signatures, and
// writes with DebugServerAllowWrite), the loaded modules, the ini (read, write and read again as Ctrl+Shift+F10 does)
// and the logs (and FusionFix's other files next to it: files, file), and over time: values read each frame (watch_start) and the hits of an instruction or a piece of
// memory (trace_start: a mid-hook or a debug register, with registers, callers and reads; trace_read, trace_stop,
// trace_list), the GPU time of FusionFix's passes and the game's (profiler: PostFxProfiler's next block), and where the
// threads spend their CPU time (cpu_profile: a session of the sampling profiler, Profiler), and what a whole scene frame
// draws (frame_capture: Frame Capture with the request's options). One client at a time; a request is a JSON object on one line ({"id": 1, "cmd": "read", ...}), the
// answer one line ({"id": 1, "ok": true, ...}), the same protocol as the recomp's debug server. Bytes travel as hex in
// memory order (little-endian here), addresses as "0x..." strings.
namespace DebugServer
{
    constexpr size_t kMaxReadBytes = 1u << 20;
    constexpr size_t kMaxRequestBytes = 4u << 20;
    constexpr size_t kMaxMatches = 1000;
    constexpr size_t kMaxLogLines = 5000;

    static bool allowWrite = false;

    // Decimal, or hex with "0x" (not octal for a leading 0); a sign, a fraction or anything after the digits fails.
    uint64_t ParseWhole(const std::string& text, const std::string& what)
    {
        const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        const std::string digits = hex ? text.substr(2) : text;
        size_t used = 0;
        uint64_t value = 0;
        try
        {
            value = std::stoull(digits, &used, hex ? 16 : 10);
        }
        catch (const std::exception&)
        {
            used = 0;
        }
        if (digits.empty() || !isxdigit(uint8_t(digits[0])) || used != digits.size())
            throw std::runtime_error("\"" + what + "\" is not a whole number: " + text);
        return value;
    }

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
        uint64_t Number(const std::string& key) const { return ParseWhole(Get(key), key); }
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

    // The length of the well-formed UTF-8 sequence at text[at] (a lead byte >= 0x80), 0 if it is not one.
    size_t Utf8Length(std::string_view text, size_t at)
    {
        const uint8_t lead = uint8_t(text[at]);
        const size_t length = lead >= 0xF0 && lead <= 0xF4 ? 4 : lead >= 0xE0 && lead < 0xF0 ? 3 : lead >= 0xC2 && lead < 0xE0 ? 2 : 0;
        if (!length || at + length > text.size())
            return 0;
        for (size_t i = 1; i < length; ++i)
            if ((uint8_t(text[at + i]) & 0xC0) != 0x80)
                return 0;
        return length;
    }

    std::string Quote(std::string_view text)
    {
        std::string out = "\"";
        for (size_t at = 0; at < text.size(); ++at)
        {
            const char c = text[at];
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
                {
                    // UTF-8 as it is; a byte of another code page (a log may hold one) as '?', for JSON wants UTF-8
                    const size_t length = Utf8Length(text, at);
                    if (length)
                    {
                        out.append(text.substr(at, length));
                        at += length - 1;
                    }
                    else
                        out += '?';
                }
                else
                    out += c;
            }
        }
        return out + "\"";
    }

    // A path as UTF-8 (path::string() goes through the ANSI code page and throws on what it cannot hold).
    std::string Utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
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
        uint64_t asked = 0;     // the request's number, for an answer taken for one that timed out not to go to the next
        uint64_t answered = 0;
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
        uint64_t number = 0;
        uint32_t maxWidth = 0;
        {
            std::lock_guard lock(capture.mutex);
            number = capture.asked;
            maxWidth = capture.maxWidth;
        }
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
            if (maxWidth && width > maxWidth)
            {
                height = std::max<UINT>(1, UINT(uint64_t(height) * maxWidth / width));
                width = maxWidth;
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
            if (number != capture.asked)
                return; // (the request timed out and another came meanwhile: this frame is not for it)
            capture.png = std::move(png);
            capture.width = width;
            capture.height = height;
            capture.error = std::move(error);
            capture.answered = number;
            capture.pending = false;
        }
        capture.done.notify_all();
    }

    std::string Screenshot(const Request& request)
    {
        std::unique_lock lock(capture.mutex);
        const uint64_t number = ++capture.asked;
        capture.maxWidth = uint32_t(request.Number("max_width", 1920));
        capture.pending = true;
        // (the game presents in its menus and while paused too; a minimized game does not)
        if (!capture.done.wait_for(lock, std::chrono::seconds(10), [number] { return capture.answered == number; }))
        {
            capture.pending = false;
            throw std::runtime_error("no frame was presented within 10 s (is the game minimized?)");
        }
        if (!capture.error.empty())
            throw std::runtime_error(capture.error);
        const auto path = FusionLog::PathFor("Screenshot", L".png");
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(capture.png.data()), std::streamsize(capture.png.size()));
        return Object({{"mime", Quote("image/png")}, {"width", std::to_string(capture.width)},
            {"height", std::to_string(capture.height)}, {"file", Quote(Utf8(path))},
            {"data", Quote(Base64(capture.png.data(), capture.png.size()))}});
    }

    // ---- frame capture (framecapture.ixx): the next whole scene frame with the request's options, answered once written

    std::string FrameCaptureRequest(const Request& request)
    {
        FrameCapture::Options options;
        for (const char* key : { "kinds", "model", "ps", "vs", "target", "lines", "pictures", "draw_pictures", "constants", "stretch_pictures", "probes", "raw", "raw_ps" })
            if (request.Has(key))
                FrameCapture::SetOption(options, key, request.Get(key));
        const int seconds = std::clamp(int(request.Number("timeout", 30)), 1, 120);
        const uint32_t number = FrameCapture::Arm(options);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (FrameCapture::Done() < number)
        {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("no scene frame was captured within " + std::to_string(seconds) + " s (is the game minimized or in a menu?)");
            Sleep(20);
        }
        std::vector<std::string> pictures;
        for (const auto& name : FrameCapture::Pictures())
            pictures.push_back(Quote(name));
        return Object({{"capture", std::to_string(number)}, {"log", Quote(Utf8(FusionLog::PathFor("FrameCapture")))},
            {"summary", Quote(FrameCapture::Summary())}, {"pictures", Array(pictures)}});
    }

    // ---- the GPU profiler (PostFxProfiler): on for the request, a block started afresh, its rows when it is written

    std::string GpuProfiler(const Request& request)
    {
        const int seconds = std::clamp(int(request.Number("timeout", 30)), 1, 120);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        struct Waiting
        {
            Waiting() { PostFxProfiler::bRemoteReset = true; ++PostFxProfiler::nRemoteRequests; }
            ~Waiting() { --PostFxProfiler::nRemoteRequests; }
        } waiting;
        const auto wait = [&](auto done, const char* what)
        {
            while (!done())
            {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error(std::string("no profiler block within ") + std::to_string(seconds) + " s: " + what);
                Sleep(20);
            }
        };
        wait([] { return !PostFxProfiler::bRemoteReset; }, "no frame reached post processing (minimized, loading?)");
        const uint32_t started = PostFxProfiler::BlocksWritten();
        wait([started] { return PostFxProfiler::BlocksWritten() > started; }, "the frames did not come back (no timestamp queries on this device?)");
        std::vector<PostFxProfiler::ProfilerRow> rows;
        int frames = 0;
        PostFxProfiler::LastBlock(rows, frames);
        std::vector<std::string> items;
        for (const auto& row : rows)
        {
            char ms[32];
            snprintf(ms, sizeof(ms), "%.3f", row.ms);
            items.push_back(Object({{"name", Quote(row.name)}, {"depth", std::to_string(row.depth)}, {"ms", ms}}));
        }
        return Object({{"frames", std::to_string(frames)}, {"rows", Array(items)}});
    }

    // ---- the sampling profiler (Profiler): a session of some seconds, its report as text

    std::string CpuProfile(const Request& request)
    {
        Profiler::SessionSettings settings;
        const auto number = [&](const char* key, int fallback, int low, int high)
        {
            return std::clamp(int((std::min)(request.Number(key, uint64_t(fallback)), uint64_t(high))), low, high);
        };
        settings.seconds = number("seconds", settings.seconds, 1, 300);
        settings.rate = number("rate", settings.rate, 50, 4000);
        settings.threads = number("threads", settings.threads, 1, 64);
        settings.waiting = request.Number("waiting", 0) != 0;
        settings.spikeMs = number("spike_ms", settings.spikeMs, 0, 2000);
        settings.functions = number("functions", settings.functions, 5, 500);
        settings.stacks = number("stacks", settings.stacks, 0, 1000);
        settings.spikes = number("spikes", settings.spikes, 0, 100);
        const std::string report = Profiler::Session(settings);
        return Object({{"seconds", std::to_string(settings.seconds)}, {"rate", std::to_string(settings.rate)},
            {"threads", std::to_string(settings.threads)}, {"folded", Quote(Utf8(FusionLog::PathFor("Profiler", L".folded")))},
            {"report", Quote(report)}});
    }

    // ---- the scene: script natives, which run only in the game's own frame, so a request waits for the next one

    struct MainThreadCall
    {
        std::function<std::string()> work;
        std::promise<std::string> result;
        std::atomic<bool> cancelled{false};
    };
    static std::mutex mainThreadMutex;
    static std::deque<std::shared_ptr<MainThreadCall>> mainThreadCalls;

    // From onGameProcessEvent, which runs only while the game itself runs (not paused, not in a menu).
    void RunMainThreadCalls()
    {
        std::deque<std::shared_ptr<MainThreadCall>> calls;
        {
            std::lock_guard lock(mainThreadMutex);
            calls.swap(mainThreadCalls);
        }
        for (auto& call : calls)
        {
            if (call->cancelled)
                continue;
            try
            {
                call->result.set_value(call->work());
            }
            catch (...)
            {
                call->result.set_exception(std::current_exception());
            }
        }
    }

    std::string OnMainThread(std::function<std::string()> work, int seconds = 5)
    {
        auto call = std::make_shared<MainThreadCall>();
        call->work = std::move(work);
        auto result = call->result.get_future();
        {
            std::lock_guard lock(mainThreadMutex);
            mainThreadCalls.push_back(call);
        }
        if (result.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready)
        {
            call->cancelled = true; // (dropped, not done late, if it has not begun)
            throw std::runtime_error("the game ran no frame within " + std::to_string(seconds) +
                " s: it is paused, in a menu or loading (or the call is still running)");
        }
        return result.get();
    }

    std::string Float(float value)
    {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.3f", value);
        return buffer;
    }

    Ped PlayerPed()
    {
        Ped ped = 0;
        Natives::GetPlayerChar(Natives::ConvertIntToPlayerIndex(Natives::GetPlayerId()), &ped);
        if (!ped)
            throw std::runtime_error("no player character (not in the game yet?)");
        return ped;
    }

    // Where the player and the game camera are, the time and the weather.
    std::string Scene()
    {
        return OnMainThread([]
        {
            const Ped ped = PlayerPed();
            float x = 0, y = 0, z = 0, heading = 0, camX = 0, camY = 0, camZ = 0, rotX = 0, rotY = 0, rotZ = 0;
            Natives::GetCharCoordinates(ped, &x, &y, &z);
            Natives::GetCharHeading(ped, &heading);
            const bool inCar = Natives::IsCharInAnyCar(ped);
            Camera camera = 0;
            Natives::GetGameCam(&camera);
            if (camera)
            {
                Natives::GetCamPos(camera, &camX, &camY, &camZ);
                Natives::GetCamRot(camera, &rotX, &rotY, &rotZ);
            }
            uint32_t hour = 0, minute = 0, weather = 0;
            Natives::GetTimeOfDay(&hour, &minute);
            Natives::GetCurrentWeather(&weather);
            return Object({
                {"player", Array({Float(x), Float(y), Float(z)})}, {"heading", Float(heading)},
                {"in_car", inCar ? "true" : "false"},
                {"camera", Array({Float(camX), Float(camY), Float(camZ)})},
                {"camera_rotation", Array({Float(rotX), Float(rotY), Float(rotZ)})},
                {"time", Quote(std::to_string(hour) + ":" + (minute < 10 ? "0" : "") + std::to_string(minute))},
                {"weather", std::to_string(weather)},
            });
        });
    }

    // The player (and the car they are in) to x, y, z, the scene and its collision loaded first.
    std::string Teleport(const Request& request)
    {
        const float x = std::stof(request.Get("x")), y = std::stof(request.Get("y")), z = std::stof(request.Get("z"));
        const bool turn = request.Has("heading");
        const float heading = turn ? std::stof(request.Get("heading")) : 0.0f;
        return OnMainThread([=]
        {
            const Ped ped = PlayerPed();
            Natives::RequestCollisionAtPosn(x, y, z);
            Natives::LoadScene(x, y, z);
            if (Natives::IsCharInAnyCar(ped))
            {
                Vehicle car = 0;
                Natives::GetCarCharIsUsing(ped, &car);
                Natives::SetCarCoordinates(car, x, y, z);
                if (turn)
                    Natives::SetCarHeading(car, heading);
            }
            else
            {
                // (z as scene gives it, the character's centre: SetCharCoordinates puts the feet there, 1 m lower)
                Natives::SetCharCoordinatesNoOffset(ped, x, y, z);
                if (turn)
                    Natives::SetCharHeading(ped, heading);
            }
            FusionLog::Write("DebugServer", "Scene", "teleport to %.1f %.1f %.1f\n", x, y, z);
            return Object({{"player", Array({Float(x), Float(y), Float(z)})}});
        }, 30); // (LoadScene streams the place in before it returns, which can take seconds)
    }

    // Freeze's script camera and whether it stopped the clock (SetTime keeps it stopped).
    static Camera frozenCamera = 0;
    static bool clockStopped = false;

    std::string SetTime(const Request& request)
    {
        const uint32_t hour = uint32_t(request.Number("hour")) % 24, minute = uint32_t(request.Number("minute", 0)) % 60;
        return OnMainThread([=]
        {
            // (while frozen the clock stays stopped at the new time)
            if (clockStopped)
                Natives::ForceTimeOfDay(hour, minute);
            else
                Natives::SetTimeOfDay(hour, minute);
            return Object({{"time", Quote(std::to_string(hour) + ":" + (minute < 10 ? "0" : "") + std::to_string(minute))}});
        });
    }

    // How fast the clock runs: real milliseconds a game minute (CE 0x1295830, the game's 2000), "ms_per_minute"
    // 1..600000 or "reset"; without either only reads it. CClock::Update (CE 0x9CB791) adds minutes until it has caught
    // up with the time (0x1173608), however many a frame, so a small value runs a day in seconds. The minute's start
    // (0x129583C) is moved to now, or the minutes since the last one would be run at the new rate at once. Saves keep
    // the rate (CE 0x8C7CB6): reset before saving.
    std::string ClockRate(const Request& request)
    {
        const bool reset = request.Get("reset", "false") != "false";
        const bool set = reset || request.Has("ms_per_minute");
        const uint32_t rate = reset ? 2000u : uint32_t(request.Number("ms_per_minute", 2000));
        if (set && (rate < 1 || rate > 600000))
            throw std::runtime_error("ms_per_minute is 1..600000");
        return OnMainThread([=]
        {
            auto& perMinute = *reinterpret_cast<uint32_t*>(GameBase() + 0xE95830);
            if (set)
            {
                *reinterpret_cast<uint32_t*>(GameBase() + 0xE9583C) = *reinterpret_cast<const uint32_t*>(GameBase() + 0xD73608);
                perMinute = rate;
                FusionLog::Write("DebugServer", "Scene", "clock %u ms a game minute\n", rate);
            }
            uint32_t hour = 0, minute = 0;
            Natives::GetTimeOfDay(&hour, &minute);
            return Object({{"ms_per_minute", std::to_string(perMinute)},
                {"time", Quote(std::to_string(hour) + ":" + (minute < 10 ? "0" : "") + std::to_string(minute))},
                {"clock_stopped", clockStopped ? "true" : "false"}});
        });
    }

    // A weather id (the game's eWeather: 0 extra sunny .. 9 sunny windy), or "release" for the game's own.
    std::string SetWeather(const Request& request)
    {
        const std::string weather = request.Get("weather");
        return OnMainThread([=]
        {
            if (weather == "release")
                Natives::ReleaseWeather();
            else
                Natives::ForceWeatherNow(uint32_t(std::stoul(weather)));
            return Object({{"weather", Quote(weather)}});
        });
    }

    // A still scene for comparing frames: a script camera where the game camera is (or at x, y, z turned rx, ry, rz
    // with fov, as given), the player frozen and without control, the clock stopped. "on": false gives all back.

    std::string Freeze(const Request& request)
    {
        const bool on = request.Get("on", "true") != "false";
        const bool placed = request.Has("x");
        float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0, fov = 0;
        if (placed)
        {
            x = std::stof(request.Get("x")), y = std::stof(request.Get("y")), z = std::stof(request.Get("z"));
            rx = std::stof(request.Get("rx", "0")), ry = std::stof(request.Get("ry", "0")), rz = std::stof(request.Get("rz", "0"));
        }
        if (request.Has("fov"))
            fov = std::stof(request.Get("fov"));
        return OnMainThread([=]() mutable
        {
            const Ped ped = PlayerPed();
            const auto player = Natives::ConvertIntToPlayerIndex(Natives::GetPlayerId());
            if (!on)
            {
                if (frozenCamera)
                {
                    Natives::SetCamActive(frozenCamera, false);
                    Natives::SetCamPropagate(frozenCamera, false);
                    Natives::ActivateScriptedCams(false, false);
                    Natives::DestroyCam(frozenCamera);
                    frozenCamera = 0;
                }
                Natives::FreezeCharPosition(ped, false);
                Natives::SetPlayerControl(player, true);
                if (clockStopped)
                    Natives::ReleaseTimeOfDay();
                clockStopped = false;
                FusionLog::Write("DebugServer", "Scene", "freeze off\n");
                return Object({{"frozen", "false"}});
            }
            if (!placed || !fov)
            {
                Camera game = 0;
                Natives::GetGameCam(&game);
                if (!game)
                    throw std::runtime_error("no game camera");
                if (!placed)
                {
                    Natives::GetCamPos(game, &x, &y, &z);
                    Natives::GetCamRot(game, &rx, &ry, &rz);
                }
                if (!fov)
                    Natives::GetCamFov(game, &fov);
            }
            if (!frozenCamera)
                Natives::CreateCam(14, &frozenCamera);
            if (!frozenCamera)
                throw std::runtime_error("CreateCam failed");
            Natives::SetCamPos(frozenCamera, x, y, z);
            Natives::SetCamRot(frozenCamera, rx, ry, rz);
            Natives::SetCamFov(frozenCamera, fov);
            Natives::SetCamActive(frozenCamera, true);
            Natives::SetCamPropagate(frozenCamera, true);
            Natives::ActivateScriptedCams(true, true);
            Natives::FreezeCharPosition(ped, true);
            Natives::SetPlayerControl(player, false);
            uint32_t hour = 0, minute = 0;
            Natives::GetTimeOfDay(&hour, &minute);
            Natives::ForceTimeOfDay(hour, minute);
            clockStopped = true;
            FusionLog::Write("DebugServer", "Scene", "freeze at %.2f %.2f %.2f rot %.2f %.2f %.2f fov %.1f\n", x, y, z, rx, ry, rz, fov);
            return Object({{"frozen", "true"}, {"camera", Array({Float(x), Float(y), Float(z)})},
                {"camera_rotation", Array({Float(rx), Float(ry), Float(rz)})}, {"fov", Float(fov)}});
        });
    }

    std::string Info()
    {
        HMODULE game = GetModuleHandleW(nullptr);
        HMODULE plugin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Info), &plugin);
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(game, exe, MAX_PATH);
        return Object({
            {"title", Quote("GTA IV (PC, FusionFix)")},
            {"exe", Quote(Utf8(std::filesystem::path(exe).filename()))},
            {"exe_base", Address(reinterpret_cast<uintptr_t>(game))},
            {"plugin_base", Address(reinterpret_cast<uintptr_t>(plugin))},
            {"pid", std::to_string(GetCurrentProcessId())},
            {"pointer_size", std::to_string(sizeof(void*))},
            {"endianness", Quote("little")},
            {"allow_write", allowWrite ? "true" : "false"},
            {"ini", Quote(Utf8(SettingsPlugin().CfgPath()))},
            {"logs_dir", Quote(Utf8(FusionLog::Directory()))},
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

    // The address space by allocation (VirtualAlloc/MapViewOfFile/image base): what holds it, what is free and in
    // how large blocks; a 32-bit game runs short of addresses well before memory.
    std::string VirtualMemoryMap(const Request& request)
    {
        const uint64_t minSize = request.Number("min_size", 1024 * 1024);
        const size_t top = size_t(std::clamp<uint64_t>(request.Number("top", 60), 1, 2000));

        struct Allocation
        {
            uintptr_t base = 0;
            uint64_t size = 0, committed = 0;
            DWORD type = 0, protect = 0;
            bool guard = false;
            int regions = 0;
        };
        std::vector<Allocation> allocations;
        std::vector<std::pair<uintptr_t, uint64_t>> freeBlocks;

        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const uintptr_t end = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
        for (uintptr_t at = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress); at < end;)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) || !info.RegionSize)
                break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (info.State == MEM_FREE)
                freeBlocks.emplace_back(base, info.RegionSize);
            else
            {
                const uintptr_t owner = reinterpret_cast<uintptr_t>(info.AllocationBase);
                if (allocations.empty() || allocations.back().base != owner)
                    allocations.push_back({owner, 0, 0, info.Type, 0, false, 0});
                auto& a = allocations.back();
                a.size += info.RegionSize;
                a.regions++;
                if (info.State == MEM_COMMIT)
                {
                    a.committed += info.RegionSize;
                    if (!a.protect)
                        a.protect = info.Protect;
                    if (info.Protect & PAGE_GUARD)
                        a.guard = true;
                }
            }
            if (base + info.RegionSize <= at)
                break;
            at = base + info.RegionSize;
        }

        std::vector<HANDLE> heaps(256);
        heaps.resize(GetProcessHeaps(DWORD(heaps.size()), heaps.data()));

        auto kind = [&](const Allocation& a) -> const char* {
            if (a.type == MEM_IMAGE) return "image";
            if (a.type == MEM_MAPPED) return "mapped";
            if (std::find(heaps.begin(), heaps.end(), reinterpret_cast<HANDLE>(a.base)) != heaps.end()) return "heap";
            if (a.guard) return "stack";
            return "private";
        };
        auto owner = [&](const Allocation& a) -> std::string {
            wchar_t path[MAX_PATH] = {};
            if (a.type == MEM_IMAGE && GetModuleFileNameW(reinterpret_cast<HMODULE>(a.base), path, MAX_PATH))
                return Utf8(std::filesystem::path(path).filename());
            if (a.type == MEM_MAPPED && K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path, MAX_PATH))
                return Utf8(std::filesystem::path(path).filename());
            return {};
        };

        struct Totals { uint64_t count = 0, reserved = 0, committed = 0, smallCount = 0, smallReserved = 0; };
        std::map<std::string, Totals> byKind;
        for (const auto& a : allocations)
        {
            auto& t = byKind[kind(a)];
            t.count++;
            t.reserved += a.size;
            t.committed += a.committed;
            if (a.size < minSize)
            {
                t.smallCount++;
                t.smallReserved += a.size;
            }
        }
        std::vector<std::string> kinds;
        for (const auto& [name, t] : byKind)
            kinds.push_back(Object({{"kind", Quote(name)}, {"count", std::to_string(t.count)},
                {"reserved", std::to_string(t.reserved)}, {"committed", std::to_string(t.committed)},
                {"small_count", std::to_string(t.smallCount)}, {"small_reserved", std::to_string(t.smallReserved)}}));

        std::sort(allocations.begin(), allocations.end(), [](const auto& l, const auto& r) { return l.size > r.size; });
        std::vector<std::string> listed;
        for (const auto& a : allocations)
        {
            if (a.size < minSize || listed.size() >= top)
                break;
            char protect[16];
            snprintf(protect, sizeof(protect), "0x%X", unsigned(a.protect));
            listed.push_back(Object({{"base", Address(a.base)}, {"size", std::to_string(a.size)},
                {"committed", std::to_string(a.committed)}, {"kind", Quote(kind(a))}, {"owner", Quote(owner(a))},
                {"protect", Quote(protect)}, {"regions", std::to_string(a.regions)}}));
        }

        uint64_t freeTotal = 0, usableFree = 0; // usable: blocks of the allocation granularity (64 KB) or more
        size_t usableBlocks = 0;
        for (const auto& block : freeBlocks)
        {
            freeTotal += block.second;
            if (block.second >= system.dwAllocationGranularity)
            {
                usableFree += block.second;
                usableBlocks++;
            }
        }
        std::sort(freeBlocks.begin(), freeBlocks.end(), [](const auto& l, const auto& r) { return l.second > r.second; });
        std::vector<std::string> largestFree;
        for (size_t i = 0; i < freeBlocks.size() && i < 16; ++i)
            largestFree.push_back(Object({{"base", Address(freeBlocks[i].first)}, {"size", std::to_string(freeBlocks[i].second)}}));

        return Object({{"max_address", Address(end)}, {"allocations", std::to_string(allocations.size())},
            {"free_total", std::to_string(freeTotal)}, {"free_blocks", std::to_string(freeBlocks.size())},
            {"free_usable", std::to_string(usableFree)}, {"free_usable_blocks", std::to_string(usableBlocks)},
            {"largest_free", Array(largestFree)}, {"kinds", Array(kinds)}, {"listed", Array(listed)}});
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
            // (not uncached or write-combined memory, the GPU's buffers mapped: reading it is many times slower)
            if (info.State == MEM_COMMIT && Readable(info.Protect) && !(info.Protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE)))
            {
                const auto* first = reinterpret_cast<const uint8_t*>(std::max(at, uintptr_t(info.BaseAddress)));
                const auto* last = reinterpret_cast<const uint8_t*>(std::min(regionEnd, end));
                SafeSearch(first, last, pattern.data(), pattern.size(), found.data(), limit, &count);
            }
            if (regionEnd <= at)
                break;
            at = regionEnd;
        }
        // (not the pattern itself, as this request decoded it)
        const auto own = reinterpret_cast<uintptr_t>(pattern.data());
        std::vector<std::string> matches;
        for (size_t i = 0; i < count; ++i)
            if (found[i] != own)
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

    // A setting of the description (SECTION.Key): its value in effect, empty for a key the description lacks
    std::string IniGet(const Request& request)
    {
        const std::string section = request.Get("section"), key = request.Get("key");
        return Object({{"section", Quote(section)}, {"key", Quote(key)}, {"value", Quote(SettingsPlugin().Get(section + "." + key))}});
    }

    // Sets the value as the menu would: stored in the cfg, and read again by the next frame as after Ctrl+Shift+F10.
    std::string IniSet(const Request& request)
    {
        const std::string section = request.Get("section"), key = request.Get("key"), value = request.Get("value");
        const auto id = section + "." + key;
        const auto result = SettingsPlugin().Set(id, value, true);
        if (result == ASI_SETTINGS_UNKNOWN_ID)
            throw std::runtime_error("no setting " + id + " in the description (settings/GTAIV.EFLC.FusionFix.json)");
        if (result != ASI_SETTINGS_OK)
            throw std::runtime_error("the value " + value + " of " + id + " is refused (" + std::to_string(result) + ")");
        IniReload::Request();
        FusionLog::Write("DebugServer", "Ini", "[%s] %s = %s\n", section.c_str(), key.c_str(), value.c_str());
        return Object({{"section", Quote(section)}, {"key", Quote(key)}, {"value", Quote(SettingsPlugin().Get(id))},
            {"note", Quote("read again at the next frame; what is set up at startup needs a restart")}});
    }

    // The last lines of a feature's log ("feature": "NightShadows"), or of the newest log.
    // FusionFix's own files next to the plugin (GTAIV.EFLC.FusionFix.*: logs, pictures, .folded): the names matching
    // "filter" with their sizes, newest first; or one of them whole as base64 (a bare name, at most kMaxFileBytes)
    constexpr uintmax_t kMaxFileBytes = 64u << 20;   // a frame capture's 1920x1080 half float target is 16.6 MB

    std::string Files(const Request& request)
    {
        const std::string filter = request.Get("filter", "");
        struct Entry { std::filesystem::file_time_type time; std::string json; };
        std::vector<Entry> found;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(FusionLog::Directory(), error))
        {
            const std::string name = Utf8(entry.path().filename());
            if (!name.starts_with("GTAIV.EFLC.FusionFix.") || name.find(filter) == std::string::npos || !entry.is_regular_file(error))
                continue;
            found.push_back({ entry.last_write_time(error), Object({{"name", Quote(name)}, {"size", std::to_string(entry.file_size(error))}}) });
        }
        std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.time > b.time; });
        std::vector<std::string> items;
        for (const auto& e : found)
            items.push_back(e.json);
        return Object({{"files", Array(items)}});
    }

    std::string File(const Request& request)
    {
        const std::string name = request.Get("name");
        const bool bare = !name.empty() && name.find_first_of("/\\:") == std::string::npos && name.find("..") == std::string::npos &&
                          std::all_of(name.begin(), name.end(), [](char c) { return uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F; });
        if (!bare || !name.starts_with("GTAIV.EFLC.FusionFix."))
            throw std::runtime_error("only a bare name of FusionFix's own files (GTAIV.EFLC.FusionFix.*) next to the plugin");
        const auto path = FusionLog::Directory() / std::filesystem::path(name);
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            throw std::runtime_error("no file " + name);
        if (size > kMaxFileBytes)
            throw std::runtime_error(name + " is over 64 MB");
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
            throw std::runtime_error("cannot read " + name);
        return Object({{"name", Quote(name)}, {"size", std::to_string(size)}, {"data", Quote(Base64(bytes.data(), bytes.size()))}});
    }

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
                const auto name = entry.path().filename().wstring();
                if (entry.path().extension() != ".log" || !name.starts_with(L"GTAIV.EFLC.FusionFix."))
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
            throw std::runtime_error("no log " + Utf8(path));
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
        return Object({{"file", Quote(Utf8(path))}, {"lines", Array(lines)}});
    }

    // ---- over time: values sampled each frame (watch), the hits of a place in code or memory (trace) ---------------
    //
    // A place to read is a spec "base[,offset...]:size": base a register (eax ecx edx ebx esp ebp esi edi, in a trace)
    // or a number. With no offsets it is the base's own value (at most 4 bytes); else the bytes at base + the first
    // offset, each further offset reading a pointer there first and adding to that ("0x15E8D30,0x10,0x24:4": 4 bytes at
    // [0x15E8D30 + 0x10] + 0x24; "ecx,0x24:16": 16 bytes at ecx + 0x24). Specs are joined with ';'.
    //
    // A trace is a mid-hook at an instruction (mode "hook", writes needed: its first bytes are patched), or a debug
    // register of every thread (modes "execute", "write" and "access", up to 4 at once; the code is not touched, threads
    // made later get it within half a second). Each hit records the registers, the code addresses on the stack (its
    // callers, roughly), the reads, and for a data breakpoint the bytes before and after; the records go to a buffer of
    // `count` (the first ones, or the last with keep "last"), the callers are counted for all hits.

    constexpr size_t kMaxSessionBytes = 32u << 20;
    constexpr size_t kMaxSpecs = 32;
    constexpr size_t kMaxSpecSize = 4096;
    constexpr size_t kMaxStackAddresses = 16;
    constexpr size_t kStackScanBytes = 512;
    constexpr size_t kHookSlots = 8;
    constexpr size_t kCallerSlots = 512;
    constexpr size_t kMaxRecordsAnswered = 5000;

    static std::atomic<uint32_t> frameCount{0};    // frames presented
    static LARGE_INTEGER clockFrequency{};

    uint64_t Microseconds()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const uint64_t ticks = uint64_t(now.QuadPart), frequency = uint64_t(clockFrequency.QuadPart);
        return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
    }

    struct Regs
    {
        uint32_t r[8];      // eax ecx edx ebx esp ebp esi edi, the encoding's order
        uint32_t eflags;
        uint32_t eip;       // the hooked instruction; a data breakpoint's next one, after the access
    };
    constexpr const char* kRegisterNames[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

    struct Spec
    {
        int reg = -1;       // the base is a register, or
        uint32_t base = 0;  // a number
        std::vector<uint32_t> offsets;
        uint32_t size = 4;
    };

    std::vector<Spec> ParseSpecs(const std::string& text, bool registers)
    {
        std::vector<Spec> specs;
        for (size_t begin = 0; begin < text.size();)
        {
            size_t end = text.find(';', begin);
            if (end == std::string::npos)
                end = text.size();
            std::string item;
            for (char c : text.substr(begin, end - begin))
                if (c != ' ')
                    item += char(tolower(uint8_t(c)));
            begin = end + 1;
            if (item.empty())
                continue;
            Spec spec;
            const size_t colon = item.rfind(':');
            if (colon != std::string::npos)
            {
                spec.size = uint32_t(ParseWhole(item.substr(colon + 1), "size"));
                item.resize(colon);
            }
            std::vector<std::string> parts;
            for (size_t at = 0;;)
            {
                const size_t comma = item.find(',', at);
                parts.push_back(item.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
            const auto named = std::find(std::begin(kRegisterNames), std::end(kRegisterNames), parts[0]);
            if (named != std::end(kRegisterNames))
            {
                if (!registers)
                    throw std::runtime_error("a watch has no registers: " + parts[0]);
                spec.reg = int(named - std::begin(kRegisterNames));
            }
            else
                spec.base = uint32_t(ParseWhole(parts[0], "base"));
            for (size_t i = 1; i < parts.size(); ++i)
                spec.offsets.push_back(uint32_t(ParseWhole(parts[i], "offset")));
            if (!spec.size || spec.size > kMaxSpecSize || spec.offsets.size() > 16 || (spec.offsets.empty() && spec.size > 4))
                throw std::runtime_error("a spec reads 1..4096 bytes through at most 16 offsets, a bare value at most 4");
            specs.push_back(std::move(spec));
            if (specs.size() > kMaxSpecs)
                throw std::runtime_error("at most 32 specs");
        }
        return specs;
    }

    // The spec's bytes into out (spec.size of them); false where a pointer on the way or the bytes cannot be read.
    // (SafeCopy, not VirtualQuery, for this runs in hooks: an unmapped page costs an exception that it catches)
    bool Resolve(const Spec& spec, const Regs* regs, uint8_t* out)
    {
        const uint32_t value = spec.reg >= 0 ? regs->r[spec.reg] : spec.base;
        if (spec.offsets.empty())
        {
            memcpy(out, &value, spec.size);
            return true;
        }
        uint32_t address = value + spec.offsets[0];
        for (size_t i = 1; i < spec.offsets.size(); ++i)
        {
            uint32_t pointer = 0;
            if (address < 0x10000 || !SafeCopy(&pointer, reinterpret_cast<const void*>(uintptr_t(address)), 4) || pointer < 0x10000)
                return false;
            address = pointer + spec.offsets[i];
        }
        return address >= 0x10000 && SafeCopy(out, reinterpret_cast<const void*>(uintptr_t(address)), spec.size);
    }

    size_t SpecsSize(const std::vector<Spec>& specs)
    {
        size_t size = 0;
        for (const auto& spec : specs)
            size += spec.size;
        return size;
    }

    // The executable sections of the loaded modules, sorted, to tell return addresses among the stack's dwords.
    struct CodeRange { uint32_t begin, end; };

    std::vector<CodeRange> CodeRanges()
    {
        std::vector<CodeRange> ranges;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return ranges;
        MODULEENTRY32W entry{sizeof(entry)};
        for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
        {
            const auto base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            static uint8_t headers[4096];
            if (!SafeCopy(headers, entry.modBaseAddr, sizeof(headers)))
                continue;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > sizeof(headers))
                continue;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(headers + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                continue;
            const auto* section = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
            {
                if (reinterpret_cast<const uint8_t*>(section + 1) > headers + sizeof(headers))
                    break;
                if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    ranges.push_back({ uint32_t(base + section->VirtualAddress), uint32_t(base + section->VirtualAddress + section->Misc.VirtualSize) });
            }
        }
        CloseHandle(snapshot);
        std::sort(ranges.begin(), ranges.end(), [](const CodeRange& a, const CodeRange& b) { return a.begin < b.begin; });
        return ranges;
    }

    bool InCode(const std::vector<CodeRange>& ranges, uint32_t address)
    {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), address, [](uint32_t a, const CodeRange& r) { return a < r.begin; });
        return it != ranges.begin() && address < (it - 1)->end;
    }

    // Whether address begins an instruction: decoded on from `from` (a function's start, if given), else from the
    // int3 padding before the nearest functions above it (the closest that decodes to it).
    bool OnInstructionBoundary(uint32_t address, uint32_t from)
    {
        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        const auto decodesTo = [&](uint32_t start)
        {
            uint32_t at = start;
            while (at < address)
            {
                ZydisDecodedInstruction instruction;
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, reinterpret_cast<const void*>(uintptr_t(at)), 15, &instruction)))
                    return false;
                at += instruction.length;
            }
            return at == address;
        };
        if (from)
            return from <= address && address - from < 0x100000 && RangeAccessible(from, address - from + 16, false) && decodesTo(from);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(address)), &info, sizeof(info)) || !RangeAccessible(address, 16, false))
            return false;
        const uint32_t low = (std::max)(uint32_t(reinterpret_cast<uintptr_t>(info.BaseAddress)) + 2, address > 0x4000 ? address - 0x4000 : 0u);
        int tried = 0;
        for (uint32_t p = address; p >= low && tried < 8; --p)
        {
            const auto* b = reinterpret_cast<const uint8_t*>(uintptr_t(p));
            if (b[-1] == 0xCC && (b[-2] == 0xCC || b[-2] == 0xC3))
            {
                ++tried;
                if (decodesTo(p))
                    return true;
                // (the rest of this run of padding decodes the same)
                while (p > low && b[-1] == 0xCC)
                    --p, --b;
            }
        }
        return false;
    }

    enum class TraceMode { Hook, Execute, Write, Access };

    // A record: the head, the code addresses on the stack, xmm0..7 if asked, the reads.
    struct TraceHead
    {
        uint64_t time;      // microseconds since the trace began
        uint32_t frame;     // frames presented since it began
        uint32_t thread;
        Regs regs;
        uint32_t before, after;     // a data breakpoint's bytes before the access (as last seen) and after
        uint32_t failed;    // bit i: read i could not be read
    };

    struct Trace
    {
        std::string name, meta;
        TraceMode mode = TraceMode::Hook;
        uint32_t address = 0, length = 0;   // (length: a data breakpoint's 1, 2 or 4 bytes)
        std::vector<Spec> reads;
        bool hasWhen = false, whenNotEqual = false;
        Spec when;
        std::vector<uint8_t> equals;
        size_t stackDepth = 4;
        bool xmm = false, keepLast = false;
        size_t recordSize = 0, capacity = 0;
        std::vector<CodeRange> code;
        uint64_t started = 0;
        uint32_t startFrame = 0;
        int slot = -1;          // its hook slot or debug register
        safetyhook::MidHook hook;
        std::atomic<int> inside{0};
        std::atomic<uint64_t> hits{0};      // all, the condition's misses too
        // Under busy (a spin lock: hits come from any thread, inside hooks, where a waiting thread must not sleep)
        std::atomic_flag busy;
        std::vector<uint8_t> records;
        uint64_t matched = 0, written = 0;
        uint32_t last = 0;      // a data breakpoint's bytes, as the last hit left them
        std::array<std::pair<uint32_t, uint32_t>, kCallerSlots> callers{};  // first code address on the stack, hits
        uint64_t callersOther = 0;

        void Lock() { while (busy.test_and_set(std::memory_order_acquire)) YieldProcessor(); }
        void Unlock() { busy.clear(std::memory_order_release); }
    };

    static std::mutex tracesMutex;      // the server thread's: starting, stopping, reading
    static std::vector<std::unique_ptr<Trace>> traces;
    static std::array<std::atomic<Trace*>, kHookSlots> hookSlots{};
    static std::array<std::atomic<Trace*>, 4> debugSlots{};
    static thread_local bool inHit = false;

    void Hit(Trace& trace, const Regs& regs, const uint8_t* xmm)
    {
        trace.hits.fetch_add(1, std::memory_order_relaxed);
        if (trace.hasWhen)
        {
            uint8_t value[64];
            const bool same = Resolve(trace.when, &regs, value) && !memcmp(value, trace.equals.data(), trace.when.size);
            if (same == trace.whenNotEqual)
                return;
        }
        // The stack's dwords that point into code: the return addresses, and some stale ones
        uint32_t stack[kMaxStackAddresses] = {};
        size_t found = 0;
        if (trace.stackDepth)
        {
            uint32_t words[kStackScanBytes / 4];
            size_t count = std::size(words);
            const void* top = reinterpret_cast<const void*>(uintptr_t(regs.r[4]));
            if (!SafeCopy(words, top, sizeof(words)))
                count = SafeCopy(words, top, 64) ? 16 : 0;
            for (size_t i = 0; i < count && found < trace.stackDepth; ++i)
                if (InCode(trace.code, words[i]))
                    stack[found++] = words[i];
        }
        uint32_t now = 0;
        const bool data = trace.mode == TraceMode::Write || trace.mode == TraceMode::Access;
        if (data)
            SafeCopy(&now, reinterpret_cast<const void*>(uintptr_t(trace.address)), trace.length);

        trace.Lock();
        ++trace.matched;
        {
            const uint32_t key = found ? stack[0] : regs.eip;
            size_t index = (key * 2654435761u) >> 23 & (kCallerSlots - 1);
            bool counted = false;
            for (int probe = 0; probe < 16 && !counted; ++probe, index = (index + 1) & (kCallerSlots - 1))
            {
                auto& slot = trace.callers[index];
                if (slot.second && slot.first != key)
                    continue;
                slot.first = key;
                ++slot.second;
                counted = true;
            }
            if (!counted)
                ++trace.callersOther;
        }
        const uint32_t before = trace.last;
        trace.last = now;
        if (trace.written >= trace.capacity && !trace.keepLast)
            return trace.Unlock();
        uint8_t* record = trace.records.data() + (trace.written % trace.capacity) * trace.recordSize;
        ++trace.written;
        TraceHead head{ Microseconds() - trace.started, frameCount.load(std::memory_order_relaxed) - trace.startFrame, GetCurrentThreadId(), regs,
                        before, now, 0 };
        uint8_t* at = record + sizeof(TraceHead);
        memcpy(at, stack, trace.stackDepth * 4);
        at += trace.stackDepth * 4;
        if (trace.xmm)
        {
            if (xmm)
                memcpy(at, xmm, 128);
            else
                memset(at, 0, 128);
            at += 128;
        }
        for (size_t i = 0; i < trace.reads.size(); ++i)
        {
            if (!Resolve(trace.reads[i], &regs, at))
                head.failed |= 1u << i;
            at += trace.reads[i].size;
        }
        memcpy(record, &head, sizeof(head));
        trace.Unlock();
    }

    // A slot's trace for this hit, kept from being freed while it is used (StopTrace waits for inside to drop).
    template <typename Slots>
    void HitSlot(Slots& slots, size_t n, const Regs& regs, const uint8_t* xmm)
    {
        Trace* trace = slots[n].load(std::memory_order_acquire);
        if (!trace)
            return;
        ++trace->inside;
        inHit = true;
        Hit(*trace, regs, xmm);
        inHit = false;
        --trace->inside;
    }

    template <size_t N>
    void OnHookHit(SafetyHookContext& ctx)
    {
        if (inHit)
            return;
        Trace* trace = hookSlots[N].load(std::memory_order_acquire);
        if (!trace)
            return;
        const Regs regs{ { ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esp, ctx.ebp, ctx.esi, ctx.edi }, ctx.eflags, trace->address };
        HitSlot(hookSlots, N, regs, reinterpret_cast<const uint8_t*>(&ctx.xmm0));
    }

    template <size_t... N>
    constexpr std::array<safetyhook::MidHookFn, kHookSlots> HookFunctions(std::index_sequence<N...>)
    {
        return { &OnHookHit<N>... };
    }
    constexpr auto kHookFunctions = HookFunctions(std::make_index_sequence<kHookSlots>());

    // ---- debug registers: set in every thread of the game but the server's own and the refresher's

    static std::mutex debugMutex;
    static std::array<std::atomic<uint32_t>, 4> debugAddresses{};   // 0: the register is free
    // What each register was last set to, kept after its trace stops: a thread not refreshed yet may still trap there
    static std::array<std::atomic<uint32_t>, 4> ownedAddresses{};
    static std::array<std::atomic<bool>, 4> ownedExecute{};
    static std::atomic<uint32_t> debugDr7{0};
    static DWORD serverThread = 0, refresherThread = 0;
    static PVOID exceptionHandler = nullptr;

    void ApplyDebugRegisters()
    {
        std::lock_guard lock(debugMutex);
        const DWORD dr7 = debugDr7;
        DWORD addresses[4];
        for (int n = 0; n < 4; ++n)
            addresses[n] = debugAddresses[n];
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self || entry.th32ThreadID == serverThread ||
                entry.th32ThreadID == refresherThread)
                continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;
            // (nothing between the suspend and the resume takes a lock or allocates: the thread may hold the heap's)
            if (SuspendThread(thread) != DWORD(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context) && (context.Dr7 != dr7 || context.Dr0 != addresses[0] || context.Dr1 != addresses[1] ||
                    context.Dr2 != addresses[2] || context.Dr3 != addresses[3]))
                {
                    context.Dr0 = addresses[0];
                    context.Dr1 = addresses[1];
                    context.Dr2 = addresses[2];
                    context.Dr3 = addresses[3];
                    context.Dr7 = dr7;
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    SetThreadContext(thread, &context);
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
    }

    // Ours by DR6's bits, or for an execute breakpoint by the address it trapped at: Wine's context for one has DR0..7
    // and DR6's bits cleared (a crash of the first test, 2026-10-23), so the registers' own state is ours to keep
    LONG CALLBACK OnDebugException(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT* c = info->ContextRecord;
        const auto trapped = uint32_t(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        bool ours = false, execute = false;
        for (size_t n = 0; n < 4; ++n)
        {
            const uint32_t owned = ownedAddresses[n];
            if (!owned)
                continue;
            const bool ownExecute = ownedExecute[n];
            if (!(c->Dr6 & (1u << n)) && !(ownExecute && trapped == owned))
                continue;
            // (also a register a stopped trace left in a thread not refreshed yet: swallowed, never passed on)
            ours = true;
            execute |= ownExecute;
            if (inHit || debugAddresses[n] != owned)
                continue;
            uint8_t xmm[128] = {};
            if ((c->ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS)
                memcpy(xmm, c->ExtendedRegisters + 160, sizeof(xmm));  // (FXSAVE's layout: xmm0 at 160)
            const Regs regs{ { c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esp, c->Ebp, c->Esi, c->Edi }, c->EFlags, c->Eip };
            HitSlot(debugSlots, n, regs, xmm);
        }
        if (!ours)
            return EXCEPTION_CONTINUE_SEARCH;
        c->Dr6 &= ~0xFu;
        // (the context goes back to the thread with its debug registers: ours, not the zeros Wine may have put there)
        if (c->ContextFlags & CONTEXT_DEBUG_REGISTERS & ~CONTEXT_i386)
        {
            c->Dr0 = debugAddresses[0];
            c->Dr1 = debugAddresses[1];
            c->Dr2 = debugAddresses[2];
            c->Dr3 = debugAddresses[3];
            c->Dr7 = debugDr7;
        }
        if (execute)
            c->EFlags |= 0x10000; // RF: the instruction runs instead of trapping again
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void UpdateDr7()
    {
        uint32_t dr7 = 0;
        for (size_t n = 0; n < 4; ++n)
        {
            const Trace* trace = debugSlots[n].load();
            if (!trace)
                continue;
            const uint32_t rw = trace->mode == TraceMode::Execute ? 0 : trace->mode == TraceMode::Write ? 1 : 3;
            const uint32_t len = trace->mode == TraceMode::Execute || trace->length == 1 ? 0 : trace->length == 2 ? 1 : 3;
            dr7 |= 1u << (2 * n) | rw << (16 + 4 * n) | len << (18 + 4 * n);
        }
        debugDr7 = dr7;
    }

    // ---- watches: values read each frame (presented, or the game's own while it runs)

    struct WatchHead
    {
        uint64_t time;
        uint32_t frame;
        uint32_t failed;
    };

    struct Watch
    {
        std::string name, meta;
        std::vector<Spec> items;
        bool onGame = false, changesOnly = false, keepLast = true;
        uint32_t every = 1, ticks = 0;
        size_t recordSize = 0, capacity = 0;
        std::vector<uint8_t> records, scratch, previous;
        bool hasPrevious = false;
        uint64_t written = 0, samples = 0, started = 0;
        uint32_t startFrame = 0;
    };

    static std::mutex watchesMutex;
    static std::vector<std::unique_ptr<Watch>> watches;

    void SampleWatches(bool game)
    {
        std::lock_guard lock(watchesMutex);
        for (auto& w : watches)
        {
            if (w->onGame != game || w->ticks++ % w->every)
                continue;
            if (w->written >= w->capacity && !w->keepLast)
                continue;
            WatchHead head{ Microseconds() - w->started, frameCount - w->startFrame, 0 };
            uint8_t* at = w->scratch.data() + sizeof(WatchHead);
            for (size_t i = 0; i < w->items.size(); ++i)
            {
                if (!Resolve(w->items[i], nullptr, at))
                {
                    head.failed |= 1u << i;
                    memset(at, 0, w->items[i].size);
                }
                at += w->items[i].size;
            }
            ++w->samples;
            const size_t valueSize = w->recordSize - sizeof(WatchHead);
            if (w->changesOnly && w->hasPrevious && !memcmp(w->previous.data(), w->scratch.data() + sizeof(WatchHead), valueSize) &&
                head.failed == *reinterpret_cast<const uint32_t*>(w->previous.data() + valueSize))
                continue;
            memcpy(w->previous.data(), w->scratch.data() + sizeof(WatchHead), valueSize);
            memcpy(w->previous.data() + valueSize, &head.failed, 4);
            w->hasPrevious = true;
            memcpy(w->scratch.data(), &head, sizeof(head));
            memcpy(w->records.data() + (w->written % w->capacity) * w->recordSize, w->scratch.data(), w->recordSize);
            ++w->written;
        }
    }

    // ---- the commands

    bool NameTaken(const std::string& name)
    {
        for (const auto& t : traces)
            if (t->name == name)
                return true;
        std::lock_guard lock(watchesMutex);
        for (const auto& w : watches)
            if (w->name == name)
                return true;
        return false;
    }

    size_t Capacity(const Request& request, size_t recordSize, size_t fallback)
    {
        const size_t count = size_t(request.Number("count", fallback));
        if (!count || count * recordSize > kMaxSessionBytes)
            throw std::runtime_error("count is 1.." + std::to_string(kMaxSessionBytes / recordSize) + " for records of " +
                std::to_string(recordSize) + " bytes");
        return count;
    }

    std::string WatchStart(const Request& request)
    {
        std::lock_guard lock(tracesMutex);
        auto w = std::make_unique<Watch>();
        w->name = request.Get("name");
        if (w->name.empty() || NameTaken(w->name))
            throw std::runtime_error("the name is empty or taken: " + w->name);
        w->items = ParseSpecs(request.Get("items"), false);
        if (w->items.empty())
            throw std::runtime_error("no items");
        w->meta = request.Get("meta", "");
        w->onGame = request.Get("on", "present") == "game";
        w->changesOnly = request.Get("changes", "false") == "true";
        w->keepLast = request.Get("keep", "last") != "first";
        w->every = (std::max)(1u, uint32_t(request.Number("every", 1)));
        w->recordSize = sizeof(WatchHead) + SpecsSize(w->items);
        w->capacity = Capacity(request, w->recordSize, 600);
        w->records.resize(w->capacity * w->recordSize);
        w->scratch.resize(w->recordSize);
        w->previous.resize(w->recordSize - sizeof(WatchHead) + 4);
        w->started = Microseconds();
        w->startFrame = frameCount;
        FusionLog::Write("DebugServer", "Watch", "%s: %zu items, every %u %s frame(s), %zu records\n", w->name.c_str(), w->items.size(),
            w->every, w->onGame ? "game" : "presented", w->capacity);
        const std::string name = w->name;
        std::lock_guard watchLock(watchesMutex);
        watches.push_back(std::move(w));
        return Object({{"name", Quote(name)}});
    }

    std::string TraceStart(const Request& request)
    {
        if (!allowWrite)
            throw std::runtime_error("traces patch code or set debug registers: writes are off ([DEBUG] DebugServerAllowWrite)");
        std::lock_guard lock(tracesMutex);
        auto t = std::make_unique<Trace>();
        t->name = request.Get("name");
        if (t->name.empty() || NameTaken(t->name))
            throw std::runtime_error("the name is empty or taken: " + t->name);
        const std::string mode = request.Get("mode", "hook");
        t->mode = mode == "hook" ? TraceMode::Hook : mode == "execute" ? TraceMode::Execute : mode == "write" ? TraceMode::Write :
            mode == "access" ? TraceMode::Access : throw std::runtime_error("mode is hook, execute, write or access");
        t->address = uint32_t(request.Number("addr"));
        t->meta = request.Get("meta", "");
        t->reads = ParseSpecs(request.Get("reads", ""), true);
        if (request.Has("when"))
        {
            const auto when = ParseSpecs(request.Get("when"), true);
            if (when.size() != 1 || when[0].size > 64)
                throw std::runtime_error("when is one spec of at most 64 bytes");
            t->when = when[0];
            t->equals = FromHex(request.Get("equals"));
            if (t->equals.size() != t->when.size)
                throw std::runtime_error("equals has " + std::to_string(t->equals.size()) + " bytes, when reads " + std::to_string(t->when.size));
            t->whenNotEqual = request.Get("op", "eq") == "ne";
            t->hasWhen = true;
        }
        t->stackDepth = (std::min)(size_t(request.Number("stack", 4)), kMaxStackAddresses);
        t->xmm = request.Get("xmm", "false") == "true";
        t->keepLast = request.Get("keep", "first") == "last";
        t->recordSize = sizeof(TraceHead) + t->stackDepth * 4 + (t->xmm ? 128 : 0) + SpecsSize(t->reads);
        t->capacity = Capacity(request, t->recordSize, 1000);
        t->records.resize(t->capacity * t->recordSize);
        t->code = CodeRanges();
        const bool data = t->mode == TraceMode::Write || t->mode == TraceMode::Access;
        if (data)
        {
            t->length = uint32_t(request.Number("length", 4));
            if ((t->length != 1 && t->length != 2 && t->length != 4) || t->address % t->length)
                throw std::runtime_error("a data breakpoint watches 1, 2 or 4 bytes at an address aligned to them");
            SafeCopy(&t->last, reinterpret_cast<const void*>(uintptr_t(t->address)), t->length);
        }
        for (const auto& other : traces)
            if (other->address == t->address && (other->mode == TraceMode::Hook) == (t->mode == TraceMode::Hook))
                throw std::runtime_error("trace " + other->name + " is there already");
        t->started = Microseconds();
        t->startFrame = frameCount;

        if (t->mode == TraceMode::Hook)
        {
            const bool force = request.Get("force", "false") == "true";
            if (!force && !OnInstructionBoundary(t->address, uint32_t(request.Number("func", 0))))
                throw std::runtime_error("the address does not decode as the start of an instruction (give func, the function's start, "
                    "or force: true)");
            for (size_t n = 0; n < kHookSlots && t->slot < 0; ++n)
                if (!hookSlots[n].load())
                    t->slot = int(n);
            if (t->slot < 0)
                throw std::runtime_error("all 8 hook slots are taken");
            // (the slot is set first: the hook can be hit as soon as it is made)
            hookSlots[t->slot] = t.get();
            auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(uintptr_t(t->address)), kHookFunctions[t->slot]);
            if (!hook)
            {
                hookSlots[t->slot] = nullptr;
                throw std::runtime_error("safetyhook could not hook there (error " + std::to_string(int(hook.error().type)) + ")");
            }
            t->hook = std::move(*hook);
        }
        else
        {
            for (size_t n = 0; n < 4 && t->slot < 0; ++n)
                if (!debugSlots[n].load())
                    t->slot = int(n);
            if (t->slot < 0)
                throw std::runtime_error("all 4 debug registers are taken");
            if (!exceptionHandler)
                exceptionHandler = AddVectoredExceptionHandler(1, OnDebugException);
            if (!refresherThread)
            {
                // Threads made later get the registers within half a second
                std::thread([]
                {
                    refresherThread = GetCurrentThreadId();
                    for (;;)
                    {
                        Sleep(500);
                        if (debugDr7)
                            ApplyDebugRegisters();
                    }
                }).detach();
            }
            debugSlots[t->slot] = t.get();
            ownedExecute[t->slot] = t->mode == TraceMode::Execute;
            ownedAddresses[t->slot] = t->address;
            debugAddresses[t->slot] = t->address;
            UpdateDr7();
            ApplyDebugRegisters();
        }
        FusionLog::Write("DebugServer", "Trace", "%s: %s at 0x%08X, %zu reads, %zu records\n", t->name.c_str(), mode.c_str(), t->address,
            t->reads.size(), t->capacity);
        const std::string name = t->name;
        traces.push_back(std::move(t));
        return Object({{"name", Quote(name)}});
    }

    // Unhooked, and freed once no thread is inside its hit any more.
    void StopTrace(Trace& t)
    {
        if (t.mode == TraceMode::Hook)
            hookSlots[t.slot] = nullptr;
        else
        {
            debugSlots[t.slot] = nullptr;
            debugAddresses[t.slot] = 0;
            UpdateDr7();
            ApplyDebugRegisters();
        }
        // (a thread that read the slot just before it was cleared has not counted itself inside yet)
        Sleep(100);
        while (t.inside)
            Sleep(1);
        // Unhooked only now: the hook's stub is freed with it, and a thread in our callback returns through the stub
        if (t.mode == TraceMode::Hook)
            t.hook.reset();
        FusionLog::Write("DebugServer", "Trace", "%s stopped after %llu hits\n", t.name.c_str(), (unsigned long long)t.hits.load());
    }

    std::string TraceStop(const Request& request)
    {
        std::lock_guard lock(tracesMutex);
        const std::string name = request.Get("name");
        size_t stopped = 0;
        for (auto it = traces.begin(); it != traces.end();)
        {
            if (name == "all" || (*it)->name == name)
            {
                StopTrace(**it);
                it = traces.erase(it);
                ++stopped;
            }
            else
                ++it;
        }
        {
            std::lock_guard watchLock(watchesMutex);
            stopped += std::erase_if(watches, [&](const auto& w) { return name == "all" || w->name == name; });
        }
        if (!stopped && name != "all")
            throw std::runtime_error("no trace or watch " + name);
        return Object({{"stopped", std::to_string(stopped)}});
    }

    std::string Hex32(uint32_t value)
    {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "\"0x%08X\"", value);
        return buffer;
    }

    // Records from number `from` on (as numbered since the start; the oldest kept if earlier), at most `max`.
    std::pair<uint64_t, uint64_t> Window(uint64_t wantedFrom, uint64_t limit, uint64_t written, size_t capacity, bool keepLast)
    {
        const uint64_t first = keepLast && written > capacity ? written - capacity : 0;
        const uint64_t kept = (std::min<uint64_t>)(written, capacity);
        const uint64_t from = (std::max)(first, wantedFrom);
        return { from, (std::max)(from, (std::min)(first + kept, from + limit)) };
    }

    std::string TraceRead(const Request& request)
    {
        std::lock_guard lock(tracesMutex);
        const std::string name = request.Get("name");
        // (read before a trace's spin lock is taken: a throw while it is held would stop the game's threads)
        const uint64_t wantedFrom = request.Number("from", 0);
        const uint64_t limit = (std::min<uint64_t>)(request.Number("max", 200), kMaxRecordsAnswered);
        for (auto& t : traces)
        {
            if (t->name != name)
                continue;
            std::vector<uint8_t> copy;
            uint64_t written = 0, matched = 0, other = 0;
            std::vector<std::pair<uint32_t, uint32_t>> callers;
            t->Lock();
            written = t->written;
            matched = t->matched;
            other = t->callersOther;
            const auto [from, to] = Window(wantedFrom, limit, written, t->capacity, t->keepLast);
            if (to > from)
                copy.resize(size_t(to - from) * t->recordSize);
            for (uint64_t n = from; n < to; ++n)
                memcpy(copy.data() + size_t(n - from) * t->recordSize, t->records.data() + size_t(n % t->capacity) * t->recordSize, t->recordSize);
            for (const auto& slot : t->callers)
                if (slot.second)
                    callers.push_back(slot);
            t->Unlock();
            std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (callers.size() > 64)
                callers.resize(64);

            std::vector<std::string> records;
            for (uint64_t n = from; n < to; ++n)
            {
                const uint8_t* record = copy.data() + size_t(n - from) * t->recordSize;
                TraceHead head;
                memcpy(&head, record, sizeof(head));
                const uint8_t* at = record + sizeof(TraceHead);
                std::vector<std::string> stack;
                for (size_t i = 0; i < t->stackDepth; ++i)
                {
                    uint32_t address;
                    memcpy(&address, at + i * 4, 4);
                    if (address)
                        stack.push_back(Hex32(address));
                }
                at += t->stackDepth * 4;
                Fields fields = { {"n", std::to_string(n)}, {"t", std::to_string(head.time)}, {"f", std::to_string(head.frame)},
                                  {"tid", std::to_string(head.thread)}, {"regs", Quote(ToHex(reinterpret_cast<const uint8_t*>(&head.regs), sizeof(Regs)))},
                                  {"stack", Array(stack)} };
                if (t->length)
                {
                    fields.push_back({"before", Quote(ToHex(reinterpret_cast<const uint8_t*>(&head.before), t->length))});
                    fields.push_back({"after", Quote(ToHex(reinterpret_cast<const uint8_t*>(&head.after), t->length))});
                }
                if (t->xmm)
                {
                    fields.push_back({"xmm", Quote(ToHex(at, 128))});
                    at += 128;
                }
                std::vector<std::string> reads;
                for (size_t i = 0; i < t->reads.size(); ++i)
                {
                    reads.push_back(head.failed & (1u << i) ? "null" : Quote(ToHex(at, t->reads[i].size)));
                    at += t->reads[i].size;
                }
                fields.push_back({"reads", Array(reads)});
                records.push_back(Object(fields));
            }
            std::vector<std::string> callerItems;
            for (const auto& [address, count] : callers)
                callerItems.push_back(Object({{"addr", Hex32(address)}, {"hits", std::to_string(count)}}));
            static constexpr const char* kModes[] = { "hook", "execute", "write", "access" };
            return Object({{"name", Quote(t->name)}, {"kind", Quote("trace")}, {"mode", Quote(kModes[int(t->mode)])},
                {"addr", Hex32(t->address)}, {"meta", Quote(t->meta)}, {"hits", std::to_string(t->hits.load())},
                {"matched", std::to_string(matched)}, {"written", std::to_string(written)}, {"capacity", std::to_string(t->capacity)},
                {"keep", Quote(t->keepLast ? "last" : "first")}, {"seconds", Float(float(double(Microseconds() - t->started) * 1e-6))},
                {"frames", std::to_string(frameCount - t->startFrame)}, {"callers", Array(callerItems)},
                {"callers_other", std::to_string(other)}, {"records", Array(records)}});
        }
        std::lock_guard watchLock(watchesMutex);
        for (auto& w : watches)
        {
            if (w->name != name)
                continue;
            const auto [from, to] = Window(wantedFrom, limit, w->written, w->capacity, w->keepLast);
            std::vector<std::string> records;
            for (uint64_t n = from; n < to; ++n)
            {
                const uint8_t* record = w->records.data() + size_t(n % w->capacity) * w->recordSize;
                WatchHead head;
                memcpy(&head, record, sizeof(head));
                const uint8_t* at = record + sizeof(WatchHead);
                std::vector<std::string> values;
                for (size_t i = 0; i < w->items.size(); ++i)
                {
                    values.push_back(head.failed & (1u << i) ? "null" : Quote(ToHex(at, w->items[i].size)));
                    at += w->items[i].size;
                }
                records.push_back(Object({{"n", std::to_string(n)}, {"t", std::to_string(head.time)}, {"f", std::to_string(head.frame)},
                    {"values", Array(values)}}));
            }
            return Object({{"name", Quote(w->name)}, {"kind", Quote("watch")}, {"meta", Quote(w->meta)},
                {"samples", std::to_string(w->samples)}, {"written", std::to_string(w->written)}, {"capacity", std::to_string(w->capacity)},
                {"keep", Quote(w->keepLast ? "last" : "first")}, {"seconds", Float(float(double(Microseconds() - w->started) * 1e-6))},
                {"frames", std::to_string(frameCount - w->startFrame)}, {"records", Array(records)}});
        }
        throw std::runtime_error("no trace or watch " + name);
    }

    std::string TraceList()
    {
        std::lock_guard lock(tracesMutex);
        std::vector<std::string> items;
        static constexpr const char* kModes[] = { "hook", "execute", "write", "access" };
        for (auto& t : traces)
        {
            t->Lock();
            const uint64_t written = t->written;
            t->Unlock();
            items.push_back(Object({{"name", Quote(t->name)}, {"kind", Quote("trace")}, {"mode", Quote(kModes[int(t->mode)])},
                {"addr", Hex32(t->address)}, {"hits", std::to_string(t->hits.load())}, {"written", std::to_string(written)},
                {"capacity", std::to_string(t->capacity)}}));
        }
        std::lock_guard watchLock(watchesMutex);
        for (auto& w : watches)
            items.push_back(Object({{"name", Quote(w->name)}, {"kind", Quote("watch")}, {"samples", std::to_string(w->samples)},
                {"written", std::to_string(w->written)}, {"capacity", std::to_string(w->capacity)}}));
        return Object({{"sessions", Array(items)}});
    }

    // ---- what is near a place: the buildings (map models, procedural props) and objects within a radius
    //
    // Pools by their CE globals (CPools::ms_pBuildingPool 0x12bd0e8, ms_pObjectPool 0x1632c60): the building pool's
    // signature holds the 500 that procedural.ixx raises at start, so a search for it after that found nothing.
    // Distances are to the model's bounding box placed in the world (0 inside it), so a 30 m row of bushes or a map
    // section whose origin is far off counts from its nearest part. A model whose drawable is in (as CDrawEntityDC's
    // execute CE 0x8dcbf0 takes it: model info +8, a fragment type, its +0xB4, else what +0xC points to) gives its
    // shaders: drawable +8 the grmShaderGroup, +8/+0xC its shaders and their
    // count; a grmShaderFx has its parameters' data at +0x14, their count at +0x1C, their types at +0x24 (0 a texture,
    // its name at +0x14, "pack:/name.dds"), the effect's name at +0x44 and the preset's (.sps) at +0x48.

    struct PoolHeader { uint32_t storage, flags; int32_t size, stride; };

    // The pool a global points to, read safely; false where it does not look like one.
    bool ReadPool(uintptr_t global, PoolHeader& pool)
    {
        uint32_t address = 0;
        if (!global || !SafeCopy(&address, reinterpret_cast<const void*>(global), 4) || address < 0x10000 ||
            !SafeCopy(&pool, reinterpret_cast<const void*>(uintptr_t(address)), sizeof(pool)))
            return false;
        return pool.storage >= 0x10000 && pool.flags >= 0x10000 && pool.size > 0 && pool.size <= 200000 && pool.stride >= 0x24 &&
            pool.stride <= 0x10000;
    }

    template <typename T>
    bool ReadValue(uintptr_t address, T& value)
    {
        return address >= 0x10000 && SafeCopy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    }

    // A short C string from game memory (printable ASCII only, up to 63 characters), read a few bytes at a time so the
    // end of a committed page does not fail it; empty when there is none
    std::string ReadName(uintptr_t address)
    {
        std::string text;
        char chunk[16];
        while (text.size() < 63 && ReadValue(address + text.size(), chunk))
        {
            for (char c : chunk)
            {
                if (!c)
                    return text;
                if (uint8_t(c) < 0x20 || uint8_t(c) >= 0x7F || text.size() >= 63)
                    return {};
                text += c;
            }
        }
        return {};
    }

    // A model's shaders: per shader its preset (or effect) and textures, as "gta_cutout.sps: cj_stalk1, cj_flower_256"
    std::vector<std::string> ModelShaders(uint32_t info)
    {
        std::vector<std::string> shaders;
        uint32_t fragType = 0, slot = 0, drawable = 0, group = 0, array = 0;
        uint16_t count = 0;
        if (!ReadValue(info + 8, fragType) ||
            !(fragType ? ReadValue(fragType + 0xB4, drawable) : ReadValue(info + 0xC, slot) && ReadValue(slot, drawable)) ||
            !ReadValue(drawable + 8, group) ||
            !ReadValue(group + 8, array) || !ReadValue(group + 0xC, count) || !count || count > 64)
            return shaders;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t shader = 0, data = 0, types = 0, names[2] = {};
            int32_t parameters = 0;
            if (!ReadValue(array + i * 4, shader) || !ReadValue(shader + 0x14, data) || !ReadValue(shader + 0x1C, parameters) ||
                !ReadValue(shader + 0x24, types) || !ReadValue(shader + 0x44, names))
                continue;
            std::string line = ReadName(names[1]);
            if (line.empty())
                line = ReadName(names[0]);
            if (line.empty())
                continue;
            std::string textures;
            for (int32_t p = 0; p < (std::min)(parameters, 32); ++p)
            {
                uint8_t type = 0xFF;
                uint32_t texture = 0, name = 0;
                if (!ReadValue(types + p, type) || type != 0 || !ReadValue(data + p * 4, texture) || !ReadValue(texture + 0x14, name))
                    continue;
                std::string text = ReadName(name);
                if (text.starts_with("pack:/"))
                    text.erase(0, 6);
                if (text.ends_with(".dds"))
                    text.erase(text.size() - 4);
                if (!text.empty())
                    textures += (textures.empty() ? ": " : ", ") + text;
            }
            line += textures;
            if (std::find(shaders.begin(), shaders.end(), line) == shaders.end())
                shaders.push_back(line);
        }
        return shaders;
    }

    std::string Lower(std::string text)
    {
        for (auto& c : text)
            c = char(tolower(uint8_t(c)));
        return text;
    }

    std::string Nearby(const Request& request)
    {
        const float x = std::stof(request.Get("x")), y = std::stof(request.Get("y")), z = std::stof(request.Get("z"));
        const float radius = std::stof(request.Get("radius", "10"));
        const size_t limit = (std::min)(size_t(request.Number("max", 100)), size_t(1000));
        const std::string kindFilter = request.Get("kind", "all");
        // model: an index (decimal) or a hash (0x...); shader / texture: parts of their names, any case
        const std::string modelText = request.Get("model", "");
        const bool modelIsHash = modelText.starts_with("0x") || modelText.starts_with("0X");
        const uint32_t modelWanted = modelText.empty() ? 0 : uint32_t(std::stoul(modelText, nullptr, modelIsHash ? 16 : 10));
        const std::string shaderWanted = Lower(request.Get("shader", "")), textureWanted = Lower(request.Get("texture", ""));
        const float minTop = std::stof(request.Get("min_top", "-1e9")), maxTop = std::stof(request.Get("max_top", "1e9"));
        const bool group = request.Get("group", "false") == "true";
        const bool details = request.Get("shaders", "true") == "true" || !shaderWanted.empty() || !textureWanted.empty();
        const uintptr_t modelInfos = GameBase() + 0xE95CD8;   // CE 0x1295cd8, model infos by index

        struct Model { bool read = false; uint32_t hash = 0, info = 0; float sphere[4] = {}, box[8] = {}; std::vector<std::string> shaders; bool shadersRead = false; };
        std::unordered_map<int16_t, Model> models;
        struct Found
        {
            float distance, originDistance;
            const char* kind;
            uint32_t entity, flags210, lodParent;
            int16_t index;
            float position[3], up;
            uint8_t alpha;
        };
        std::vector<Found> found;
        const std::pair<const char*, uintptr_t> pools[] = { { "building", GameBase() + 0xEBD0E8 }, { "object", GameBase() + 0x1232C60 } };
        for (const auto& [kind, global] : pools)
        {
            if (kindFilter != "all" && kindFilter != kind)
                continue;
            PoolHeader pool;
            if (!ReadPool(global, pool))
                continue;
            std::vector<uint8_t> flags(size_t(pool.size));
            if (!SafeCopy(flags.data(), reinterpret_cast<const void*>(uintptr_t(pool.flags)), flags.size()))
                continue;
            for (int32_t i = 0; i < pool.size; ++i)
            {
                if (flags[i] & 0x80)
                    continue;
                const uint32_t entity = pool.storage + uint32_t(i) * uint32_t(pool.stride);
                uint8_t head[0x70];
                if (!SafeCopy(head, reinterpret_cast<const void*>(uintptr_t(entity)), sizeof(head)))
                    continue;
                int16_t index = -1;
                memcpy(&index, head + 0x2E, 2);
                if (index < 0 || index >= 31000)
                    continue;
                auto& model = models[index];
                if (!model.read)
                {
                    model.read = true;
                    if (ReadValue(modelInfos + uintptr_t(index) * 4, model.info) && model.info >= 0x10000)
                    {
                        SafeCopy(model.sphere, reinterpret_cast<const void*>(uintptr_t(model.info + 0x10)), sizeof(model.sphere));
                        SafeCopy(model.box, reinterpret_cast<const void*>(uintptr_t(model.info + 0x20)), sizeof(model.box));
                        ReadValue(model.info + 0x3C, model.hash);
                    }
                }
                if (!model.info || (modelIsHash ? model.hash != modelWanted : !modelText.empty() && uint32_t(index) != modelWanted))
                    continue;
                if (!(model.box[6] >= minTop && model.box[6] <= maxTop))
                    continue;
                // The position (+0x10) and heading (+0x1C), or the matrix (+0x20) where there is one
                uint32_t matrixAddress = 0;
                memcpy(&matrixAddress, head + 0x20, 4);
                float matrix[16];
                const bool hasMatrix = matrixAddress >= 0x10000 && SafeCopy(matrix, reinterpret_cast<const void*>(uintptr_t(matrixAddress)), sizeof(matrix));
                float position[3], heading = 0.0f;
                memcpy(position, hasMatrix ? matrix + 12 : reinterpret_cast<const float*>(head + 0x10), sizeof(position));
                memcpy(&heading, head + 0x1C, 4);
                const float dx = position[0] - x, dy = position[1] - y, dz = position[2] - z;
                const float originDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float reach = std::abs(model.sphere[3]) + std::sqrt(model.sphere[0] * model.sphere[0] + model.sphere[1] * model.sphere[1] + model.sphere[2] * model.sphere[2]);
                if (!(originDistance <= radius + reach))
                    continue;
                // The place in the model's space (the matrix's rows as its axes, or turned by the heading)
                float local[3];
                if (hasMatrix)
                    for (int k = 0; k < 3; ++k)
                        local[k] = -(dx * matrix[k * 4] + dy * matrix[k * 4 + 1] + dz * matrix[k * 4 + 2]);
                else
                {
                    const float c = std::cos(heading), s = std::sin(heading);
                    local[0] = -(dx * c + dy * s);
                    local[1] = -(-dx * s + dy * c);
                    local[2] = -dz;
                }
                float outside = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float d = (std::max)((std::max)(model.box[k] - local[k], local[k] - model.box[4 + k]), 0.0f);
                    outside += d * d;
                }
                const float distance = std::sqrt(outside);
                if (!(distance <= radius))
                    continue;
                if (details && !model.shadersRead)
                {
                    model.shaders = ModelShaders(model.info);
                    model.shadersRead = !model.shaders.empty();
                }
                if (!shaderWanted.empty() || !textureWanted.empty())
                {
                    bool match = false;
                    for (const auto& line : model.shaders)
                    {
                        const auto lower = Lower(line);
                        const auto colon = lower.find(':');
                        const auto shaderPart = lower.substr(0, colon), texturePart = colon == std::string::npos ? std::string() : lower.substr(colon);
                        if ((shaderWanted.empty() || shaderPart.find(shaderWanted) != std::string::npos) &&
                            (textureWanted.empty() || texturePart.find(textureWanted) != std::string::npos))
                            match = true;
                    }
                    if (!match)
                        continue;
                }
                Found item{ distance, originDistance, kind, entity, 0, 0, index, { position[0], position[1], position[2] }, hasMatrix ? matrix[10] : 1.0f, head[0x63] };
                memcpy(&item.lodParent, head + 0x4C, 4);
                ReadValue(entity + 0x210, item.flags210);
                found.push_back(item);
            }
        }
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance || (a.distance == b.distance && a.originDistance < b.originDistance); });

        auto modelFields = [&](int16_t index) -> Fields
        {
            const auto& model = models[index];
            std::vector<std::string> shaders;
            for (const auto& line : model.shaders)
                shaders.push_back(Quote(line));
            return { {"model", std::to_string(index)}, {"hash", Hex32(model.hash)}, {"info", Hex32(model.info)},
                {"size", Array({Float(model.box[4] - model.box[0]), Float(model.box[5] - model.box[1]), Float(model.box[6] - model.box[2])})},
                {"top", Float(model.box[6])}, {"bottom", Float(model.box[2])}, {"shaders", Array(shaders)} };
        };
        std::vector<std::string> items;
        if (group)
        {
            // One row per model, nearest first: how many, the nearest one's distance and entity
            std::vector<int16_t> order;
            std::unordered_map<int16_t, std::pair<size_t, const Found*>> counts;
            for (const auto& item : found)
            {
                auto& entry = counts[item.index];
                if (!entry.first++)
                {
                    entry.second = &item;
                    order.push_back(item.index);
                }
            }
            for (size_t i = 0; i < order.size() && i < limit; ++i)
            {
                const auto& [count, nearest] = counts[order[i]];
                auto fields = modelFields(order[i]);
                fields.insert(fields.begin(), { {"kind", Quote(nearest->kind)}, {"count", std::to_string(count)}, {"distance", Float(nearest->distance)},
                    {"entity", Hex32(nearest->entity)}, {"pos", Array({Float(nearest->position[0]), Float(nearest->position[1]), Float(nearest->position[2])})} });
                items.push_back(Object(fields));
            }
            return Object({{"total", std::to_string(found.size())}, {"models", std::to_string(order.size())}, {"entities", Array(items)}});
        }
        for (size_t i = 0; i < found.size() && i < limit; ++i)
        {
            const auto& item = found[i];
            auto fields = modelFields(item.index);
            fields.insert(fields.begin(), { {"kind", Quote(item.kind)}, {"entity", Hex32(item.entity)}, {"distance", Float(item.distance)},
                {"origin_distance", Float(item.originDistance)},
                {"pos", Array({Float(item.position[0]), Float(item.position[1]), Float(item.position[2])})}, {"up", Float(item.up)},
                {"flags210", Hex32(item.flags210)}, {"lod_parent", Hex32(item.lodParent)}, {"alpha", std::to_string(item.alpha)} });
            items.push_back(Object(fields));
        }
        return Object({{"total", std::to_string(found.size())}, {"entities", Array(items)}});
    }

    std::string Handle(const Request& request)
    {
        const std::string command = request.Get("cmd");
        if (command == "ping") return Object({{"pong", "true"}});
        if (command == "info") return Info();
        if (command == "modules") return Modules();
        if (command == "vmmap") return VirtualMemoryMap(request);
        if (command == "read") return Read(request);
        if (command == "write") return Write(request);
        if (command == "find") return Find(request);
        if (command == "pattern") return Pattern(request);
        if (command == "ini_get") return IniGet(request);
        if (command == "ini_set") return IniSet(request);
        if (command == "log_tail") return LogTail(request);
        if (command == "files") return Files(request);
        if (command == "file") return File(request);
        if (command == "screenshot") return Screenshot(request);
        if (command == "scene") return Scene();
        if (command == "teleport") return Teleport(request);
        if (command == "set_time") return SetTime(request);
        if (command == "set_weather") return SetWeather(request);
        if (command == "clock_rate") return ClockRate(request);
        if (command == "freeze") return Freeze(request);
        if (command == "watch_start") return WatchStart(request);
        if (command == "trace_start") return TraceStart(request);
        if (command == "trace_read") return TraceRead(request);
        if (command == "trace_stop") return TraceStop(request);
        if (command == "trace_list") return TraceList();
        if (command == "nearby") return Nearby(request);
        if (command == "profiler") return GpuProfiler(request);
        if (command == "cpu_profile") return CpuProfile(request);
        if (command == "frame_capture") return FrameCaptureRequest(request);
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
        QueryPerformanceFrequency(&clockFrequency);
        std::thread([listener]
        {
            serverThread = GetCurrentThreadId();
            for (;;)
            {
                SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                {
                    Sleep(100); // (a broken listener fails at once: not a busy loop)
                    continue;
                }
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
            SettingsReader reader;
            const int port = reader.ReadInteger("DEBUG", "DebugServerPort", 0);
            DebugServer::allowWrite = reader.ReadInteger("DEBUG", "DebugServerAllowWrite", 0) != 0;
            if (port > 0 && port < 65536)
            {
                FusionFix::onBeforePresent() += []()
                {
                    DebugServer::TakeScreenshot();
                    ++DebugServer::frameCount;
                    DebugServer::SampleWatches(false);
                };
                FusionFix::onGameProcessEvent() += []()
                {
                    DebugServer::RunMainThreadCalls();
                    DebugServer::SampleWatches(true);
                };
                DebugServer::Start(port);
            }
        };
    }
} DebugServerModule;
