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
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> sharedFence;
        ComPtr<ID3D12Fence> localFence;
        uint64_t localValue = 0;
        HANDLE fenceEvent = nullptr;

        // Evaluate and Generate each take one
        static constexpr uint32_t Frames = 6;
        std::array<ComPtr<ID3D12CommandAllocator>, Frames> allocators;
        std::array<uint64_t, Frames> allocatorValues{};
        ComPtr<ID3D12GraphicsCommandList> list;
        uint32_t frame = 0;

        std::array<SharedTexture, static_cast<size_t>(Protocol::Texture::Count)> textures;
        HANDLE sharedFenceHandle = nullptr;

        // GPU time of the helper's own work, a few timestamps a command list, read back once its allocator is reused
        // and logged every 300 frames: what the upscaler and the frame generation cost without the copies and the
        // waits on the game's side
        enum class Work : uint32_t { None, Evaluate, Generate };
        static constexpr uint32_t StampsPerFrame = 4;
        ComPtr<ID3D12QueryHeap> queryHeap;
        ComPtr<ID3D12Resource> queryReadback;
        const uint64_t* queryData = nullptr;
        double ticksPerMs = 0.0;
        std::array<uint32_t, Frames> stampCounts{};
        std::array<Work, Frames> stampWork{};
        struct GpuStats
        {
            uint32_t evaluates = 0, generates = 0, prepares = 0;
            double upscale = 0.0, prepare = 0.0, generate = 0.0, evaluateList = 0.0, generateList = 0.0;
            double upscaleMax = 0.0, generateMax = 0.0;
        } gpuStats;

        void CreateTimestamps()
        {
            uint64_t frequency = 0;
            if (FAILED(queue->GetTimestampFrequency(&frequency)) || !frequency)
                return;
            D3D12_QUERY_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heapDesc.Count = Frames * StampsPerFrame;
            if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap))))
                return;

            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = sizeof(uint64_t) * Frames * StampsPerFrame;
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
            ticksPerMs = static_cast<double>(frequency) / 1000.0;
        }

        // A timestamp in the frame's command list
        void Stamp(ID3D12GraphicsCommandList* cmd)
        {
            if (!queryHeap || stampCounts[frame] >= StampsPerFrame)
                return;
            cmd->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * StampsPerFrame + stampCounts[frame]++);
        }

        // Before the frame's command list is closed
        void ResolveStamps()
        {
            if (queryHeap && stampCounts[frame])
                list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * StampsPerFrame, stampCounts[frame], queryReadback.Get(),
                    sizeof(uint64_t) * frame * StampsPerFrame);
        }

        // The timestamps of a frame the GPU has finished: Evaluate has start, upscaled, prepared; Generate start, generated
        void ReadStamps(uint32_t slot)
        {
            auto count = stampCounts[slot];
            auto work = stampWork[slot];
            stampCounts[slot] = 0;
            stampWork[slot] = Work::None;
            if (!queryData || count < 2)
                return;
            auto t = queryData + slot * StampsPerFrame;
            auto ms = [&](uint32_t from, uint32_t to) { return t[to] > t[from] ? static_cast<double>(t[to] - t[from]) / ticksPerMs : 0.0; };
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
                    Log("  frame generation: prepare %.3f over %u frames, generate %.3f (%.3f at most) over %u frames, Generate's command list %.3f",
                        g.prepare / g.prepares, g.prepares, g.generates ? g.generate / g.generates : 0.0, g.generateMax, g.generates,
                        g.generates ? g.generateList / g.generates : 0.0);
                g = {};
            }
        }

        // Wine (see the protocol): the shared textures have no UAV flag, which keeps them plain for the game's Vulkan
        // import, the upscaler and the frame generation write into uavs, which are copied into the shared textures.
        // sharedFence is then the game's semaphore, or without it (cpuSync) the GPU work is waited for on the CPU.
        bool wine = false;
        bool cpuSync = false;
        std::array<ComPtr<ID3D12Resource>, static_cast<size_t>(Protocol::Texture::Count)> uavs;

        bool Create(LUID luid)
        {
            ComPtr<IDXGIFactory4> factory;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
                return false;
            if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
                return false;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                return false;

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

            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence))))
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
            allocators[0]->Reset();
            list->Reset(allocators[0].Get(), nullptr);
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

        // Command list of one frame; allocators are reused once the GPU finished with them
        ID3D12GraphicsCommandList* BeginFrame(Work work)
        {
            frame = (frame + 1) % Frames;
            auto fence = FrameFence();
            if (fence)
                WaitFence(fence, allocatorValues[frame], 1000);
            // Timestamps the GPU wrote: only once it is past the frame
            if (stampCounts[frame] && fence && fence->GetCompletedValue() >= allocatorValues[frame])
                ReadStamps(frame);
            stampCounts[frame] = 0;
            stampWork[frame] = work;
            allocators[frame]->Reset();
            list->Reset(allocators[frame].Get(), nullptr);
            return list.Get();
        }

        // The fence the frames' allocators are tracked with
        ID3D12Fence* FrameFence()
        {
            return cpuSync ? localFence.Get() : sharedFence.Get();
        }

        // Wine: runs the frame and waits for it, the game copies the output once Evaluate is answered
        bool SubmitFrameAndWait()
        {
            ResolveStamps();
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(localFence.Get(), ++localValue);
            allocatorValues[frame] = localValue;
            return WaitFence(localFence.Get(), localValue, 2000);
        }

        void SubmitFrame(uint64_t waitValue, uint64_t signalValue, bool execute)
        {
            ResolveStamps();
            list->Close();
            queue->Wait(sharedFence.Get(), waitValue);
            if (execute)
            {
                ID3D12CommandList* lists[] = { list.Get() };
                queue->ExecuteCommandLists(1, lists);
            }
            queue->Signal(sharedFence.Get(), signalValue);
            allocatorValues[frame] = signalValue;
        }

        void ReleaseTextures()
        {
            // The game has already imported or dropped its duplicates
            if (auto fence = FrameFence())
                WaitFence(fence, *std::max_element(allocatorValues.begin(), allocatorValues.end()), 1000);
            for (auto& texture : textures)
            {
                if (texture.handle)
                    CloseHandle(texture.handle);
                texture = {};
            }
            for (auto& uav : uavs)
                uav.Reset();
            if (sharedFenceHandle)
                CloseHandle(sharedFenceHandle);
            sharedFenceHandle = nullptr;
            sharedFence.Reset();
            allocatorValues.fill(0);
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

        bool CreateSharedFence()
        {
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence))))
                return false;
            return SUCCEEDED(device->CreateSharedHandle(sharedFence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
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

    class Helper
    {
        Connection connection;
        Device device;
        DLSS dlss;
        FSR fsr;
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

        // Wine: the game's timeline semaphore, duplicated into this process, as the shared fence. Without it GameFence
        // is cleared in the answer and both sides wait on the CPU.
        void OpenGameFence()
        {
            auto& shared = *connection.shared;
            auto handle = reinterpret_cast<HANDLE>(shared.FenceHandle);
            shared.FenceHandle = 0;
            if (!(flags & Protocol::ConfigureFlags::GameFence))
            {
                if (handle)
                    CloseHandle(handle);
                return;
            }

            ComPtr<ID3D12Fence> fence;
            auto hr = handle ? device.device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence)) : E_INVALIDARG;
            if (handle)
                CloseHandle(handle);
            if (SUCCEEDED(hr) && fence)
            {
                device.sharedFence = fence;
                Log("The game's semaphore is the shared fence");
            }
            else
            {
                flags &= ~Protocol::ConfigureFlags::GameFence;
                Log("The game's semaphore could not be opened (0x%08X): waiting on the CPU", static_cast<uint32_t>(hr));
            }
            shared.Flags = flags;
        }

        bool Configure()
        {
            auto& shared = *connection.shared;

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
                (!device.wine && !device.CreateSharedFence()))
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
                    flags &= ~(Protocol::ConfigureFlags::FrameGeneration | Protocol::ConfigureFlags::EightBitFrames);
                    shared.Flags = flags;
                }
            }

            for (size_t i = 0; i < static_cast<size_t>(T::Count); ++i)
            {
                shared.TextureSizes[i] = device.AllocationSize(static_cast<T>(i));
                shared.TextureHandles[i] = 0;
                if (!device.textures[i].handle)
                    continue;
                if (!Duplicate(device.textures[i].handle, shared.TextureHandles[i]))
                    return false;
            }
            shared.FenceHandle = 0;
            if (!device.wine && !Duplicate(device.sharedFenceHandle, shared.FenceHandle))
                return false;

            backend = shared.ConfigureBackend;
            connection.Message("%s ready at %ux%u -> %ux%u%s", backend == Protocol::Backend::DLSS ? "DLSS" : "FSR", width, height, outputWidth, outputHeight,
                !fsr.HasFrameGeneration() ? "" : (flags & Protocol::ConfigureFlags::EightBitFrames) ? ", with frame generation (8-bit frames)" : ", with frame generation");
            return true;
        }

        bool Evaluate()
        {
            auto& shared = *connection.shared;
            if (backend == Protocol::Backend::None || (!device.cpuSync && !device.sharedFence))
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
            preparedFrame = fsr.HasFrameGeneration() && fsr.PrepareFrame(cmd, device, frame);
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
            if (!fsr.HasFrameGeneration() || (!device.cpuSync && !device.sharedFence))
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

    public:
        int Run(const std::wstring& name)
        {
            if (!connection.Open(name))
                return 1;

            auto& shared = *connection.shared;
            gLog = _wfopen(shared.LogPath, L"w");
            Log("GTAIV.EFLC.FusionFix upscaler helper started for process %u", shared.GameProcessId);

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
                default: break;
                }
                connection.Respond(ok ? Protocol::Status::Ok : Protocol::Status::Failed, serial);
            }

            if (device.queue && device.localFence)
            {
                device.queue->Signal(device.localFence.Get(), ++device.localValue);
                device.WaitFence(device.localFence.Get(), device.localValue, 2000);
            }
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
