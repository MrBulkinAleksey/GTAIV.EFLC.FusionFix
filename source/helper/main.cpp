// GTAIV.EFLC.FusionFix.exe
//
// 64-bit helper of the plugin: NVIDIA DLSS (NGX) and AMD FSR only exist for 64-bit processes. It runs on
// its own D3D12 device, on the adapter the game renders with, and exchanges the frames with the game
// through shared textures and a shared fence. See upscaler_protocol.hpp for the protocol.
//
// This software contains source code provided by NVIDIA Corporation.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_dx12.h>

#include "upscaler_protocol.hpp"

using Microsoft::WRL::ComPtr;
namespace Protocol = UpscalerProtocol;

namespace
{
    // -----------------------------------------------------------------------------------------------
    // Log

    FILE* gLog = nullptr;

    void Log(const char* format, ...)
    {
        if (!gLog)
            return;

        SYSTEMTIME time;
        GetLocalTime(&time);
        // The format of FusionFix's own logs (FusionLog.hpp): "[hh:mm:ss.mmm] [Feature.Component] text".
        fprintf(gLog, "[%02d:%02d:%02d.%03d] [UpscalerHelper] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

        va_list args;
        va_start(args, format);
        vfprintf(gLog, format, args);
        va_end(args);

        fputc('\n', gLog);
        fflush(gLog);
    }

    // -----------------------------------------------------------------------------------------------
    // Connection to the game

    struct Connection
    {
        HANDLE mapping = nullptr;
        HANDLE request = nullptr;
        HANDLE response = nullptr;
        HANDLE game = nullptr;
        Protocol::Shared* shared = nullptr;

        bool Open(const std::wstring& name)
        {
            mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (name + Protocol::MappingSuffix).c_str());
            request = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + Protocol::RequestSuffix).c_str());
            response = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (name + Protocol::ResponseSuffix).c_str());
            if (!mapping || !request || !response)
                return false;

            shared = static_cast<Protocol::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Protocol::Shared)));
            if (!shared || shared->Version != Protocol::Version)
                return false;

            game = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, shared->GameProcessId);
            return game != nullptr;
        }

        // serial: of the request answered. The game may already have written the next one while this was worked on.
        void Respond(Protocol::Status status, uint32_t serial)
        {
            shared->ResponseStatus = status;
            MemoryBarrier();
            shared->ResponseSerial = serial;
            MemoryBarrier();
            SetEvent(response);
        }

        void Message(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            vsnprintf(shared->Message, sizeof(shared->Message), format, args);
            va_end(args);
            Log("%s", shared->Message);
        }
    };

    // -----------------------------------------------------------------------------------------------
    // D3D12

    struct SharedTexture
    {
        ComPtr<ID3D12Resource> resource;
        HANDLE handle = nullptr;
    };

    class Device
    {
    public:
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence> inputFence;     // signalled by the game when the inputs are copied
        ComPtr<ID3D12Fence> outputFence;    // signalled here when the output is ready, by both queues in order
        ComPtr<ID3D12Fence> presentFence;   // presentation: signalled by the present thread once Present and HudLess are free
        HANDLE presentFenceHandle = nullptr;
        ComPtr<ID3D12Fence> generatedFence; // presentation: Generate done, for the present thread; this process only
        uint64_t generatedValue = 0;
        ComPtr<ID3D12Fence> localFence;
        uint64_t localValue = 0;
        HANDLE fenceEvent = nullptr;

        // Command lists of the frames, on a queue each: Evaluate on the direct queue, Generate on the compute queue with
        // asyncGeneration, where the GPU can run it beside the game's next frame, on the direct queue otherwise. Their
        // allocators are reused once the GPU finished with them.
        static constexpr uint32_t Frames = 6;
        enum class Work : uint32_t { None, Evaluate, Generate, Present };
        struct Ring
        {
            ComPtr<ID3D12CommandQueue> queue;
            std::array<ComPtr<ID3D12CommandAllocator>, Frames> allocators;
            std::array<uint64_t, Frames> allocatorValues{};
            std::array<ID3D12Fence*, Frames> allocatorFences{};   // the fence allocatorValues is of; null: FrameFence()
            ComPtr<ID3D12GraphicsCommandList> list;
            uint32_t frame = 0;
            uint64_t lastSignal = 0;        // the last value of the shared fence it was given to signal
            // Timestamps, see below
            uint32_t stampBase = 0;
            double ticksPerMs = 0.0;
            std::array<uint32_t, Frames> stampCounts{};
            std::array<Work, Frames> stampWork{};

            bool Create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc{};
                queueDesc.Type = type;
                if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                    return false;
                for (auto& allocator : allocators)
                    if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))))
                        return false;
                if (FAILED(device->CreateCommandList(0, type, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))))
                    return false;
                list->Close();
                uint64_t frequency = 0;
                if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency)
                    ticksPerMs = static_cast<double>(frequency) / 1000.0;
                return true;
            }
        };
        Ring direct;
        Ring compute;                       // its queue is empty when it could not be made
        Ring* current = &direct;            // the ring of the frame being recorded
        bool asyncGeneration = false;
        ComPtr<ID3D12CommandQueue> queue;   // the direct one, for immediate work
        ComPtr<ID3D12GraphicsCommandList> list;

        std::array<SharedTexture, static_cast<size_t>(Protocol::Texture::Count)> textures;
        HANDLE inputFenceHandle = nullptr;
        HANDLE outputFenceHandle = nullptr;

        // GPU time of the helper's own work, a few timestamps a command list, read back once its allocator is reused
        // and logged every 300 frames: what the upscaler and the frame generation cost without the copies and the
        // waits on the game's side
        static constexpr uint32_t StampsPerFrame = 4;
        ComPtr<ID3D12QueryHeap> queryHeap;
        ComPtr<ID3D12Resource> queryReadback;
        const uint64_t* queryData = nullptr;
        struct GpuStats
        {
            uint32_t evaluates = 0, generates = 0, prepares = 0, asyncGenerates = 0;
            double upscale = 0.0, prepare = 0.0, generate = 0.0, evaluateList = 0.0, generateList = 0.0;
            double upscaleMax = 0.0, generateMax = 0.0;
        } gpuStats;

        void CreateTimestamps()
        {
            constexpr uint32_t count = 2 * Frames * StampsPerFrame;
            D3D12_QUERY_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heapDesc.Count = count;
            if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap))))
                return;

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = sizeof(uint64_t) * count;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* mapped = nullptr;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&queryReadback))) ||
                FAILED(queryReadback->Map(0, nullptr, &mapped)) || !mapped)
            {
                queryHeap.Reset();
                queryReadback.Reset();
                return;
            }
            queryData = static_cast<const uint64_t*>(mapped);
            direct.stampBase = 0;
            compute.stampBase = Frames * StampsPerFrame;
        }

        // A timestamp in the frame's command list
        void Stamp(ID3D12GraphicsCommandList* cmd)
        {
            auto& r = *current;
            if (!queryHeap || !r.ticksPerMs || r.stampCounts[r.frame] >= StampsPerFrame)
                return;
            cmd->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, r.stampBase + r.frame * StampsPerFrame + r.stampCounts[r.frame]++);
        }

        // Before the frame's command list is closed
        void ResolveStamps()
        {
            auto& r = *current;
            auto first = r.stampBase + r.frame * StampsPerFrame;
            if (queryHeap && r.stampCounts[r.frame])
                r.list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, r.stampCounts[r.frame], queryReadback.Get(),
                    sizeof(uint64_t) * first);
        }

        // The timestamps of a frame the GPU has finished: Evaluate has start, upscaled, prepared; Generate start, generated
        void ReadStamps(Ring& r, uint32_t slot)
        {
            auto count = r.stampCounts[slot];
            auto work = r.stampWork[slot];
            r.stampCounts[slot] = 0;
            r.stampWork[slot] = Work::None;
            if (!queryData || count < 2)
                return;
            auto t = queryData + r.stampBase + slot * StampsPerFrame;
            auto ms = [&](uint32_t from, uint32_t to) { return t[to] > t[from] ? static_cast<double>(t[to] - t[from]) / r.ticksPerMs : 0.0; };
            auto& g = gpuStats;
            if (work == Work::Evaluate)
            {
                ++g.evaluates;
                auto upscale = ms(0, 1);
                g.upscale += upscale;
                g.upscaleMax = std::max(g.upscaleMax, upscale);
                if (count >= 3)
                {
                    ++g.prepares;
                    g.prepare += ms(1, 2);
                }
                g.evaluateList += ms(0, count - 1);
            }
            else if (work == Work::Generate)
            {
                ++g.generates;
                g.asyncGenerates += &r == &compute;
                auto generate = ms(0, 1);
                g.generate += generate;
                g.generateMax = std::max(g.generateMax, generate);
                g.generateList += ms(0, count - 1);
            }

            if (g.evaluates >= 300)
            {
                Log("GPU milliseconds of the helper's work over %u frames: upscaler %.3f (%.3f at most), Evaluate's command list %.3f",
                    g.evaluates, g.upscale / g.evaluates, g.upscaleMax, g.evaluateList / g.evaluates);
                if (g.prepares)
                    Log("  frame generation: prepare %.3f over %u frames, generate %.3f (%.3f at most) over %u frames (%u on the compute queue), Generate's command list %.3f",
                        g.prepare / g.prepares, g.prepares, g.generates ? g.generate / g.generates : 0.0, g.generateMax, g.generates, g.asyncGenerates,
                        g.generates ? g.generateList / g.generates : 0.0);
                g = {};
            }
        }

        // Wine (see the protocol): the shared textures have no UAV flag, which keeps them plain for the game's Vulkan
        // import, the upscaler and the frame generation write into uavs, which are copied into the shared textures.
        // The shared fences are then the game's semaphores, or without them (cpuSync) the GPU work is waited for on the CPU.
        bool wine = false;
        bool cpuSync = false;
        std::array<ComPtr<ID3D12Resource>, static_cast<size_t>(Protocol::Texture::Count)> uavs;

        ComPtr<IDXGIFactory4> factory;      // also makes the swap chain of the presentation

        bool Create(LUID luid)
        {
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
                return false;
            if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
                return false;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                return false;

            if (!direct.Create(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT))
                return false;
            queue = direct.queue;
            list = direct.list;
            if (!compute.Create(device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE))
            {
                Log("No compute queue: the frame generation runs on the direct queue");
                compute = {};
            }

            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence))) ||
                FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&generatedFence))))
                return false;

            CreateTimestamps();
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return fenceEvent != nullptr;
        }

        bool WaitFence(ID3D12Fence* fence, uint64_t value, DWORD timeout)
        {
            if (fence->GetCompletedValue() >= value)
                return true;
            fence->SetEventOnCompletion(value, fenceEvent);
            return WaitForSingleObject(fenceEvent, timeout) == WAIT_OBJECT_0;
        }

        // Command list for immediate work, e.g. feature creation, waited for on the CPU
        ID3D12GraphicsCommandList* BeginImmediate()
        {
            WaitFence(localFence.Get(), localValue, 5000);
            direct.allocators[0]->Reset();
            list->Reset(direct.allocators[0].Get(), nullptr);
            return list.Get();
        }

        bool SubmitImmediate()
        {
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(localFence.Get(), ++localValue);
            return WaitFence(localFence.Get(), localValue, 10000);
        }

        // Command list of one frame
        ID3D12GraphicsCommandList* BeginFrame(Work work)
        {
            current = work == Work::Generate && asyncGeneration && compute.queue ? &compute : &direct;
            auto& r = *current;
            r.frame = (r.frame + 1) % Frames;
            auto fence = r.allocatorFences[r.frame] ? r.allocatorFences[r.frame] : FrameFence();
            if (fence)
                WaitFence(fence, r.allocatorValues[r.frame], 1000);
            // Timestamps the GPU wrote: only once it is past the frame
            if (r.stampCounts[r.frame] && fence && fence->GetCompletedValue() >= r.allocatorValues[r.frame])
                ReadStamps(r, r.frame);
            r.stampCounts[r.frame] = 0;
            r.stampWork[r.frame] = work;
            r.allocators[r.frame]->Reset();
            r.list->Reset(r.allocators[r.frame].Get(), nullptr);
            return r.list.Get();
        }

        // The fence the frames' allocators are tracked with
        ID3D12Fence* FrameFence()
        {
            return cpuSync ? localFence.Get() : outputFence.Get();
        }

        // Wine: runs the frame and waits for it, the game copies the output once Evaluate is answered
        bool SubmitFrameAndWait()
        {
            auto& r = *current;
            ResolveStamps();
            if (FAILED(r.list->Close()))
                return false;
            ID3D12CommandList* lists[] = { r.list.Get() };
            r.queue->ExecuteCommandLists(1, lists);
            r.queue->Signal(localFence.Get(), ++localValue);
            r.allocatorValues[r.frame] = localValue;
            r.allocatorFences[r.frame] = nullptr;
            return WaitFence(localFence.Get(), localValue, 2000);
        }

        void SubmitFrame(uint64_t waitValue, uint64_t signalValue, bool execute)
        {
            auto& r = *current;
            ResolveStamps();
            r.list->Close();
            r.queue->Wait(inputFence.Get(), waitValue);
            if (execute)
            {
                ID3D12CommandList* lists[] = { r.list.Get() };
                r.queue->ExecuteCommandLists(1, lists);
            }
            // The output fence only goes up: the value given to the other queue before this one is signalled first, or
            // this one's would let the game take that queue's frame before it is ready. The work itself still overlaps.
            // Only this process signals it, so the wait can't be passed by a value of the game's.
            auto& other = &r == &direct ? compute : direct;
            if (other.queue && other.lastSignal)
                r.queue->Wait(outputFence.Get(), other.lastSignal);
            r.queue->Signal(outputFence.Get(), signalValue);
            r.lastSignal = signalValue;
            r.allocatorValues[r.frame] = signalValue;
            r.allocatorFences[r.frame] = nullptr;
        }

        // Generate for the presentation: once the game's copies are in, signals generatedFence, which the present thread
        // waits for; the game waits for nothing of it. Returns the value.
        uint64_t SubmitGenerated(uint64_t waitValue, bool execute)
        {
            auto& r = *current;
            ResolveStamps();
            r.list->Close();
            r.queue->Wait(inputFence.Get(), waitValue);
            if (execute)
            {
                ID3D12CommandList* lists[] = { r.list.Get() };
                r.queue->ExecuteCommandLists(1, lists);
            }
            r.queue->Signal(generatedFence.Get(), ++generatedValue);
            r.allocatorValues[r.frame] = generatedValue;
            r.allocatorFences[r.frame] = generatedFence.Get();
            return generatedValue;
        }

        void ReleaseTextures()
        {
            // The game has already imported or dropped its duplicates
            if (auto fence = FrameFence())
                for (auto ring : { &direct, &compute })
                    WaitFence(fence, *std::max_element(ring->allocatorValues.begin(), ring->allocatorValues.end()), 1000);
            for (auto& texture : textures)
            {
                if (texture.handle)
                    CloseHandle(texture.handle);
                texture = {};
            }
            for (auto& uav : uavs)
                uav.Reset();
            for (auto handle : { &inputFenceHandle, &outputFenceHandle })
            {
                if (*handle)
                    CloseHandle(*handle);
                *handle = nullptr;
            }
            if (presentFenceHandle)
                CloseHandle(presentFenceHandle);
            presentFenceHandle = nullptr;
            inputFence.Reset();
            outputFence.Reset();
            presentFence.Reset();
            for (auto ring : { &direct, &compute })
            {
                ring->allocatorValues.fill(0);
                ring->allocatorFences.fill(nullptr);
                ring->lastSignal = 0;
            }
        }

        // A shader can write the format through a typed UAV
        bool CanWrite(DXGI_FORMAT format)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ format };
            return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        }

        bool CreateTexture(Protocol::Texture index, uint32_t width, uint32_t height, DXGI_FORMAT format, bool unorderedAccess)
        {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = unorderedAccess && !wine ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

            auto& texture = textures[static_cast<size_t>(index)];
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture.resource))))
                return false;
            // vkd3d-proton only makes textures shareable with another process: a buffer gets an empty handle
            if (FAILED(device->CreateSharedHandle(texture.resource.Get(), nullptr, GENERIC_ALL, nullptr, &texture.handle)) || !texture.handle)
                return false;

            if (wine && unorderedAccess)
            {
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&uavs[static_cast<size_t>(index)]))))
                    return false;
            }
            return true;
        }

        void ReleaseTexture(Protocol::Texture index)
        {
            auto& texture = textures[static_cast<size_t>(index)];
            if (texture.handle)
                CloseHandle(texture.handle);
            texture = {};
            uavs[static_cast<size_t>(index)].Reset();
        }

        uint64_t AllocationSize(Protocol::Texture index)
        {
            auto& resource = textures[static_cast<size_t>(index)].resource;
            if (!resource)
                return 0;
            auto desc = resource->GetDesc();
            return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        }

        // Wine: what the upscaler or the frame generation wrote into the shared texture, both in the common state
        // before and after
        void CopyFromUav(ID3D12GraphicsCommandList* cmd, Protocol::Texture index)
        {
            auto shared = textures[static_cast<size_t>(index)].resource.Get();
            auto uav = uavs[static_cast<size_t>(index)].Get();
            D3D12_RESOURCE_BARRIER barriers[2]{};
            for (auto& barrier : barriers)
            {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            }
            barriers[0].Transition.pResource = uav;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[1].Transition.pResource = shared;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmd->ResourceBarrier(2, barriers);
            cmd->CopyResource(shared, uav);
            for (auto& barrier : barriers)
            {
                barrier.Transition.StateBefore = barrier.Transition.StateAfter;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            }
            cmd->ResourceBarrier(2, barriers);
        }

        bool CreateSharedFence(ComPtr<ID3D12Fence>& fence, HANDLE& handle)
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))))
                return false;
            return SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
        }

        bool CreateSharedFences()
        {
            return CreateSharedFence(inputFence, inputFenceHandle) && CreateSharedFence(outputFence, outputFenceHandle);
        }

        // Both queues done with everything submitted so far: before what their work uses goes, the upscaler's and the
        // frame generation's contexts among it (a Generate the game dropped is waited for by nobody else)
        void Flush()
        {
            if (!localFence)
                return;
            for (auto& q : { direct.queue, compute.queue })
            {
                if (q)
                {
                    q->Signal(localFence.Get(), ++localValue);
                    WaitFence(localFence.Get(), localValue, 2000);
                }
            }
        }

        // What the upscaler and the frame generation read and write
        ID3D12Resource* Texture(Protocol::Texture index)
        {
            if (auto& uav = uavs[static_cast<size_t>(index)])
                return uav.Get();
            return textures[static_cast<size_t>(index)].resource.Get();
        }

        // The textures of one pass from the common state to their use, or back
        void Transition(ID3D12GraphicsCommandList* cmd, bool toUse, std::initializer_list<Protocol::Texture> indices)
        {
            D3D12_RESOURCE_BARRIER barriers[static_cast<size_t>(Protocol::Texture::Count)]{};
            UINT count = 0;
            for (auto index : indices)
            {
                auto resource = Texture(index);
                if (!resource)
                    continue;
                auto written = index == Protocol::Texture::Output || index == Protocol::Texture::Generated;
                auto use = written ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                auto& barrier = barriers[count++];
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resource;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = toUse ? D3D12_RESOURCE_STATE_COMMON : use;
                barrier.Transition.StateAfter = toUse ? use : D3D12_RESOURCE_STATE_COMMON;
            }
            if (count)
                cmd->ResourceBarrier(count, barriers);
        }
    };

    // -----------------------------------------------------------------------------------------------
    // Frame parameters

    struct FrameParams
    {
        uint32_t width = 0;           // render size
        uint32_t height = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float motionScaleX = 1.0f;
        float motionScaleY = 1.0f;
        float cameraNear = 0.1f;
        float cameraFar = 1000.0f;
        float cameraFovY = 1.0f;
        float frameTimeMs = 16.6f;
        float sharpness = 0.0f;
        bool reset = false;
        bool reactive = false;
        // Frame generation
        float cameraPosition[3]{};
        float cameraUp[3]{};
        float cameraRight[3]{};
        float cameraForward[3]{};
        uint64_t frameId = 0;
        bool hudLess = false;
    };

    // -----------------------------------------------------------------------------------------------
    // NVIDIA DLSS

    class DLSS
    {
        ID3D12Device* device = nullptr;
        NVSDK_NGX_Parameter* capabilities = nullptr;
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        bool initialized = false;

        // NGX keeps using the feature search paths after initialization
        std::vector<std::wstring> paths;
        std::vector<const wchar_t*> pathPointers;

    public:
        bool available = false;

        bool Init(ID3D12Device* d3d, const std::vector<std::wstring>& searchPaths, const std::wstring& dataPath, std::string& message)
        {
            device = d3d;

            paths = searchPaths;
            pathPointers.clear();
            for (auto& path : paths)
                pathPointers.push_back(path.c_str());

            NVSDK_NGX_FeatureCommonInfo info{};
            info.PathListInfo.Path = pathPointers.data();
            info.PathListInfo.Length = static_cast<unsigned int>(pathPointers.size());

            // Identifies this integration to NGX, a project id is only needed for NVIDIA-registered titles
            auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("1b4f6c2e-9a3d-4e57-8c21-6f0d3b7a9e14", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                dataPath.c_str(), device, &info, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result))
            {
                message = "NGX initialization failed: " + std::to_string(static_cast<uint32_t>(result));
                return false;
            }
            initialized = true;

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities)) || !capabilities)
            {
                message = "NGX capability parameters are not available";
                return false;
            }

            int supported = 0;
            int needsUpdatedDriver = 0;
            NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
            auto queried = NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
            if (NVSDK_NGX_FAILED(queried) || !supported)
            {
                message = needsUpdatedDriver ? "DLSS needs a newer NVIDIA driver" : "DLSS is not supported: no RTX GPU or nvngx_dlss.dll not found";
                return false;
            }

            if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&parameters)) || !parameters)
            {
                message = "NGX parameters could not be allocated";
                return false;
            }

            available = true;
            message = "DLSS available";
            return true;
        }

        // The quality mode nearest to the render scale; DLAA when nothing is upscaled
        static NVSDK_NGX_PerfQuality_Value PerfQuality(uint32_t width, uint32_t outputWidth)
        {
            if (width >= outputWidth)
                return NVSDK_NGX_PerfQuality_Value_DLAA;
            auto scale = static_cast<float>(width) / static_cast<float>(outputWidth);
            if (scale >= 0.62f)
                return NVSDK_NGX_PerfQuality_Value_MaxQuality;
            if (scale >= 0.54f)
                return NVSDK_NGX_PerfQuality_Value_Balanced;
            if (scale >= 0.42f)
                return NVSDK_NGX_PerfQuality_Value_MaxPerf;
            return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }

        bool Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight, uint32_t preset)
        {
            Release();

            // The preset applies to whichever quality mode the scale picks
            for (auto hint : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
                NVSDK_NGX_Parameter_SetUI(parameters, hint, preset);

            NVSDK_NGX_DLSS_Create_Params create{};
            create.Feature.InWidth = width;
            create.Feature.InHeight = height;
            create.Feature.InTargetWidth = outputWidth;
            create.Feature.InTargetHeight = outputHeight;
            create.Feature.InPerfQualityValue = PerfQuality(width, outputWidth);
            // HDR scene color, motion vectors at render resolution without jitter, standard depth
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            create.InEnableOutputSubrects = false;

            auto result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &feature, parameters, &create);
            if (NVSDK_NGX_FAILED(result))
            {
                Log("DLSS feature creation failed: 0x%08X", static_cast<uint32_t>(result));
                feature = nullptr;
                return false;
            }
            return true;
        }

        bool Evaluate(ID3D12GraphicsCommandList* cmd, Device& d, const FrameParams& frame)
        {
            if (!feature)
                return false;

            NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
            eval.Feature.pInColor = d.Texture(Protocol::Texture::Color);
            eval.Feature.pInOutput = d.Texture(Protocol::Texture::Output);
            eval.pInDepth = d.Texture(Protocol::Texture::Depth);
            eval.pInMotionVectors = d.Texture(Protocol::Texture::Motion);
            eval.InJitterOffsetX = frame.jitterX;
            eval.InJitterOffsetY = frame.jitterY;
            eval.InRenderSubrectDimensions = { frame.width, frame.height };
            eval.InReset = frame.reset ? 1 : 0;
            eval.InMVScaleX = frame.motionScaleX;
            eval.InMVScaleY = frame.motionScaleY;
            eval.InPreExposure = 1.0f;
            eval.InExposureScale = 1.0f;
            eval.InFrameTimeDeltaInMsec = frame.frameTimeMs;

            auto result = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, feature, parameters, &eval);
            if (NVSDK_NGX_FAILED(result))
            {
                Log("DLSS evaluation failed: 0x%08X", static_cast<uint32_t>(result));
                return false;
            }
            return true;
        }

        void Release()
        {
            if (feature)
                NVSDK_NGX_D3D12_ReleaseFeature(feature);
            feature = nullptr;
        }

        void Shutdown()
        {
            Release();
            if (parameters)
                NVSDK_NGX_D3D12_DestroyParameters(parameters);
            if (capabilities)
                NVSDK_NGX_D3D12_DestroyParameters(capabilities);
            parameters = nullptr;
            capabilities = nullptr;
            if (initialized)
                NVSDK_NGX_D3D12_Shutdown1(device);
            initialized = false;
        }
    };

    // -----------------------------------------------------------------------------------------------
    // AMD FSR, through the FidelityFX API runtime supplied by the user

    class FSR
    {
        HMODULE module = nullptr;
        ffxFunctions functions{};
        ffxContext context = nullptr;
        ffxContext frameGeneration = nullptr;
        ID3D12Device* device = nullptr;
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;

        // The versions of an effect the runtime has for this GPU, logged
        uint64_t LogVersions(uint64_t createDescType, const char* effect)
        {
            uint64_t count = 0;
            ffxQueryDescGetVersions versions{};
            versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            versions.createDescType = createDescType;
            versions.device = device;
            versions.outputCount = &count;
            if (functions.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0)
                return 0;

            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            if (functions.Query(nullptr, &versions.header) == FFX_API_RETURN_OK)
                for (uint64_t i = 0; i < count; ++i)
                    Log("%s version available: %s", effect, names[i] ? names[i] : "?");
            return count;
        }

    public:
        bool available = false;
        bool frameGenerationAvailable = false;

        bool Init(ID3D12Device* d3d, const std::vector<std::wstring>& searchPaths, std::string& message)
        {
            device = d3d;

            // FidelityFX SDK 2.x ships a small loader next to the effect libraries, 1.1 a single library
            for (auto& dir : searchPaths)
            {
                for (auto name : { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_dx12.dll" })
                {
                    auto path = std::filesystem::path(dir) / name;
                    if (!std::filesystem::exists(path))
                        continue;
                    // The loader loads amd_fidelityfx_upscaler_dx12.dll by name later, which has to find it next to the
                    // loader and not only in the helper's folder
                    SetDllDirectoryW(dir.c_str());
                    AddDllDirectory(dir.c_str());
                    module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    if (module)
                    {
                        Log("FidelityFX runtime: %ls", path.c_str());
                        break;
                    }
                }
                if (module)
                    break;
            }

            if (!module)
            {
                message = "FSR is not available: amd_fidelityfx_loader_dx12.dll not found";
                return false;
            }

            ffxLoadFunctions(&functions, module);
            if (!functions.CreateContext || !functions.DestroyContext || !functions.Query || !functions.Dispatch)
            {
                message = "FSR is not available: the FidelityFX runtime has no FidelityFX API exports";
                return false;
            }

            if (LogVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, "FSR") == 0)
            {
                message = "FSR is not available: the FidelityFX runtime has no upscaler for this GPU";
                return false;
            }

            // amd_fidelityfx_framegeneration_dx12.dll next to the loader
            frameGenerationAvailable = LogVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, "Frame generation") != 0;
            if (!frameGenerationAvailable)
                Log("Frame generation is not available: no frame generation in the FidelityFX runtime");

            available = true;
            message = "FSR available";
            return true;
        }

        bool Create(uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight)
        {
            Release();

            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = device;

            ffxCreateContextDescUpscale create{};
            create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            create.header.pNext = &backend.header;
            create.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
            create.maxRenderSize = { width, height };
            create.maxUpscaleSize = { outputWidth, outputHeight };

            auto result = functions.CreateContext(&context, &create.header, nullptr);
            if (result != FFX_API_RETURN_OK)
            {
                Log("FSR context creation failed: %u", result);
                context = nullptr;
                return false;
            }

            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (functions.Query(&context, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
                Log("FSR provider: %s", provider.versionName);
            return true;
        }

        bool Evaluate(ID3D12GraphicsCommandList* cmd, Device& d, const FrameParams& frame)
        {
            if (!context)
                return false;

            ffxDispatchDescUpscale dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dispatch.commandList = cmd;
            dispatch.color = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Color), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.depth = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Depth), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.motionVectors = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Motion), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            if (frame.reactive)
                dispatch.reactive = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Reactive), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.output = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Output), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.jitterOffset = { frame.jitterX, frame.jitterY };
            dispatch.motionVectorScale = { frame.motionScaleX, frame.motionScaleY };
            dispatch.renderSize = { frame.width, frame.height };
            dispatch.upscaleSize = { frame.outputWidth, frame.outputHeight };
            dispatch.enableSharpening = frame.sharpness > 0.0f;
            dispatch.sharpness = frame.sharpness;
            dispatch.frameTimeDelta = frame.frameTimeMs;
            dispatch.preExposure = 1.0f;
            dispatch.reset = frame.reset;
            dispatch.cameraNear = frame.cameraNear;
            dispatch.cameraFar = frame.cameraFar;
            dispatch.cameraFovAngleVertical = frame.cameraFovY;
            dispatch.viewSpaceToMetersFactor = 1.0f;

            auto result = functions.Dispatch(&context, &dispatch.header);
            if (result != FFX_API_RETURN_OK)
            {
                Log("FSR dispatch failed: %u", result);
                return false;
            }
            return true;
        }

        void Release()
        {
            if (context)
                functions.DestroyContext(&context, nullptr);
            context = nullptr;
        }

        // -------------------------------------------------------------------------------------------
        // Frame generation, without AMD's swap chain: the game presents the frames itself

        bool CreateFrameGeneration(uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight, bool hdr, bool eightBit)
        {
            ReleaseFrameGeneration();
            if (!frameGenerationAvailable)
                return false;

            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = device;

            ffxCreateContextDescFrameGenerationVersion version{};
            version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
            version.header.pNext = &backend.header;
            version.version = FFX_FRAMEGENERATION_VERSION;

            // Standard depth, motion vectors at the render size without jitter
            ffxCreateContextDescFrameGeneration create{};
            create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
            create.header.pNext = &version.header;
            create.flags = hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
            create.displaySize = { outputWidth, outputHeight };
            create.maxRenderSize = { width, height };
            create.backBufferFormat = eightBit ? FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM : FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;

            auto result = functions.CreateContext(&frameGeneration, &create.header, nullptr);
            if (result != FFX_API_RETURN_OK)
            {
                Log("Frame generation context creation failed: %u", result);
                frameGeneration = nullptr;
                return false;
            }
            displayWidth = outputWidth;
            displayHeight = outputHeight;

            ffxQueryGetProviderVersion provider{};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (functions.Query(&frameGeneration, &provider.header) == FFX_API_RETURN_OK && provider.versionName)
                Log("Frame generation provider: %s", provider.versionName);
            return true;
        }

        bool HasFrameGeneration() const
        {
            return frameGeneration != nullptr;
        }

        // The configuration and the preparation of one frame, with its depth and motion vectors. Must come before
        // GenerateFrame of the same frame.
        bool PrepareFrame(ID3D12GraphicsCommandList* cmd, Device& d, const FrameParams& frame)
        {
            if (!frameGeneration)
                return false;

            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.swapChain = nullptr;
            configure.frameGenerationEnabled = true;
            configure.allowAsyncWorkloads = false;
            configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
            configure.generationRect = { 0, 0, static_cast<int32_t>(displayWidth), static_cast<int32_t>(displayHeight) };
            configure.frameID = frame.frameId;
            // Written by Generate of this frame, read by its dispatch: the HUD is what differs from Present
            if (frame.hudLess)
                configure.HUDLessColor = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::HudLess), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            auto result = functions.Configure(&frameGeneration, &configure.header);
            if (result != FFX_API_RETURN_OK)
            {
                static uint32_t reported = 0;
                if (reported++ < 5)
                    Log("Frame generation configure failed: %u", result);
                return false;
            }

            ffxDispatchDescFrameGenerationPrepareV2 prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prepare.frameID = frame.frameId;
            prepare.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
            prepare.commandList = cmd;
            prepare.renderSize = { frame.width, frame.height };
            prepare.jitterOffset = { frame.jitterX, frame.jitterY };
            prepare.motionVectorScale = { frame.motionScaleX, frame.motionScaleY };
            prepare.frameTimeDelta = frame.frameTimeMs;
            prepare.reset = frame.reset;
            prepare.cameraNear = frame.cameraNear;
            prepare.cameraFar = frame.cameraFar;
            prepare.cameraFovAngleVertical = frame.cameraFovY;
            prepare.viewSpaceToMetersFactor = 1.0f;
            prepare.depth = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Depth), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            prepare.motionVectors = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Motion), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            for (int i = 0; i < 3; ++i)
            {
                prepare.cameraPosition[i] = frame.cameraPosition[i];
                prepare.cameraUp[i] = frame.cameraUp[i];
                prepare.cameraRight[i] = frame.cameraRight[i];
                prepare.cameraForward[i] = frame.cameraForward[i];
            }

            result = functions.Dispatch(&frameGeneration, &prepare.header);
            if (result != FFX_API_RETURN_OK)
            {
                static uint32_t reported = 0;
                if (reported++ < 5)
                    Log("Frame generation prepare failed: %u", result);
                return false;
            }
            return true;
        }

        // The frame between the previous Present and this one into Generated
        bool GenerateFrame(ID3D12GraphicsCommandList* cmd, Device& d, uint64_t frameId, bool reset, bool hdr, float maxLuminance)
        {
            if (!frameGeneration)
                return false;

            ffxDispatchDescFrameGeneration dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
            dispatch.commandList = cmd;
            dispatch.presentColor = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Present), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.outputs[0] = ffxApiGetResourceDX12(d.Texture(Protocol::Texture::Generated), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.numGeneratedFrames = 1;
            dispatch.reset = reset;
            dispatch.backbufferTransferFunction = hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            dispatch.minMaxLuminance[0] = 0.0f;
            dispatch.minMaxLuminance[1] = maxLuminance;
            dispatch.generationRect = { 0, 0, static_cast<int32_t>(displayWidth), static_cast<int32_t>(displayHeight) };
            dispatch.frameID = frameId;

            auto result = functions.Dispatch(&frameGeneration, &dispatch.header);
            if (result != FFX_API_RETURN_OK)
            {
                static uint32_t reported = 0;
                if (reported++ < 5)
                    Log("Frame generation dispatch failed: %u", result);
                return false;
            }
            return true;
        }

        void ReleaseFrameGeneration()
        {
            if (frameGeneration)
                functions.DestroyContext(&frameGeneration, nullptr);
            frameGeneration = nullptr;
        }

        void Shutdown()
        {
            ReleaseFrameGeneration();
            Release();
            if (module)
                FreeLibrary(module);
            module = nullptr;
        }
    };

    // -----------------------------------------------------------------------------------------------
    // Presentation (ConfigureFlags::HelperPresent)
    //
    // A child window of this process over the game's window, made on a thread of its own that pumps its messages: a
    // window in another process's window tree that leaves them unanswered holds that process up. It takes no input:
    // disabled, it is passed over by the hit testing, so the game's window keeps the mouse and the keyboard (as the
    // swap chains of Chromium's GPU process over the browser's window). The game hides it when it presents a frame itself.
    //
    // A flip model swap chain on it, on a queue of its own, presents from a thread of its own, so neither the requests nor
    // the game wait for it: per frame the generated frame when there is one, and PresentDelay of the frame time after it
    // the rendered one, sooner when the next frame comes first. Its frame latency waitable object paces the copies.

    class Display
    {
        HANDLE windowThread = nullptr;
        HANDLE ready = nullptr;
        HWND parent = nullptr;

        static constexpr uint32_t BufferCount = 3;
        std::array<ComPtr<ID3D12Resource>, BufferCount> buffers;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool tearing = false;
        HANDLE latency = nullptr;

        // The present thread's queue (the swap chain's), command lists and fence
        ID3D12Device* device = nullptr;
        ComPtr<ID3D12CommandQueue> queue;
        static constexpr uint32_t Lists = 4;
        std::array<ComPtr<ID3D12CommandAllocator>, Lists> allocators;
        std::array<uint64_t, Lists> allocatorValues{};
        uint32_t allocatorIndex = 0;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        uint64_t fenceValue = 0;
        HANDLE fenceEvent = nullptr;

        // What a frame presents from, set while the textures exist
        ID3D12Resource* presentTexture = nullptr;
        ID3D12Resource* generatedTexture = nullptr;
        ID3D12Fence* inputFence = nullptr;
        ID3D12Fence* presentFence = nullptr;
        ID3D12Fence* generatedFence = nullptr;

    public:
        struct Job
        {
            uint64_t inputValue = 0;        // the game's copies into Present and HudLess, on the input fence
            uint64_t presentValue = 0;      // on the present fence once both are free
            uint64_t generatedValue = 0;    // Generate of the frame on generatedFence, 0 without
            bool showGenerated = false;     // the generated frame is shown first
            uint32_t syncInterval = 1;
            float delay = 0.5f;
            LARGE_INTEGER arrived{};        // when the game asked, for the frame time
        };

    private:
        std::thread thread;
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Job> jobs;
        bool stopping = false;
        bool busy = false;
        bool hidden = false;                // Hide: the game presents itself, until the next job

        // Pacing, the present thread's
        double frameMs = 0.0;               // smoothed time between two frames' jobs
        LARGE_INTEGER lastJob{};
        LARGE_INTEGER frequency{};

        // Logged every 300 frames
        struct Stats
        {
            uint32_t frames = 0, generated = 0, cut = 0, failures = 0;
            double presentMs = 0.0, presentMax = 0.0, latencyMs = 0.0, gapMs = 0.0, aimMs = 0.0;
        } stats;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST: return HTTRANSPARENT;
            case WM_ERASEBKGND: return 1;
            case WM_DESTROY: PostQuitMessage(0); return 0;
            default: return DefWindowProcW(hwnd, message, wParam, lParam);
            }
        }

        static DWORD WINAPI WindowThread(void* parameter)
        {
            auto& d = *static_cast<Display*>(parameter);
            WNDCLASSEXW windowClass{ sizeof(windowClass) };
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"GTAIV.EFLC.FusionFix.Present";
            RegisterClassExW(&windowClass);

            RECT client{};
            GetClientRect(d.parent, &client);
            d.window = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, windowClass.lpszClassName, L"",
                WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1), d.parent, nullptr,
                windowClass.hInstance, nullptr);
            if (!d.window)
                Log("Present: the child window could not be made: error %lu", GetLastError());
            SetEvent(d.ready);
            if (!d.window)
                return 1;

            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return 0;
        }

        double Now()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        }

        void WaitLocal(uint64_t value, DWORD timeout)
        {
            if (value && fence->GetCompletedValue() < value && SUCCEEDED(fence->SetEventOnCompletion(value, fenceEvent)))
                WaitForSingleObject(fenceEvent, timeout);
        }

        // The window follows the size of the game's window's client area; the swap chain stretches into it
        void FollowParent()
        {
            RECT client{}, own{};
            if (!GetClientRect(parent, &client) || !GetClientRect(window, &own))
                return;
            if (client.right != own.right || client.bottom != own.bottom)
                SetWindowPos(window, nullptr, 0, 0, std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1),
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }

        // source into the next back buffer once waitFence reaches waitValue, then presented; signalFence reaches
        // signalValue after the copy. Returns the value of the own fence the copy is done at.
        uint64_t Show(ID3D12Resource* source, ID3D12Fence* waitFence, uint64_t waitValue, ID3D12Fence* signalFence, uint64_t signalValue, uint32_t syncInterval)
        {
            if (latency)
            {
                auto start = Now();
                WaitForSingleObjectEx(latency, 100, TRUE);
                stats.latencyMs += Now() - start;
            }
            allocatorIndex = (allocatorIndex + 1) % Lists;
            WaitLocal(allocatorValues[allocatorIndex], 1000);
            allocators[allocatorIndex]->Reset();
            list->Reset(allocators[allocatorIndex].Get(), nullptr);

            auto back = buffers[swapChain->GetCurrentBackBufferIndex()].Get();
            D3D12_RESOURCE_BARRIER barriers[2]{};
            for (auto& barrier : barriers)
            {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            barriers[0].Transition.pResource = back;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[1].Transition.pResource = source;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(2, barriers);
            list->CopyResource(back, source);
            for (auto& barrier : barriers)
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(2, barriers);
            list->Close();

            queue->Wait(waitFence, waitValue);
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            if (signalFence)
                queue->Signal(signalFence, signalValue);
            queue->Signal(fence.Get(), ++fenceValue);
            allocatorValues[allocatorIndex] = fenceValue;
            auto copied = fenceValue;

            auto start = Now();
            auto hr = swapChain->Present(syncInterval, syncInterval == 0 && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
            auto ms = Now() - start;
            stats.presentMs += ms;
            stats.presentMax = std::max(stats.presentMax, ms);
            if (FAILED(hr) && stats.failures++ < 5)
                Log("Present failed: 0x%08X", static_cast<uint32_t>(hr));
            // Shown with its first frame, and again after Hide with the next Present: not by one queued before Hide
            if (SUCCEEDED(hr))
            {
                std::lock_guard lock(mutex);
                if (!hidden && !IsWindowVisible(window))
                    ShowWindowAsync(window, SW_SHOWNA);
            }
            return copied;
        }

        void Process(const Job& job)
        {
            // Without a swap chain the textures are let go all the same, or the game waits for them
            if (!swapChain || !presentTexture)
            {
                queue->Wait(inputFence, job.inputValue);
                if (job.generatedValue)
                    queue->Wait(generatedFence, job.generatedValue);
                queue->Signal(presentFence, job.presentValue);
                return;
            }
            FollowParent();

            if (lastJob.QuadPart)
            {
                auto frame = static_cast<double>(job.arrived.QuadPart - lastJob.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
                if (frame > 0.0 && frame < 250.0)
                    frameMs = frameMs > 0.0 ? frameMs + (frame - frameMs) * 0.1 : frame;
            }
            lastJob = job.arrived;

            if (job.showGenerated && generatedTexture && frameMs > 0.0)
            {
                auto copied = Show(generatedTexture, generatedFence, job.generatedValue, nullptr, 0, job.syncInterval);
                // From when the generated frame reached the swap chain, cut short by the next frame
                WaitLocal(copied, 100);
                auto shownAt = Now();
                auto aim = std::clamp(frameMs * job.delay, 0.0, 50.0);
                bool cut = false;
                {
                    std::unique_lock lock(mutex);
                    auto remaining = aim - (Now() - shownAt);
                    if (remaining > 1.5)
                        cut = changed.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>((remaining - 1.0) * 1000.0)),
                            [&] { return stopping || !jobs.empty(); });
                }
                // The last millisecond spun, the sleep is coarser
                while (!cut && Now() - shownAt < aim)
                {
                    std::lock_guard lock(mutex);
                    cut = stopping || !jobs.empty();
                }
                ++stats.generated;
                stats.cut += cut;
                stats.gapMs += Now() - shownAt;
                stats.aimMs += aim;
                // The rendered frame after the generated one's copy, which waited for Generate
                Show(presentTexture, fence.Get(), copied, presentFence, job.presentValue, job.syncInterval);
            }
            else
            {
                // The generation, when it ran, still read Present and HudLess
                if (job.generatedValue)
                    queue->Wait(generatedFence, job.generatedValue);
                Show(presentTexture, inputFence, job.inputValue, presentFence, job.presentValue, job.syncInterval);
            }

            if (++stats.frames >= 300)
            {
                auto& s = stats;
                Log("Present over %u frames (%.2f ms each, sync interval %u): %u generated frames shown, the rendered one %.2f ms after them "
                    "(aimed at %.2f, %u cut short by the next frame); Present %.3f ms of the CPU on average (%.3f at most), "
                    "the latency wait %.3f ms", s.frames, frameMs, job.syncInterval, s.generated, s.generated ? s.gapMs / s.generated : 0.0,
                    s.generated ? s.aimMs / s.generated : 0.0, s.cut, s.presentMs / (s.frames + s.generated), s.presentMax,
                    s.latencyMs / (s.frames + s.generated));
                auto failures = s.failures;
                s = {};
                s.failures = failures;
            }
        }

        void Run()
        {
            while (true)
            {
                Job job;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || !jobs.empty(); });
                    if (jobs.empty())
                        return;
                    job = jobs.front();
                    jobs.pop_front();
                    busy = true;
                }
                Process(job);
                {
                    std::lock_guard lock(mutex);
                    busy = false;
                }
                changed.notify_all();
            }
        }

        bool CreateQueue(ID3D12Device* d3d)
        {
            device = d3d;
            QueryPerformanceFrequency(&frequency);
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                return false;
            for (auto& allocator : allocators)
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
                    return false;
            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))))
                return false;
            list->Close();
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            thread = std::thread([this] { Run(); });
            return fenceEvent != nullptr;
        }

        void ReleaseSwapChain()
        {
            for (auto& buffer : buffers)
                buffer.Reset();
            if (latency)
                CloseHandle(latency);
            latency = nullptr;
            swapChain.Reset();
            width = height = 0;
            format = DXGI_FORMAT_UNKNOWN;
        }

    public:
        HWND window = nullptr;
        ComPtr<IDXGISwapChain3> swapChain;
        uint32_t width = 0;
        uint32_t height = 0;

        // The window over the game's, made once, and a swap chain of the size and format of Present. Drained, GPU idle.
        bool Configure(ComPtr<IDXGIFactory4>& factory, ID3D12Device* d3d, HWND gameWindow, uint32_t w, uint32_t h, DXGI_FORMAT f, bool hdr)
        {
            if (window && parent != gameWindow)
                Release();
            if (!queue && !CreateQueue(d3d))
            {
                Log("Present: the queue could not be made");
                return false;
            }
            if (!window)
            {
                if (!gameWindow || !IsWindow(gameWindow))
                {
                    Log("Present: the game's window %p is not a window", gameWindow);
                    return false;
                }
                parent = gameWindow;
                ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                windowThread = CreateThread(nullptr, 0, WindowThread, this, 0, nullptr);
                if (!windowThread || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 || !window)
                {
                    Release();
                    return false;
                }
                Log("Present: child window %p over the game's window %p", window, parent);
            }
            if (swapChain && width == w && height == h && format == f)
                return true;
            ReleaseSwapChain();

            ComPtr<IDXGIFactory5> factory5;
            BOOL allowTearing = FALSE;
            tearing = SUCCEEDED(factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))) && allowTearing;

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = w;
            desc.Height = h;
            desc.Format = f;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = BufferCount;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
            ComPtr<IDXGISwapChain1> swapChain1;
            auto hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &swapChain1);
            if (FAILED(hr) || FAILED(swapChain1.As(&swapChain)))
            {
                Log("Present: the swap chain could not be made (0x%08X) at %ux%u, format %d", static_cast<uint32_t>(hr), w, h, int(f));
                ReleaseSwapChain();
                return false;
            }
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
            if (hdr && FAILED(swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)))
                Log("Present: scRGB could not be set on the swap chain");
            // Two frames queued: the generated and the rendered one
            swapChain->SetMaximumFrameLatency(2);
            latency = swapChain->GetFrameLatencyWaitableObject();
            for (uint32_t i = 0; i < BufferCount; ++i)
            {
                if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))))
                {
                    ReleaseSwapChain();
                    return false;
                }
            }
            width = w;
            height = h;
            format = f;
            Log("Present: swap chain %ux%u, format %d%s%s", w, h, int(f), hdr ? ", scRGB" : "", tearing ? ", tearing allowed" : "");
            return true;
        }

        // The textures and fences the frames come with, while they exist; null ones at release
        void Bind(ID3D12Resource* present, ID3D12Resource* generated, ID3D12Fence* input, ID3D12Fence* presentDone, ID3D12Fence* generatedDone)
        {
            presentTexture = present;
            generatedTexture = generated;
            inputFence = input;
            presentFence = presentDone;
            generatedFence = generatedDone;
        }

        bool Ready() const
        {
            return queue && presentFence && inputFence;
        }

        void Post(Job job)
        {
            QueryPerformanceCounter(&job.arrived);
            {
                std::lock_guard lock(mutex);
                jobs.push_back(job);
                hidden = false;
            }
            changed.notify_all();
        }

        // Every job presented and the queue idle, before what they use goes
        void Drain()
        {
            if (!queue)
                return;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::seconds(2), [&] { return jobs.empty() && !busy; });
            }
            queue->Signal(fence.Get(), ++fenceValue);
            WaitLocal(fenceValue, 2000);
        }

        void Release()
        {
            Drain();
            Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
            if (thread.joinable())
            {
                {
                    std::lock_guard lock(mutex);
                    stopping = true;
                }
                changed.notify_all();
                thread.join();
            }
            ReleaseSwapChain();
            if (window)
                PostMessageW(window, WM_CLOSE, 0, 0);
            if (windowThread)
            {
                WaitForSingleObject(windowThread, 2000);
                CloseHandle(windowThread);
            }
            if (ready)
                CloseHandle(ready);
            windowThread = ready = nullptr;
            window = nullptr;
            parent = nullptr;
        }

        // The game presents itself (Hide): the window's show and hide messages are posted in this order under the lock
        void Hide()
        {
            std::lock_guard lock(mutex);
            hidden = true;
            if (window)
                ShowWindowAsync(window, SW_HIDE);
        }

        // Without the presentation: drained, hidden, its swap chain released
        void Stop()
        {
            Drain();
            Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
            if (window)
                ShowWindowAsync(window, SW_HIDE);
            ReleaseSwapChain();
        }
    };

    // -----------------------------------------------------------------------------------------------

    class Helper
    {
        Connection connection;
        Device device;
        DLSS dlss;
        FSR fsr;
        Display display;
        Protocol::Backend backend = Protocol::Backend::None;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t flags = 0;

        bool Duplicate(HANDLE source, uint64_t& target)
        {
            HANDLE duplicate = nullptr;
            if (!source || !DuplicateHandle(GetCurrentProcess(), source, connection.game, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                Log("Handle %p could not be duplicated into the game: error %lu", source, GetLastError());
                return false;
            }
            target = reinterpret_cast<uint64_t>(duplicate);
            return true;
        }

        // Wine: the game's timeline semaphores, duplicated into this process, as the shared fences. Without them GameFence
        // is cleared in the answer and both sides wait on the CPU.
        void OpenGameFence()
        {
            auto& shared = *connection.shared;
            HANDLE handles[] = { reinterpret_cast<HANDLE>(shared.InputFenceHandle), reinterpret_cast<HANDLE>(shared.OutputFenceHandle) };
            shared.InputFenceHandle = shared.OutputFenceHandle = 0;
            ComPtr<ID3D12Fence> fences[2];
            HRESULT hr = S_OK;
            for (int i = 0; i < 2; ++i)
            {
                if ((flags & Protocol::ConfigureFlags::GameFence) && SUCCEEDED(hr))
                    hr = handles[i] ? device.device->OpenSharedHandle(handles[i], IID_PPV_ARGS(&fences[i])) : E_INVALIDARG;
                if (handles[i])
                    CloseHandle(handles[i]);
            }
            if (!(flags & Protocol::ConfigureFlags::GameFence))
                return;

            if (SUCCEEDED(hr) && fences[0] && fences[1])
            {
                device.inputFence = fences[0];
                device.outputFence = fences[1];
                Log("The game's semaphores are the shared fences");
            }
            else
            {
                flags &= ~Protocol::ConfigureFlags::GameFence;
                Log("The game's semaphores could not be opened (0x%08X): waiting on the CPU", static_cast<uint32_t>(hr));
            }
            shared.Flags = flags;
        }

        bool Configure()
        {
            auto& shared = *connection.shared;

            // The GPU first: the contexts go before the textures, and nothing may still run with them, the frames waiting to
            // be presented included
            display.Drain();
            display.Bind(nullptr, nullptr, nullptr, nullptr, nullptr);
            device.Flush();
            dlss.Release();
            fsr.Release();
            fsr.ReleaseFrameGeneration();
            device.ReleaseTextures();
            backend = Protocol::Backend::None;

            width = shared.Width;
            height = shared.Height;
            flags = shared.Flags;
            outputWidth = shared.OutputWidth ? shared.OutputWidth : width;
            outputHeight = shared.OutputHeight ? shared.OutputHeight : height;
            Log("Configure %s at %ux%u -> %ux%u%s", shared.ConfigureBackend == Protocol::Backend::DLSS ? "DLSS" : "FSR", width, height,
                outputWidth, outputHeight, (flags & Protocol::ConfigureFlags::Wine) ? ", Wine" : "");
            if (width == 0 || height == 0 || outputWidth > 16384 || outputHeight > 16384 || outputWidth < width || outputHeight < height)
                return false;
            device.wine = (flags & Protocol::ConfigureFlags::Wine) != 0;
            OpenGameFence();
            device.cpuSync = device.wine && !(flags & Protocol::ConfigureFlags::GameFence);

            using T = Protocol::Texture;
            if (!device.CreateTexture(T::Color, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
                !device.CreateTexture(T::Depth, width, height, DXGI_FORMAT_R32_FLOAT, false) ||
                !device.CreateTexture(T::Motion, width, height, DXGI_FORMAT_R16G16_FLOAT, false) ||
                !device.CreateTexture(T::Reactive, width, height, DXGI_FORMAT_R16_FLOAT, false) ||
                !device.CreateTexture(T::Output, outputWidth, outputHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, true) ||
                (!device.wine && !device.CreateSharedFences()))
            {
                connection.Message("Shared textures could not be created");
                return false;
            }

            bool created = false;
            if (shared.ConfigureBackend == Protocol::Backend::DLSS && dlss.available)
            {
                auto cmd = device.BeginImmediate();
                created = dlss.Create(cmd, width, height, outputWidth, outputHeight, shared.DLSSPreset);
                created = device.SubmitImmediate() && created;
            }
            else if (shared.ConfigureBackend == Protocol::Backend::FSR && fsr.available)
            {
                created = fsr.Create(width, height, outputWidth, outputHeight);
            }

            if (!created)
            {
                connection.Message("The upscaler could not be created at %ux%u -> %ux%u", width, height, outputWidth, outputHeight);
                return false;
            }

            // Frame generation, with either upscaler. Without it the upscaler works on, and the flag is cleared.
            if (flags & Protocol::ConfigureFlags::FrameGeneration)
            {
                bool hdr = (flags & Protocol::ConfigureFlags::HighDynamicRange) != 0;
                // The frames as the back buffer has them, 8 bits without HDR, if the frame generation can write them
                if ((flags & Protocol::ConfigureFlags::EightBitFrames) && (hdr || !device.CanWrite(DXGI_FORMAT_B8G8R8A8_UNORM)))
                {
                    Log("B8G8R8A8_UNORM can't be written by a shader on this GPU: the frame generation's textures stay 16-bit float");
                    flags &= ~Protocol::ConfigureFlags::EightBitFrames;
                    shared.Flags = flags;
                }
                bool eightBit = (flags & Protocol::ConfigureFlags::EightBitFrames) != 0;
                if ((flags & Protocol::ConfigureFlags::AsyncGeneration) && !device.compute.queue)
                {
                    flags &= ~Protocol::ConfigureFlags::AsyncGeneration;
                    shared.Flags = flags;
                }
                auto format = eightBit ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
                bool generation = fsr.CreateFrameGeneration(width, height, outputWidth, outputHeight, hdr, eightBit) &&
                    device.CreateTexture(T::Present, outputWidth, outputHeight, format, false) &&
                    device.CreateTexture(T::Generated, outputWidth, outputHeight, format, true) &&
                    device.CreateTexture(T::HudLess, outputWidth, outputHeight, format, false);
                if (!generation)
                {
                    Log("Frame generation could not be set up at %ux%u -> %ux%u", width, height, outputWidth, outputHeight);
                    fsr.ReleaseFrameGeneration();
                    device.ReleaseTexture(T::Present);
                    device.ReleaseTexture(T::Generated);
                    device.ReleaseTexture(T::HudLess);
                    flags &= ~(Protocol::ConfigureFlags::FrameGeneration | Protocol::ConfigureFlags::EightBitFrames | Protocol::ConfigureFlags::AsyncGeneration);
                    shared.Flags = flags;
                }
            }

            // Presentation by the helper, the frame generation's frames too, whose Present it shares. Not under Wine, nor with
            // 16-bit frames without HDR (the frame generation could not write 8 bits): sRGB encoded, a 16-bit swap chain would
            // show them as linear.
            if (flags & Protocol::ConfigureFlags::HelperPresent)
            {
                bool hdr = (flags & Protocol::ConfigureFlags::HighDynamicRange) != 0;
                bool generation = fsr.HasFrameGeneration();
                bool eightBit = generation ? (flags & Protocol::ConfigureFlags::EightBitFrames) != 0 : !hdr;
                auto format = eightBit ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
                bool presenting = !device.wine && (hdr || eightBit) &&
                    (generation || device.CreateTexture(T::Present, outputWidth, outputHeight, format, false)) &&
                    device.CreateSharedFence(device.presentFence, device.presentFenceHandle) &&
                    display.Configure(device.factory, device.device.Get(), reinterpret_cast<HWND>(shared.PresentWindow), outputWidth, outputHeight, format, hdr);
                if (presenting)
                {
                    display.Bind(device.Texture(T::Present), generation ? device.Texture(T::Generated) : nullptr, device.inputFence.Get(),
                        device.presentFence.Get(), device.generatedFence.Get());
                    // The game's textures for Present are the back buffer's A8R8G8B8 without HDR
                    if (eightBit)
                        flags |= Protocol::ConfigureFlags::EightBitFrames;
                }
                else
                {
                    Log("Presentation could not be set up at %ux%u: the game presents", outputWidth, outputHeight);
                    if (!generation)
                        device.ReleaseTexture(T::Present);
                    device.presentFence.Reset();
                    if (device.presentFenceHandle)
                        CloseHandle(device.presentFenceHandle);
                    device.presentFenceHandle = nullptr;
                    display.Stop();
                    flags &= ~Protocol::ConfigureFlags::HelperPresent;
                }
                shared.Flags = flags;
            }
            else
            {
                display.Stop();
            }
            shared.ChildWindow = (flags & Protocol::ConfigureFlags::HelperPresent) ? reinterpret_cast<uint64_t>(display.window) : 0;

            for (size_t i = 0; i < static_cast<size_t>(T::Count); ++i)
            {
                shared.TextureSizes[i] = device.AllocationSize(static_cast<T>(i));
                shared.TextureHandles[i] = 0;
                if (!device.textures[i].handle)
                    continue;
                if (!Duplicate(device.textures[i].handle, shared.TextureHandles[i]))
                    return false;
            }
            shared.InputFenceHandle = shared.OutputFenceHandle = shared.PresentFenceHandle = 0;
            if (!device.wine && (!Duplicate(device.inputFenceHandle, shared.InputFenceHandle) || !Duplicate(device.outputFenceHandle, shared.OutputFenceHandle)))
                return false;
            if ((flags & Protocol::ConfigureFlags::HelperPresent) && !Duplicate(device.presentFenceHandle, shared.PresentFenceHandle))
                return false;

            backend = shared.ConfigureBackend;
            device.asyncGeneration = fsr.HasFrameGeneration() && (flags & Protocol::ConfigureFlags::AsyncGeneration) != 0;
            connection.Message("%s ready at %ux%u -> %ux%u%s%s", backend == Protocol::Backend::DLSS ? "DLSS" : "FSR", width, height, outputWidth, outputHeight,
                !fsr.HasFrameGeneration() ? "" : (flags & Protocol::ConfigureFlags::EightBitFrames) ? ", with frame generation (8-bit frames" : ", with frame generation (16-bit frames",
                (flags & Protocol::ConfigureFlags::AsyncGeneration) ? ", on the compute queue)" : fsr.HasFrameGeneration() ? ")" : "");
            return true;
        }

        bool Evaluate()
        {
            auto& shared = *connection.shared;
            if (backend == Protocol::Backend::None || (!device.cpuSync && (!device.inputFence || !device.outputFence)))
                return false;

            FrameParams frame;
            frame.width = width;
            frame.height = height;
            frame.outputWidth = outputWidth;
            frame.outputHeight = outputHeight;
            frame.jitterX = shared.JitterX;
            frame.jitterY = shared.JitterY;
            frame.motionScaleX = shared.MotionScaleX;
            frame.motionScaleY = shared.MotionScaleY;
            frame.cameraNear = shared.CameraNear;
            frame.cameraFar = shared.CameraFar;
            frame.cameraFovY = shared.CameraFovY;
            frame.frameTimeMs = shared.FrameTimeMs;
            frame.sharpness = shared.Sharpness;
            frame.reset = shared.Reset != 0;
            frame.reactive = (flags & Protocol::ConfigureFlags::ReactiveMask) != 0;
            for (int i = 0; i < 3; ++i)
            {
                frame.cameraPosition[i] = shared.CameraPosition[i];
                frame.cameraUp[i] = shared.CameraUp[i];
                frame.cameraRight[i] = shared.CameraRight[i];
                frame.cameraForward[i] = shared.CameraForward[i];
            }
            frame.frameId = shared.FrameId;
            frame.hudLess = shared.HudLess != 0;

            using T = Protocol::Texture;
            auto cmd = device.BeginFrame(Device::Work::Evaluate);
            device.Transition(cmd, true, { T::Color, T::Depth, T::Motion, T::Reactive, T::Output });
            device.Stamp(cmd);
            bool evaluated = backend == Protocol::Backend::DLSS ? dlss.Evaluate(cmd, device, frame) : fsr.Evaluate(cmd, device, frame);
            device.Stamp(cmd);
            // A failed preparation leaves the upscaled frame: Generate of this frame fails instead
            // Recorded into the same command list, which is not executed when the upscaler failed
            preparedFrame = fsr.HasFrameGeneration() && fsr.PrepareFrame(cmd, device, frame) && evaluated;
            if (fsr.HasFrameGeneration())
                device.Stamp(cmd);
            if (fsr.HasFrameGeneration())
            {
                ++generationStats.prepares;
                generationStats.prepareFailed += !preparedFrame;
                generationStats.prepareResets += frame.reset;
                generationStats.prepareGaps += preparedFrameId != 0 && frame.frameId != preparedFrameId + 1;
                generationStats.hudLess += frame.hudLess;
            }
            preparedFrameId = frame.frameId;
            device.Transition(cmd, false, { T::Color, T::Depth, T::Motion, T::Reactive, T::Output });

            if (device.wine)
                device.CopyFromUav(cmd, T::Output);

            // Answered once the output is in the shared texture
            if (device.cpuSync)
                return device.SubmitFrameAndWait() && evaluated;

            // The fence always advances, so the game can rely on the values it waits for
            device.SubmitFrame(shared.WaitValue, shared.SignalValue, evaluated);
            return evaluated;
        }

        bool preparedFrame = false;
        uint64_t preparedFrameId = 0;

        // What the frame generation was asked, logged every 300 Generate requests
        struct GenerationStats
        {
            uint32_t prepares = 0, prepareFailed = 0, prepareResets = 0, prepareGaps = 0, hudLess = 0;
            uint32_t generates = 0, unprepared = 0, resets = 0, failed = 0, gaps = 0;
            uint64_t lastId = 0;
        } generationStats;

        bool Generate()
        {
            auto& shared = *connection.shared;
            if (!fsr.HasFrameGeneration() || (!device.cpuSync && (!device.inputFence || !device.outputFence)))
                return false;

            // Only after the preparation of the same frame
            bool prepared = preparedFrame && preparedFrameId == shared.FrameId;
            preparedFrame = false;

            auto& g = generationStats;
            ++g.generates;
            g.unprepared += !prepared;
            g.resets += shared.GenerateReset != 0;
            g.gaps += g.lastId != 0 && shared.FrameId != g.lastId + 1;
            g.lastId = shared.FrameId;

            using T = Protocol::Texture;
            auto cmd = device.BeginFrame(Device::Work::Generate);
            bool generated = false;
            if (prepared)
            {
                device.Transition(cmd, true, { T::Present, T::Generated, T::HudLess });
                device.Stamp(cmd);
                generated = fsr.GenerateFrame(cmd, device, shared.FrameId, shared.GenerateReset != 0,
                    (flags & Protocol::ConfigureFlags::HighDynamicRange) != 0, shared.MaxLuminance);
                device.Stamp(cmd);
                g.failed += !generated;
                device.Transition(cmd, false, { T::Present, T::Generated, T::HudLess });
                if (device.wine)
                    device.CopyFromUav(cmd, T::Generated);
            }

            if (g.generates >= 300)
            {
                // Only when something was off: a reset now and then is a camera cut
                if (g.unprepared || g.failed || g.gaps || g.prepareFailed || g.prepareGaps || g.resets > 2)
                    Log("Frame generation over %u Generate: %u not prepared, %u reset, %u failed, %u with a gap in the frame numbers; "
                    "%u Prepare: %u failed, %u reset, %u with a gap, %u with the frame before the HUD",
                    g.generates, g.unprepared, g.resets, g.failed, g.gaps, g.prepares, g.prepareFailed, g.prepareResets, g.prepareGaps, g.hudLess);
                auto lastId = g.lastId;
                g = {};
                g.lastId = lastId;
            }

            if (device.cpuSync)
                return device.SubmitFrameAndWait() && generated;

            device.SubmitFrame(shared.WaitValue, shared.SignalValue, generated);
            return generated;
        }

        // Presentation: with PresentGenerate the frame between the one before and this one is generated first, on the
        // compute queue with FrameGenerationAsyncCompute; the present thread shows both
        bool Present()
        {
            auto& shared = *connection.shared;
            if (!display.Ready())
                return false;

            Display::Job job;
            job.inputValue = shared.WaitValue;
            job.presentValue = shared.SignalValue;
            job.syncInterval = std::min(shared.SyncInterval, 4u);
            job.delay = std::clamp(shared.PresentDelay, 0.0f, 1.0f);
            bool ok = true;
            if (shared.PresentGenerate && fsr.HasFrameGeneration())
            {
                // Only after the preparation of the same frame
                bool prepared = preparedFrame && preparedFrameId == shared.FrameId;
                preparedFrame = false;
                auto& g = generationStats;
                ++g.generates;
                g.unprepared += !prepared;
                g.resets += shared.GenerateReset != 0;
                g.gaps += g.lastId != 0 && shared.FrameId != g.lastId + 1;
                g.lastId = shared.FrameId;

                if (prepared)
                {
                    using T = Protocol::Texture;
                    auto cmd = device.BeginFrame(Device::Work::Generate);
                    device.Transition(cmd, true, { T::Present, T::Generated, T::HudLess });
                    device.Stamp(cmd);
                    bool generated = fsr.GenerateFrame(cmd, device, shared.FrameId, shared.GenerateReset != 0,
                        (flags & Protocol::ConfigureFlags::HighDynamicRange) != 0, shared.MaxLuminance);
                    device.Stamp(cmd);
                    device.Transition(cmd, false, { T::Present, T::Generated, T::HudLess });
                    g.failed += !generated;
                    ok = generated;
                    job.generatedValue = device.SubmitGenerated(shared.WaitValue, generated);
                    // Nothing to interpolate from after a reset: the rendered frame alone
                    job.showGenerated = generated && shared.GenerateReset == 0;
                }

                if (g.generates >= 300)
                {
                    if (g.unprepared || g.failed || g.gaps || g.prepareFailed || g.prepareGaps || g.resets > 2)
                        Log("Frame generation over %u Present: %u not prepared, %u reset, %u failed, %u with a gap in the frame numbers; "
                        "%u Prepare: %u failed, %u reset, %u with a gap, %u with the frame before the HUD",
                        g.generates, g.unprepared, g.resets, g.failed, g.gaps, g.prepares, g.prepareFailed, g.prepareResets, g.prepareGaps, g.hudLess);
                    auto lastId = g.lastId;
                    g = {};
                    g.lastId = lastId;
                }
            }
            display.Post(job);
            // A failed generation still presents the rendered frame (counted above)
            static uint32_t reported = 0;
            if (!ok && reported++ < 5)
                Log("Frame generation failed: the rendered frame alone");
            return true;
        }

    public:
        int Run(const std::wstring& name)
        {
            if (!connection.Open(name))
                return 1;

            auto& shared = *connection.shared;
            gLog = _wfopen(shared.LogPath, L"w");
            Log("GTAIV.EFLC.FusionFix upscaler helper started for process %u", shared.GameProcessId);

            // The game's GPU work waits for what this submits: a helper that gets the CPU late, behind the game's own
            // busy threads, holds up the frame and the game's next request alike. Its work is short and it sleeps between.
            bool raised = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) && SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            Log("Priority %s", raised ? "raised: above normal, highest thread" : "could not be raised");

            LUID luid{ shared.AdapterLuidLow, shared.AdapterLuidHigh };
            if (!device.Create(luid))
            {
                connection.Message("D3D12 device could not be created on the game's adapter");
                connection.Respond(Protocol::Status::Failed, shared.RequestSerial);
                return 2;
            }

            DXGI_ADAPTER_DESC1 adapterDesc{};
            device.adapter->GetDesc1(&adapterDesc);
            Log("Adapter: %ls", adapterDesc.Description);

            // The runtimes are shipped in the game folder, next to vulkan.dll
            std::vector<std::wstring> searchPaths = { shared.GameDirectory, shared.PluginsDirectory };
            std::string dlssMessage;
            std::string fsrMessage;
            if (adapterDesc.VendorId == 0x10DE)
                dlss.Init(device.device.Get(), searchPaths, shared.PluginsDirectory, dlssMessage);
            else
                dlssMessage = "DLSS is not available: not an NVIDIA GPU";
            fsr.Init(device.device.Get(), searchPaths, fsrMessage);

            Log("%s", dlssMessage.c_str());
            Log("%s", fsrMessage.c_str());
            shared.DLSSAvailable = dlss.available;
            shared.FSRAvailable = fsr.available;
            shared.FrameGenerationAvailable = fsr.available && fsr.frameGenerationAvailable;
            connection.Message("%s; %s", dlssMessage.c_str(), fsrMessage.c_str());
            connection.Respond(Protocol::Status::Ok, shared.RequestSerial);

            HANDLE handles[] = { connection.request, connection.game };
            while (true)
            {
                auto wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0)
                    break;

                MemoryBarrier();
                auto serial = shared.RequestSerial;
                MemoryBarrier();
                auto command = shared.RequestCommand;
                if (command == Protocol::Command::Shutdown)
                {
                    connection.Respond(Protocol::Status::Ok, serial);
                    break;
                }

                bool ok = false;
                switch (command)
                {
                case Protocol::Command::Configure:
                    ok = Configure();
                    if (!ok)
                        Log("Configure failed");
                    break;
                case Protocol::Command::Evaluate:
                {
                    ok = Evaluate();
                    // The first frame, and the first failures
                    static uint32_t evaluated = 0, failed = 0;
                    if (ok && evaluated++ == 0)
                        Log("First frame upscaled");
                    if (!ok && failed++ < 5)
                        Log("Evaluate failed");
                    break;
                }
                case Protocol::Command::Generate:
                {
                    ok = Generate();
                    static uint32_t generated = 0, failed = 0;
                    if (ok && generated++ == 0)
                        Log("First frame generated");
                    if (!ok && failed++ < 5)
                        Log("Generate failed");
                    break;
                }
                case Protocol::Command::Present:
                {
                    ok = Present();
                    static uint32_t failed = 0;
                    if (!ok && failed++ < 5)
                        Log("Present failed");
                    break;
                }
                case Protocol::Command::Hide:
                    display.Hide();
                    ok = true;
                    break;
                default: break;
                }
                connection.Respond(ok ? Protocol::Status::Ok : Protocol::Status::Failed, serial);
            }

            // Both queues done before their work's resources go
            display.Release();
            device.Flush();
            dlss.Shutdown();
            fsr.Shutdown();
            device.ReleaseTextures();
            Log("Helper stopped");
            if (gLog)
                fclose(gLog);
            return 0;
        }
    };
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3 || std::wstring_view(argv[1]) != Protocol::ArgumentName)
        return 0;

    std::wstring name = argv[2];
    LocalFree(argv);

    Helper helper;
    return helper.Run(name);
}
