module;

#include <common.hxx>
#include <cstdarg>
#include <cstdio>
#include <d3d12.h>
#include <d3d9on12.h>
#include <dxvk_interop.hpp>
#include <upscaler_protocol.hpp>

#include "FusionLog.hpp"

export module upscaler;

import common;
import comvars;

namespace Protocol = UpscalerProtocol;

// NVIDIA DLSS and AMD FSR, run by GTAIV.EFLC.FusionFix.exe (x64) next to the plugin.
//
// The helper creates shared D3D12 textures and a shared fence. The plugin copies the frame's color, depth,
// motion vectors and reactive mask into them on the GPU, the helper waits for that on its queue, runs the
// upscaler and signals the fence, and the plugin copies the result back, all without the CPU waiting for
// the GPU. The copies run where the game renders:
// - DXVK: the shared textures and the fence are imported into DXVK's Vulkan device, the copies are
//   submitted to DXVK's queue. Under Wine (Proton) the textures are imported as opaque handles and both
//   sides wait for their GPU work on the CPU: Wine crashes importing a D3D12 fence of another process, and
//   older Proton can't import D3D12 resource handles.
// - D3D9on12 (Graphics API "DirectX 12"): the shared textures and the fence are opened on the D3D12 device
//   of D3D9on12, the game's textures are unwrapped to their D3D12 resources and copied on a queue of the
//   plugin.

namespace
{
    constexpr size_t TextureCount = static_cast<size_t>(Protocol::Texture::Count);

    // Game textures copied into the shared ones, by their index; null ones are skipped
    using Textures = std::array<IDirect3DTexture9*, TextureCount>;

    // The helper duplicated its handles into this process, they are closed once imported or not
    void CloseSharedHandles(Protocol::Shared& shared)
    {
        for (auto& handle : shared.TextureHandles)
        {
            if (handle)
                CloseHandle(reinterpret_cast<HANDLE>(handle));
            handle = 0;
        }
        if (shared.FenceHandle)
            CloseHandle(reinterpret_cast<HANDLE>(shared.FenceHandle));
        shared.FenceHandle = 0;
    }

    // GTAIV.EFLC.FusionFix.Upscaler.log next to the plugin (FusionLog, Upscaler.Game): what the game side did
    // with the helper, the first time each thing fails (the helper, another process, writes
    // GTAIV.EFLC.FusionFix.UpscalerHelper.log in the same format)
    void Log(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        FusionLog::WriteV("Upscaler", "Game", format, args);
        va_end(args);
    }

    // Logs a failure the first few times it happens
    bool Report(uint32_t& count)
    {
        return count++ < 3;
    }

    bool IsWine()
    {
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        return (ntdll && GetProcAddress(ntdll, "wine_get_version")) || GetModuleHandleW(L"winevulkan.dll");
    }

    uint32_t AdapterVendor(IDirect3DDevice9* device)
    {
        uint32_t vendor = 0;
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(device->GetDirect3D(&d3d)) && d3d)
        {
            D3DDEVICE_CREATION_PARAMETERS parameters{};
            D3DADAPTER_IDENTIFIER9 identifier{};
            if (SUCCEEDED(device->GetCreationParameters(&parameters)) && SUCCEEDED(d3d->GetAdapterIdentifier(parameters.AdapterOrdinal, 0, &identifier)))
                vendor = identifier.VendorId;
            d3d->Release();
        }
        return vendor;
    }

    // Exchange of the frames between the game's renderer and the shared textures
    class Bridge
    {
    public:
        LUID luid{};
        uint32_t vendorId = 0;
        uint32_t width = 0;           // render size of the inputs
        uint32_t height = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        bool frameGeneration = false; // Present and Generated were imported
        bool wine = false;            // ConfigureFlags::Wine
        bool gameFence = false;       // Wine: the helper opened the game's semaphore, else both sides wait on the CPU

        virtual ~Bridge() = default;
        // Wine: a semaphore of the game for the helper's fence, its handle duplicated into the helper. Before Configure.
        virtual bool PrepareGameFence(Protocol::Shared& shared, HANDLE helperProcess) { return false; }
        // Opens the shared textures and the fence of a configuration, and closes their handles
        virtual bool Import(Protocol::Shared& shared, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) = 0;
        bool WaitOnCpu() const { return wine && !gameFence; }
        virtual void ReleaseImports() = 0;
        // Game textures -> shared textures, then the fence reaches signalValue. Null inputs are skipped.
        virtual bool SubmitInputs(const Textures& inputs, uint64_t signalValue) = 0;
        // Once the fence reaches waitValue: shared texture (Output or Generated) -> game texture
        virtual bool SubmitOutput(IDirect3DTexture9* target, Protocol::Texture index, uint64_t waitValue) = 0;

        // Size of a shared texture
        uint32_t Width(size_t index) const { return Protocol::IsOutputSize(static_cast<Protocol::Texture>(index)) ? outputWidth : width; }
        uint32_t Height(size_t index) const { return Protocol::IsOutputSize(static_cast<Protocol::Texture>(index)) ? outputHeight : height; }
        // The helper won't signal value any more: the GPU work that waits for it is let go from the CPU
        virtual void SignalFromCpu(uint64_t value) = 0;
    };
    struct Vulkan
    {
        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
        PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkCreateImage vkCreateImage = nullptr;
        PFN_vkDestroyImage vkDestroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
        PFN_vkAllocateMemory vkAllocateMemory = nullptr;
        PFN_vkFreeMemory vkFreeMemory = nullptr;
        PFN_vkBindImageMemory vkBindImageMemory = nullptr;
        PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
        PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
        PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
        PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
        PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR = nullptr;   // optional, for the game's semaphore under Wine
        PFN_vkSignalSemaphore vkSignalSemaphore = nullptr;                         // optional, when the helper is gone
        PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;
        PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
        PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
        PFN_vkQueueSubmit vkQueueSubmit = nullptr;
        PFN_vkCreateFence vkCreateFence = nullptr;
        PFN_vkWaitForFences vkWaitForFences = nullptr;
        PFN_vkResetFences vkResetFences = nullptr;

        bool Load(VkInstance instance, VkDevice device)
        {
            HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
            if (!loader)
                loader = GetModuleHandleW(L"winevulkan.dll");
            if (!loader)
                return false;

            vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
            if (!vkGetInstanceProcAddr)
                return false;

#define LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name))
#define LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name))
            LOAD_INSTANCE(vkGetDeviceProcAddr);
            LOAD_INSTANCE(vkGetPhysicalDeviceProperties2);
            LOAD_INSTANCE(vkGetPhysicalDeviceMemoryProperties);
            if (!vkGetDeviceProcAddr || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceMemoryProperties)
                return false;

            LOAD_DEVICE(vkCreateImage);
            LOAD_DEVICE(vkDestroyImage);
            LOAD_DEVICE(vkGetImageMemoryRequirements);
            LOAD_DEVICE(vkAllocateMemory);
            LOAD_DEVICE(vkFreeMemory);
            LOAD_DEVICE(vkBindImageMemory);
            LOAD_DEVICE(vkGetMemoryWin32HandlePropertiesKHR);
            LOAD_DEVICE(vkCreateSemaphore);
            LOAD_DEVICE(vkDestroySemaphore);
            LOAD_DEVICE(vkImportSemaphoreWin32HandleKHR);
            LOAD_DEVICE(vkGetSemaphoreWin32HandleKHR);
            LOAD_DEVICE(vkSignalSemaphore);
            LOAD_DEVICE(vkGetSemaphoreCounterValue);
            LOAD_DEVICE(vkCreateCommandPool);
            LOAD_DEVICE(vkAllocateCommandBuffers);
            LOAD_DEVICE(vkBeginCommandBuffer);
            LOAD_DEVICE(vkEndCommandBuffer);
            LOAD_DEVICE(vkResetCommandBuffer);
            LOAD_DEVICE(vkCmdPipelineBarrier);
            LOAD_DEVICE(vkCmdCopyImage);
            LOAD_DEVICE(vkQueueSubmit);
            LOAD_DEVICE(vkCreateFence);
            LOAD_DEVICE(vkWaitForFences);
            LOAD_DEVICE(vkResetFences);
#undef LOAD_INSTANCE
#undef LOAD_DEVICE

            // The external memory and semaphore functions only exist when DXVK enabled the extensions
            return vkCreateImage && vkDestroyImage && vkGetImageMemoryRequirements && vkAllocateMemory && vkFreeMemory && vkBindImageMemory &&
                vkGetMemoryWin32HandlePropertiesKHR && vkCreateSemaphore && vkDestroySemaphore && vkImportSemaphoreWin32HandleKHR &&
                vkCreateCommandPool && vkAllocateCommandBuffers && vkBeginCommandBuffer && vkEndCommandBuffer && vkResetCommandBuffer &&
                vkCmdPipelineBarrier && vkCmdCopyImage && vkQueueSubmit && vkCreateFence && vkWaitForFences && vkResetFences;
        }
    };

    struct GameImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent{};
    };

    bool GetGameImage(IDirect3DTexture9* texture, GameImage& out)
    {
        if (!texture)
            return false;

        ID3D9VkInteropTexture* interop = nullptr;
        if (FAILED(texture->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&interop))) || !interop)
            return false;

        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        auto hr = interop->GetVulkanImageInfo(&out.image, &out.layout, &info);
        interop->Release();
        out.format = info.format;
        out.extent = info.extent;
        return SUCCEEDED(hr) && out.image != VK_NULL_HANDLE;
    }

    VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        uint32_t srcQueueFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstQueueFamily = VK_QUEUE_FAMILY_IGNORED)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = srcQueueFamily;
        barrier.dstQueueFamilyIndex = dstQueueFamily;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return barrier;
    }

    VkImageCopy FullCopy(uint32_t width, uint32_t height)
    {
        VkImageCopy copy{};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.extent = { width, height, 1 };
        return copy;
    }

    // ---------------------------------------------------------------------------------------------
    // DXVK

    class DXVKBridge : public Bridge
    {
    public:
        ID3D9VkInteropDevice* interop = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        Vulkan vk;

        VkCommandPool pool = VK_NULL_HANDLE;
        struct Slot
        {
            VkCommandBuffer inputs = VK_NULL_HANDLE;
            VkCommandBuffer output = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            bool submitted = false;
        };
        std::array<Slot, 4> slots{};
        uint32_t slot = 0;

        struct SharedImage
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
        };
        std::array<SharedImage, TextureCount> images{};
        VkSemaphore semaphore = VK_NULL_HANDLE;

        // Vulkan formats of the shared textures and of the game textures copied from and to them
        static constexpr VkFormat Formats[TextureCount] =
        {
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT
        };

        bool Init(IDirect3DDevice9* realDevice)
        {
            if (FAILED(realDevice->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&interop))) || !interop)
                return false;

            interop->GetVulkanHandles(&instance, &physicalDevice, &device);
            uint32_t queueIndex = 0;
            interop->GetSubmissionQueue(&queue, &queueIndex, &queueFamily);
            if (!instance || !physicalDevice || !device || !queue || !vk.Load(instance, device))
                return false;

            VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            properties.pNext = &id;
            vk.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
            if (!id.deviceLUIDValid)
                return false;
            std::memcpy(&luid, id.deviceLUID, sizeof(luid));
            vendorId = properties.properties.vendorID;
            wine = IsWine();

            VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamily;
            if (vk.vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
                return false;

            for (auto& s : slots)
            {
                VkCommandBuffer buffers[2]{};
                VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocate.commandPool = pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 2;
                if (vk.vkAllocateCommandBuffers(device, &allocate, buffers) != VK_SUCCESS)
                    return false;
                s.inputs = buffers[0];
                s.output = buffers[1];

                VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                if (vk.vkCreateFence(device, &fenceInfo, nullptr, &s.fence) != VK_SUCCESS)
                    return false;
            }
            return true;
        }

        void ReleaseImports() override
        {
            // Nothing may still be using the images: wait for the last submissions
            for (auto& s : slots)
            {
                if (s.submitted)
                {
                    vk.vkWaitForFences(device, 1, &s.fence, VK_TRUE, 2000000000ull);
                    vk.vkResetFences(device, 1, &s.fence);
                    s.submitted = false;
                }
            }
            for (auto& image : images)
            {
                if (image.image)
                    vk.vkDestroyImage(device, image.image, nullptr);
                if (image.memory)
                    vk.vkFreeMemory(device, image.memory, nullptr);
                image = {};
            }
            if (semaphore)
                vk.vkDestroySemaphore(device, semaphore, nullptr);
            semaphore = VK_NULL_HANDLE;
            width = height = outputWidth = outputHeight = 0;
            frameGeneration = false;
        }

        // Windows: a D3D12 resource handle. Wine: the same as an opaque handle, which is what vkd3d-proton exports
        // and every winevulkan imports; the image then has the usage vkd3d-proton gives a texture without flags.
        bool ImportImage(SharedImage& target, HANDLE handle, VkFormat format, uint32_t w, uint32_t h)
        {
            auto handleType = wine ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
            external.handleTypes = handleType;

            VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            info.pNext = &external;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = { w, h, 1 };
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (wine ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (auto vr = vk.vkCreateImage(device, &info, nullptr, &target.image); vr != VK_SUCCESS)
            {
                Log("import: vkCreateImage %d", vr);
                return false;
            }
            target.format = format;

            VkMemoryRequirements requirements{};
            vk.vkGetImageMemoryRequirements(device, target.image, &requirements);

            // Opaque handles have no handle properties: the exporter's memory is plain device local memory
            auto types = requirements.memoryTypeBits;
            if (!wine)
            {
                VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
                if (auto vr = vk.vkGetMemoryWin32HandlePropertiesKHR(device, handleType, handle, &handleProperties); vr != VK_SUCCESS)
                {
                    Log("import: vkGetMemoryWin32HandlePropertiesKHR %d", vr);
                    return false;
                }
                types &= handleProperties.memoryTypeBits;
            }

            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vk.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
            uint32_t typeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            {
                auto flags = memoryProperties.memoryTypes[i].propertyFlags;
                if (!(types & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    continue;
                if (typeIndex == UINT32_MAX || flags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                    typeIndex = i;
                if (flags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                    break;
            }
            if (typeIndex == UINT32_MAX)
            {
                Log("import: no device local memory type in %08x", requirements.memoryTypeBits);
                return false;
            }

            VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            dedicated.image = target.image;

            VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
            import.pNext = &dedicated;
            import.handleType = handleType;
            import.handle = handle;

            VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.pNext = &import;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = typeIndex;
            if (auto vr = vk.vkAllocateMemory(device, &allocate, nullptr, &target.memory); vr != VK_SUCCESS)
            {
                Log("import: vkAllocateMemory %d (size %llu, type %u)", vr, static_cast<unsigned long long>(allocate.allocationSize), allocate.memoryTypeIndex);
                return false;
            }

            if (auto vr = vk.vkBindImageMemory(device, target.image, target.memory, 0); vr != VK_SUCCESS)
            {
                Log("import: vkBindImageMemory %d", vr);
                return false;
            }
            return true;
        }

        bool PrepareGameFence(Protocol::Shared& shared, HANDLE helperProcess) override
        {
            if (!wine || !vk.vkGetSemaphoreWin32HandleKHR || !helperProcess)
                return false;

            VkExportSemaphoreCreateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
            exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkSemaphoreTypeCreateInfo type{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            type.pNext = &exportInfo;
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            info.pNext = &type;
            if (auto vr = vk.vkCreateSemaphore(device, &info, nullptr, &semaphore); vr != VK_SUCCESS)
            {
                Log("game fence: vkCreateSemaphore %d", vr);
                semaphore = VK_NULL_HANDLE;
                return false;
            }

            HANDLE handle = nullptr;
            VkSemaphoreGetWin32HandleInfoKHR get{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
            get.semaphore = semaphore;
            get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            HANDLE duplicate = nullptr;
            if (auto vr = vk.vkGetSemaphoreWin32HandleKHR(device, &get, &handle); vr != VK_SUCCESS || !handle ||
                !DuplicateHandle(GetCurrentProcess(), handle, helperProcess, &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                Log("game fence: the semaphore could not be shared (%d, handle %p)", vr, handle);
                vk.vkDestroySemaphore(device, semaphore, nullptr);
                semaphore = VK_NULL_HANDLE;
                return false;
            }
            shared.FenceHandle = reinterpret_cast<uint64_t>(duplicate);
            return true;
        }

        // Evaluate released the previous imports before the helper's Configure
        bool Import(Protocol::Shared& shared, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) override
        {
            bool ok = true;
            for (size_t i = 0; i < images.size(); ++i)
            {
                auto handle = reinterpret_cast<HANDLE>(shared.TextureHandles[i]);
                // Without frame generation its textures don't exist
                if (!handle && Protocol::IsFrameGenerationTexture(static_cast<Protocol::Texture>(i)))
                    continue;
                auto iw = Protocol::IsOutputSize(static_cast<Protocol::Texture>(i)) ? ow : w;
                auto ih = Protocol::IsOutputSize(static_cast<Protocol::Texture>(i)) ? oh : h;
                ok = handle && ImportImage(images[i], handle, Formats[i], iw, ih);
                if (!ok)
                {
                    Log("import: shared texture %zu (%ux%u) failed, handle %p", i, iw, ih, handle);
                    break;
                }
            }

            auto fenceHandle = reinterpret_cast<HANDLE>(shared.FenceHandle);
            if (ok && wine)
            {
                // The game's own semaphore, or without it the CPU waits
                if (!gameFence && semaphore)
                {
                    vk.vkDestroySemaphore(device, semaphore, nullptr);
                    semaphore = VK_NULL_HANDLE;
                }
            }
            else if (ok && fenceHandle)
            {
                VkSemaphoreTypeCreateInfo type{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
                type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                info.pNext = &type;
                ok = vk.vkCreateSemaphore(device, &info, nullptr, &semaphore) == VK_SUCCESS;

                if (ok)
                {
                    VkImportSemaphoreWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
                    import.semaphore = semaphore;
                    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
                    import.handle = fenceHandle;
                    ok = vk.vkImportSemaphoreWin32HandleKHR(device, &import) == VK_SUCCESS;
                }
            }
            else
            {
                ok = false;
            }
            CloseSharedHandles(shared);

            if (!ok)
            {
                ReleaseImports();
                return false;
            }

            width = w;
            height = h;
            outputWidth = ow;
            outputHeight = oh;
            frameGeneration = images[static_cast<size_t>(Protocol::Texture::Present)].image && images[static_cast<size_t>(Protocol::Texture::Generated)].image;
            return true;
        }

        Slot& NextSlot()
        {
            slot = (slot + 1) % slots.size();
            auto& s = slots[slot];
            if (s.submitted)
            {
                vk.vkWaitForFences(device, 1, &s.fence, VK_TRUE, 2000000000ull);
                vk.vkResetFences(device, 1, &s.fence);
                s.submitted = false;
            }
            return s;
        }

        bool Submit(VkCommandBuffer buffer, VkSemaphore waitSemaphore, uint64_t waitValue, uint64_t signalValue, VkFence fence)
        {
            VkTimelineSemaphoreSubmitInfo timeline{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.pNext = &timeline;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &buffer;

            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            if (waitValue)
            {
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &waitSemaphore;
                submit.pWaitDstStageMask = &waitStage;
                timeline.waitSemaphoreValueCount = 1;
                timeline.pWaitSemaphoreValues = &waitValue;
            }
            if (signalValue)
            {
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &semaphore;
                timeline.signalSemaphoreValueCount = 1;
                timeline.pSignalSemaphoreValues = &signalValue;
            }

            interop->LockSubmissionQueue();
            auto result = vk.vkQueueSubmit(queue, 1, &submit, fence);
            interop->ReleaseSubmissionQueue();
            return result == VK_SUCCESS;
        }

        bool SubmitInputs(const Textures& inputs, uint64_t signalValue) override
        {
            // Everything the game rendered so far must reach the queue first. Before the images are asked for, too: DXVK
            // can give a texture new storage (relocation), which only its command thread knows about.
            interop->FlushRenderingCommands();

            GameImage sources[TextureCount];
            for (size_t i = 0; i < TextureCount; ++i)
            {
                if (!inputs[i])
                    continue;
                if (!images[i].image || !GetGameImage(inputs[i], sources[i]) || sources[i].format != Formats[i] ||
                    sources[i].extent.width != Width(i) || sources[i].extent.height != Height(i))
                {
                    static uint32_t reported = 0;
                    if (Report(reported))
                        Log("inputs: texture %zu is format %d %ux%u, expected %d %ux%u", i, sources[i].format, sources[i].extent.width,
                            sources[i].extent.height, Formats[i], Width(i), Height(i));
                    return false;
                }
            }

            auto& s = NextSlot();

            auto cmd = s.inputs;
            vk.vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk.vkBeginCommandBuffer(cmd, &begin);

            VkImageMemoryBarrier before[TextureCount * 2];
            VkImageMemoryBarrier after[TextureCount * 2];
            uint32_t barriers = 0;
            for (size_t i = 0; i < TextureCount; ++i)
            {
                if (!inputs[i])
                    continue;
                auto shared = images[i].image;
                before[barriers] = ImageBarrier(sources[i].image, sources[i].layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                after[barriers++] = ImageBarrier(sources[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sources[i].layout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
                before[barriers] = ImageBarrier(shared, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                // Handed over to D3D12, where it is in the common state
                after[barriers++] = ImageBarrier(shared, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, queueFamily, VK_QUEUE_FAMILY_EXTERNAL);
            }

            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, barriers, before);
            for (size_t i = 0; i < TextureCount; ++i)
            {
                if (!inputs[i])
                    continue;
                auto copy = FullCopy(Width(i), Height(i));
                vk.vkCmdCopyImage(cmd, sources[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, barriers, after);
            vk.vkEndCommandBuffer(cmd);

            if (!WaitOnCpu())
                return Submit(cmd, VK_NULL_HANDLE, 0, signalValue, VK_NULL_HANDLE);

            // Wine: done before the helper is asked to read them
            if (!Submit(cmd, VK_NULL_HANDLE, 0, 0, s.fence))
                return false;
            auto done = vk.vkWaitForFences(device, 1, &s.fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
            vk.vkResetFences(device, 1, &s.fence);
            return done;
        }

        bool SubmitOutput(IDirect3DTexture9* target, Protocol::Texture index, uint64_t waitValue) override
        {
            // The target's current storage, as for the inputs
            interop->FlushRenderingCommands();

            auto i = static_cast<size_t>(index);
            GameImage destination;
            if (!images[i].image || !GetGameImage(target, destination) || destination.format != Formats[i] ||
                destination.extent.width != Width(i) || destination.extent.height != Height(i))
            {
                static uint32_t reported = 0;
                if (Report(reported))
                    Log("output: target %zu is format %d %ux%u, expected %d %ux%u", i, destination.format, destination.extent.width,
                        destination.extent.height, Formats[i], Width(i), Height(i));
                return false;
            }

            auto& s = slots[slot];
            auto output = images[i].image;
            auto cmd = s.output;
            vk.vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk.vkBeginCommandBuffer(cmd, &begin);

            VkImageMemoryBarrier before[2] =
            {
                ImageBarrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, queueFamily),
                ImageBarrier(destination.image, destination.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
            };
            VkImageMemoryBarrier after[2] =
            {
                ImageBarrier(output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0, queueFamily, VK_QUEUE_FAMILY_EXTERNAL),
                ImageBarrier(destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destination.layout, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            };

            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
            auto copy = FullCopy(Width(i), Height(i));
            vk.vkCmdCopyImage(cmd, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, after);
            vk.vkEndCommandBuffer(cmd);

            // Wine: the helper answered Evaluate once the output was ready
            if (!Submit(cmd, WaitOnCpu() ? VK_NULL_HANDLE : semaphore, WaitOnCpu() ? 0 : waitValue, 0, s.fence))
                return false;
            s.submitted = true;
            return true;
        }

        void SignalFromCpu(uint64_t value) override
        {
            if (!semaphore || !vk.vkSignalSemaphore || !vk.vkGetSemaphoreCounterValue)
                return;
            uint64_t current = 0;
            if (vk.vkGetSemaphoreCounterValue(device, semaphore, &current) != VK_SUCCESS || current >= value)
                return;
            VkSemaphoreSignalInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
            signal.semaphore = semaphore;
            signal.value = value;
            vk.vkSignalSemaphore(device, &signal);
        }
    };

    // ---------------------------------------------------------------------------------------------
    // D3D9on12

    DXGI_FORMAT TypelessFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
            return DXGI_FORMAT_R16G16_TYPELESS;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
            return DXGI_FORMAT_R16_TYPELESS;
        default:
            return format;
        }
    }

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    class D3D12Bridge : public Bridge
    {
    public:
        IDirect3DDevice9* device9 = nullptr;
        IDirect3DDevice9On12* on12 = nullptr;
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12Fence* fence = nullptr;          // completion of the copies, for D3D9on12 and the command allocators
        uint64_t fenceValue = 0;
        HANDLE event = nullptr;

        struct Slot
        {
            ID3D12CommandAllocator* allocator = nullptr;
            ID3D12GraphicsCommandList* list = nullptr;
            uint64_t value = 0;               // fence value the GPU is done with the slot at
        };
        std::array<Slot, 8> slots{};
        uint32_t slot = 0;

        std::array<ID3D12Resource*, TextureCount> images{};
        ID3D12Fence* sharedFence = nullptr;

        bool Init(IDirect3DDevice9* realDevice)
        {
            if (FAILED(realDevice->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) || !on12)
                return false;
            if (FAILED(on12->GetD3D12Device(IID_PPV_ARGS(&device))) || !device)
                return false;

            device9 = realDevice;
            luid = device->GetAdapterLuid();
            vendorId = AdapterVendor(realDevice);

            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                return false;
            if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;

            for (auto& s : slots)
            {
                if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))))
                    return false;
                if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&s.list))))
                    return false;
                s.list->Close();
            }

            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return event != nullptr;
        }

        void WaitFor(uint64_t value)
        {
            if (value && fence->GetCompletedValue() < value && SUCCEEDED(fence->SetEventOnCompletion(value, event)))
                WaitForSingleObject(event, 2000);
        }

        Slot& NextSlot()
        {
            slot = (slot + 1) % slots.size();
            auto& s = slots[slot];
            WaitFor(s.value);
            s.allocator->Reset();
            s.list->Reset(s.allocator, nullptr);
            return s;
        }

        // Advances the fence after the work submitted so far: D3D9on12 waits for it before it uses a returned
        // resource again
        uint64_t SignalCopies()
        {
            queue->Signal(fence, ++fenceValue);
            return fenceValue;
        }

        void Return(IDirect3DTexture9* texture, ID3D12Resource*& resource)
        {
            ID3D12Fence* fences[] = { fence };
            UINT64 values[] = { fenceValue };
            on12->ReturnUnderlyingResource(texture, 1, values, fences);
            resource->Release();
            resource = nullptr;
        }

        bool Matches(ID3D12Resource* gameResource, ID3D12Resource* shared)
        {
            auto a = gameResource->GetDesc();
            auto b = shared->GetDesc();
            return a.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && a.Width == b.Width && a.Height == b.Height &&
                TypelessFormat(a.Format) == TypelessFormat(b.Format);
        }

        void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source)
        {
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = destination;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = source;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = 0;
            // Matches() made sure both have the source's size
            auto desc = source->GetDesc();
            D3D12_BOX box{ 0, 0, 0, static_cast<UINT>(desc.Width), desc.Height, 1 };
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }

        void SignalFromCpu(uint64_t value) override
        {
            if (sharedFence && sharedFence->GetCompletedValue() < value)
                sharedFence->Signal(value);
        }

        void ReleaseImports() override
        {
            WaitFor(fenceValue);
            for (auto& image : images)
            {
                if (image)
                    image->Release();
                image = nullptr;
            }
            if (sharedFence)
                sharedFence->Release();
            sharedFence = nullptr;
            width = height = outputWidth = outputHeight = 0;
            frameGeneration = false;
        }

        bool Import(Protocol::Shared& shared, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) override
        {
            bool ok = true;
            for (size_t i = 0; i < images.size(); ++i)
            {
                auto handle = reinterpret_cast<HANDLE>(shared.TextureHandles[i]);
                // Without frame generation its textures don't exist
                if (!handle && Protocol::IsFrameGenerationTexture(static_cast<Protocol::Texture>(i)))
                    continue;
                ok = ok && handle && SUCCEEDED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&images[i])));
            }
            auto fenceHandle = reinterpret_cast<HANDLE>(shared.FenceHandle);
            ok = ok && fenceHandle && SUCCEEDED(device->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&sharedFence)));
            CloseSharedHandles(shared);

            if (!ok)
            {
                ReleaseImports();
                return false;
            }

            width = w;
            height = h;
            outputWidth = ow;
            outputHeight = oh;
            frameGeneration = images[static_cast<size_t>(Protocol::Texture::Present)] && images[static_cast<size_t>(Protocol::Texture::Generated)];
            return true;
        }

        bool SubmitInputs(const Textures& inputs, uint64_t signalValue) override
        {
            // D3D9on12 records into command lists of its own, which have to reach the GPU before this queue can
            // wait for them. An event query is the D3D9 way to submit them.
            IDirect3DQuery9* query = nullptr;
            if (SUCCEEDED(device9->CreateQuery(D3DQUERYTYPE_EVENT, &query)) && query)
            {
                query->Issue(D3DISSUE_END);
                query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
                query->Release();
            }

            // Unwrapping makes the queue wait for the D3D9 work on the resource, and leaves it in the common state
            ID3D12Resource* sources[TextureCount]{};
            bool ok = true;
            for (size_t i = 0; i < TextureCount && ok; ++i)
            {
                if (!inputs[i])
                    continue;
                ok = images[i] && SUCCEEDED(on12->UnwrapUnderlyingResource(inputs[i], queue, IID_PPV_ARGS(&sources[i]))) && sources[i] &&
                    Matches(sources[i], images[i]);
            }

            if (ok)
            {
                auto& s = NextSlot();
                D3D12_RESOURCE_BARRIER before[TextureCount * 2];
                D3D12_RESOURCE_BARRIER after[TextureCount * 2];
                UINT barriers = 0;
                for (size_t i = 0; i < TextureCount; ++i)
                {
                    if (!sources[i])
                        continue;
                    before[barriers] = Transition(sources[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    after[barriers++] = Transition(sources[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    before[barriers] = Transition(images[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                    after[barriers++] = Transition(images[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                }
                s.list->ResourceBarrier(barriers, before);
                for (size_t i = 0; i < TextureCount; ++i)
                    if (sources[i])
                        Copy(s.list, images[i], sources[i]);
                s.list->ResourceBarrier(barriers, after);
                ok = SUCCEEDED(s.list->Close());

                if (ok)
                {
                    ID3D12CommandList* lists[] = { s.list };
                    queue->ExecuteCommandLists(1, lists);
                    queue->Signal(sharedFence, signalValue);
                }
                s.value = SignalCopies();
            }
            else
            {
                SignalCopies();
            }

            for (size_t i = 0; i < TextureCount; ++i)
                if (sources[i])
                    Return(inputs[i], sources[i]);
            return ok;
        }

        bool SubmitOutput(IDirect3DTexture9* target, Protocol::Texture index, uint64_t waitValue) override
        {
            auto source = images[static_cast<size_t>(index)];
            ID3D12Resource* destination = nullptr;
            if (!source || FAILED(on12->UnwrapUnderlyingResource(target, queue, IID_PPV_ARGS(&destination))) || !destination)
                return false;

            bool ok = Matches(destination, source);
            if (ok)
            {
                auto& s = NextSlot();
                D3D12_RESOURCE_BARRIER before[2] =
                {
                    Transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    Transition(destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
                };
                D3D12_RESOURCE_BARRIER after[2] =
                {
                    Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                    Transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                };
                s.list->ResourceBarrier(2, before);
                Copy(s.list, destination, source);
                s.list->ResourceBarrier(2, after);
                ok = SUCCEEDED(s.list->Close());

                if (ok)
                {
                    queue->Wait(sharedFence, waitValue);
                    ID3D12CommandList* lists[] = { s.list };
                    queue->ExecuteCommandLists(1, lists);
                }
                s.value = SignalCopies();
            }
            else
            {
                SignalCopies();
            }

            Return(target, destination);
            return ok;
        }
    };

    // ---------------------------------------------------------------------------------------------
    // Helper process

    class HelperProcess
    {
    public:
        HANDLE process = nullptr;
        HANDLE job = nullptr;
        HANDLE mapping = nullptr;
        HANDLE request = nullptr;
        HANDLE response = nullptr;
        Protocol::Shared* shared = nullptr;
        uint32_t serial = 0;

        bool Start(const std::filesystem::path& exe, const LUID& luid)
        {
            auto name = std::wstring(L"Local\\GTAIV.EFLC.FusionFix.Upscaler.") + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());

            mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Protocol::Shared), (name + Protocol::MappingSuffix).c_str());
            request = CreateEventW(nullptr, FALSE, FALSE, (name + Protocol::RequestSuffix).c_str());
            response = CreateEventW(nullptr, FALSE, FALSE, (name + Protocol::ResponseSuffix).c_str());
            if (!mapping || !request || !response)
                return false;

            shared = static_cast<Protocol::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Protocol::Shared)));
            if (!shared)
                return false;

            std::memset(shared, 0, sizeof(Protocol::Shared));
            shared->Version = Protocol::Version;
            shared->GameProcessId = GetCurrentProcessId();
            shared->AdapterLuidLow = luid.LowPart;
            shared->AdapterLuidHigh = luid.HighPart;
            wcsncpy_s(shared->GameDirectory, GetExeModulePath().wstring().c_str(), _TRUNCATE);
            wcsncpy_s(shared->PluginsDirectory, exe.parent_path().wstring().c_str(), _TRUNCATE);
            wcsncpy_s(shared->LogPath, FusionLog::PathFor("UpscalerHelper").wstring().c_str(), _TRUNCATE);

            // The helper ends with the game
            job = CreateJobObjectW(nullptr, nullptr);
            if (job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            }

            auto commandLine = L"\"" + exe.wstring() + L"\" " + Protocol::ArgumentName + L" " + name;
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                exe.parent_path().c_str(), &startup, &info))
                return false;

            if (job)
                AssignProcessToJobObject(job, info.hProcess);
            ResumeThread(info.hThread);
            CloseHandle(info.hThread);
            process = info.hProcess;
            return true;
        }

        // Returns true once the helper answered its start
        bool PollStarted(bool& ok)
        {
            if (WaitForSingleObject(response, 0) != WAIT_OBJECT_0)
            {
                if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                {
                    ok = false;
                    return true;
                }
                return false;
            }
            MemoryBarrier();
            ok = shared->ResponseStatus == Protocol::Status::Ok;
            return true;
        }

        // A request posted without waiting for its answer, which comes before the next one is written
        bool pending = false;

        void Post(Protocol::Command command)
        {
            shared->RequestCommand = command;
            shared->RequestSerial = ++serial;
            MemoryBarrier();
            SetEvent(request);
            pending = true;
        }

        enum class Answer { Ok, Failed, None };

        // The answer to the last request, None when the helper didn't give it in time or exited
        Answer Collect(DWORD timeout)
        {
            pending = false;
            auto start = GetTickCount64();
            while (true)
            {
                auto elapsed = static_cast<DWORD>(GetTickCount64() - start);
                if (elapsed >= timeout)
                    return Answer::None;

                HANDLE handles[] = { response, process };
                auto wait = WaitForMultipleObjects(2, handles, FALSE, timeout - elapsed);
                if (wait != WAIT_OBJECT_0)
                    return Answer::None;

                MemoryBarrier();
                // A late answer to an earlier request is skipped
                if (shared->ResponseSerial == serial)
                    return shared->ResponseStatus == Protocol::Status::Ok ? Answer::Ok : Answer::Failed;
            }
        }

        bool Request(Protocol::Command command, DWORD timeout)
        {
            // The helper reads a request's parameters before it answers: they can be written once it did
            if (pending && Collect(timeout) == Answer::None)
                return false;
            Post(command);
            return Collect(timeout) == Answer::Ok;
        }

        // graceful: ask the helper to release everything first, not while the game is being unloaded
        void Stop(bool graceful)
        {
            if (graceful && shared && process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
                Request(Protocol::Command::Shutdown, 500);
            if (process)
                CloseHandle(process);
            if (job)
                CloseHandle(job);
            process = job = nullptr;
        }

        // Before the helper is started again: its shared memory and events go with it
        void Close()
        {
            if (shared)
                UnmapViewOfFile(shared);
            for (auto handle : { mapping, request, response })
                if (handle)
                    CloseHandle(handle);
            shared = nullptr;
            mapping = request = response = nullptr;
            pending = false;
        }
    };

    // ---------------------------------------------------------------------------------------------

    enum class State
    {
        Idle, Starting, Ready, Failed
    };

    State state = State::Idle;
    DXVKBridge dxvkBridge;
    D3D12Bridge d3d12Bridge;
    Bridge* bridge = nullptr;
    HelperProcess helper;
    std::atomic<bool> dlssAvailable = false;
    std::atomic<bool> fsrAvailable = false;
    std::atomic<bool> frameGenerationAvailable = false;
    std::atomic<uint32_t> generation = 0;

    uint32_t configuredBackend = 0;
    uint32_t configuredWidth = 0;
    uint32_t configuredHeight = 0;
    uint32_t configuredOutputWidth = 0;
    uint32_t configuredOutputHeight = 0;
    uint32_t configuredPreset = 0;
    uint32_t configuredFlags = 0;
    bool configureFailed = false;
    uint64_t fenceValue = 0;
    uint64_t pendingOutputValue = 0;   // signalled by the helper for the request posted last
    Protocol::Command pendingCommand = Protocol::Command::None;

    // Frame generation: FrameId of the frames the helper prepared, Generate is asked once for the last one
    bool generationFailed = false;     // until the next configuration
    uint64_t frameId = 0;
    uint64_t preparedFrameId = 0;      // 0: this frame was not prepared
    uint64_t generatedFrameId = 0;
    bool preparedReset = false;
    bool preparedHudLess = false;
    bool generatedReset = false;       // the last Generate had nothing to interpolate from

    std::filesystem::path HelperPath()
    {
        return GetThisModulePath() / L"GTAIV.EFLC.FusionFix.exe";
    }

    bool FidelityFXPresent()
    {
        for (auto dir : { GetExeModulePath(), GetThisModulePath() })
            for (auto name : { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_dx12.dll" })
                if (std::filesystem::exists(dir / name))
                    return true;
        return false;
    }

    void Fail()
    {
        state = State::Failed;
        dlssAvailable = false;
        fsrAvailable = false;
        frameGenerationAvailable = false;
        ++generation;
        helper.Stop(true);
    }

    // The helper exited while it worked: it is started again, a few times, and the upscaler stays offered meanwhile.
    // Without this the choice vanished from the menu until the game was restarted.
    void Lost()
    {
        static uint32_t restarts = 0;
        if (restarts++ >= 3)
        {
            Log("The helper exited, it is not started again any more");
            Fail();
            return;
        }

        Log("The helper exited, starting it again");
        bridge->ReleaseImports();
        helper.Stop(false);
        helper.Close();
        configuredBackend = 0;
        configureFailed = false;
        generationFailed = false;
        preparedFrameId = 0;
        if (!helper.Start(HelperPath(), bridge->luid))
        {
            Log("The helper could not be started: error %lu", GetLastError());
            helper.Stop(false);
            Fail();
            return;
        }
        state = State::Starting;
    }

    const char* CommandName(Protocol::Command command)
    {
        return command == Protocol::Command::Generate ? "Generate" : "Evaluate";
    }

    // The request posted last without waiting is answered before anything else is written. The answer came long
    // ago, the helper answers once its GPU work is submitted. False: nothing can be asked of the helper now.
    bool CollectPending()
    {
        if (!helper.pending)
            return true;

        auto answer = helper.Collect(500);
        if (answer == HelperProcess::Answer::Ok)
            return true;

        static uint32_t reported = 0;
        bool exited = WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0;
        if (Report(reported))
            Log("%s %s%s", CommandName(pendingCommand), answer == HelperProcess::Answer::None ? "got no answer" : "failed", exited ? ", the helper exited" : "");
        // The copy of the output waits on the GPU for a value nobody will signal now
        if (answer == HelperProcess::Answer::None)
            bridge->SignalFromCpu(pendingOutputValue);
        if (exited)
        {
            Lost();
            return false;
        }
        if (answer == HelperProcess::Answer::None)
        {
            configureFailed = true;
            return false;
        }
        // A failed Generate leaves the upscaler working
        if (pendingCommand == Protocol::Command::Generate)
            generationFailed = true;
        else
            configureFailed = true;
        return true;
    }

    void Post(Protocol::Command command, uint64_t outputValue)
    {
        helper.Post(command);
        pendingCommand = command;
        pendingOutputValue = outputValue;
    }
}

export namespace Upscaler
{
    enum class Backend : uint32_t
    {
        DLSS = static_cast<uint32_t>(Protocol::Backend::DLSS),
        FSR = static_cast<uint32_t>(Protocol::Backend::FSR),
    };

    struct Frame
    {
        IDirect3DTexture9* Color = nullptr;
        IDirect3DTexture9* Depth = nullptr;   // R32F, standard [0, 1] depth
        IDirect3DTexture9* Motion = nullptr;  // G16R16F, previous - current in texture coordinates
        IDirect3DTexture9* Reactive = nullptr; // R16F, optional
        IDirect3DTexture9* Output = nullptr;  // A16B16G16R16F
        uint32_t Width = 0;           // render size of the inputs
        uint32_t Height = 0;
        uint32_t OutputWidth = 0;     // size of Output, 0 for the render size
        uint32_t OutputHeight = 0;
        float JitterX = 0.0f;
        float JitterY = 0.0f;
        float CameraNear = 0.1f;
        float CameraFar = 1000.0f;
        float CameraFovY = 1.0f;
        float FrameTimeMs = 16.6f;
        float Sharpness = 0.0f;
        uint32_t DLSSPreset = 0;
        bool Reset = false;
        // Frame generation: prepared with this frame's depth and motion vectors, Generate follows
        bool FrameGeneration = false;
        bool HighDynamicRange = false;    // the frame given to Generate is scRGB
        bool HudLess = false;             // Generate of this frame comes with the frame before the HUD
        float CameraPosition[3]{};        // world space
        float CameraUp[3]{};
        float CameraRight[3]{};
        float CameraForward[3]{};
    };

    bool IsAvailable(Backend backend)
    {
        return backend == Backend::DLSS ? dlssAvailable.load() : fsrAvailable.load();
    }

    // The helper has started or failed to: IsAvailable won't change on its own any more
    bool IsSettled()
    {
        return state == State::Ready || state == State::Failed;
    }

    // Changes whenever the availability does
    uint32_t Generation()
    {
        return generation.load();
    }

    // Render thread, every frame: starts the helper once the device is known
    void Update()
    {
        if (state == State::Idle)
        {
            auto device = RageDirect3DDevice9::m_pRealDevice ? *RageDirect3DDevice9::m_pRealDevice : nullptr;
            if (!device)
                return;

            state = State::Failed;
            if (!std::filesystem::exists(HelperPath()))
                return;

            // DXVK or D3D9on12: the D3D9 runtime itself can't share its textures with D3D12
            if (dxvkBridge.Init(device))
                bridge = &dxvkBridge;
            else if (d3d12Bridge.Init(device))
                bridge = &d3d12Bridge;
            else
            {
                Log("Neither DXVK nor D3D9on12 could share textures with the helper");
                return;
            }
            Log("%s, GPU vendor %04x%s", bridge == &dxvkBridge ? "DXVK" : "D3D9on12", bridge->vendorId, bridge->wine ? ", Wine" : "");

            // Nothing to offer: neither an NVIDIA GPU nor AMD's FidelityFX runtime
            if (bridge->vendorId != 0x10DE && !FidelityFXPresent())
                return;

            if (!helper.Start(HelperPath(), bridge->luid))
            {
                Log("The helper could not be started: error %lu", GetLastError());
                helper.Stop(false);
                return;
            }
            state = State::Starting;
        }

        if (state == State::Starting)
        {
            bool ok = false;
            if (!helper.PollStarted(ok))
                return;
            if (!ok)
            {
                Log("The helper failed to start: %s", helper.shared ? helper.shared->Message : "");
                Fail();
                return;
            }
            Log("Helper: %s", helper.shared->Message);
            dlssAvailable = helper.shared->DLSSAvailable != 0;
            fsrAvailable = helper.shared->FSRAvailable != 0;
            frameGenerationAvailable = helper.shared->FrameGenerationAvailable != 0;
            state = State::Ready;
            ++generation;
        }
    }

    // Render thread: upscales Frame.Color into Frame.Output, false leaves Output untouched
    bool Evaluate(Backend backend, const Frame& frame)
    {
        preparedFrameId = 0;
        if (state != State::Ready || !IsAvailable(backend) || !frame.Color || !frame.Depth || !frame.Motion || !frame.Output)
            return false;

        if (!CollectPending())
            return false;

        auto backendId = static_cast<uint32_t>(backend);
        auto flags = frame.Reactive ? Protocol::ConfigureFlags::ReactiveMask : 0u;
        if (bridge->wine)
            flags |= Protocol::ConfigureFlags::Wine;
        if (frame.FrameGeneration && frameGenerationAvailable)
            flags |= Protocol::ConfigureFlags::FrameGeneration | (frame.HighDynamicRange ? Protocol::ConfigureFlags::HighDynamicRange : 0u);
        auto outputWidth = frame.OutputWidth ? frame.OutputWidth : frame.Width;
        auto outputHeight = frame.OutputHeight ? frame.OutputHeight : frame.Height;
        bool reconfigure = configuredBackend != backendId || configuredWidth != frame.Width || configuredHeight != frame.Height ||
            configuredOutputWidth != outputWidth || configuredOutputHeight != outputHeight ||
            configuredPreset != frame.DLSSPreset || configuredFlags != flags;
        if (reconfigure)
        {
            bridge->ReleaseImports();
            configuredBackend = backendId;
            configuredWidth = frame.Width;
            configuredHeight = frame.Height;
            configuredOutputWidth = outputWidth;
            configuredOutputHeight = outputHeight;
            configuredPreset = frame.DLSSPreset;
            configuredFlags = flags;
            configureFailed = true;

            auto& shared = *helper.shared;
            shared.ConfigureBackend = static_cast<Protocol::Backend>(backendId);
            shared.Width = frame.Width;
            shared.Height = frame.Height;
            shared.OutputWidth = outputWidth;
            shared.OutputHeight = outputHeight;
            shared.DLSSPreset = frame.DLSSPreset;
            shared.Flags = flags;
            shared.FenceHandle = 0;
            if (bridge->PrepareGameFence(shared, helper.process))
                shared.Flags |= Protocol::ConfigureFlags::GameFence;
            if (!helper.Request(Protocol::Command::Configure, 15000))
            {
                bool exited = WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0;
                Log("Configure %ux%u -> %ux%u failed%s: %s", frame.Width, frame.Height, outputWidth, outputHeight,
                    exited ? ", the helper exited" : "", shared.Message);
                if (exited)
                    Lost();
                return false;
            }
            bridge->gameFence = (shared.Flags & Protocol::ConfigureFlags::GameFence) != 0;
            if (bridge->wine)
                Log("Synchronization: %s", bridge->gameFence ? "the game's semaphore, on the GPU" : "on the CPU");
            if ((flags & Protocol::ConfigureFlags::FrameGeneration) && !(shared.Flags & Protocol::ConfigureFlags::FrameGeneration))
                Log("Frame generation could not be set up, the upscaler works without it");
            if (!bridge->Import(shared, frame.Width, frame.Height, outputWidth, outputHeight))
            {
                Log("Configure %ux%u -> %ux%u: the shared resources could not be imported", frame.Width, frame.Height, outputWidth, outputHeight);
                return false;
            }
            Log("Configured: %s", shared.Message);

            configureFailed = false;
            generationFailed = false;
            fenceValue = 0;
        }
        else if (configureFailed)
        {
            // A failed configuration is not retried every frame, only when something changes
            return false;
        }

        auto inputValue = ++fenceValue;
        auto outputValue = ++fenceValue;

        Textures inputs{ frame.Color, frame.Depth, frame.Motion, frame.Reactive };
        if (!bridge->SubmitInputs(inputs, inputValue))
        {
            // Nothing was submitted, the helper was not asked for this frame. Retried when the mode or size changes.
            static uint32_t reported = 0;
            if (Report(reported))
                Log("The inputs could not be submitted");
            configureFailed = true;
            return false;
        }

        auto& shared = *helper.shared;
        shared.WaitValue = inputValue;
        shared.SignalValue = outputValue;
        shared.JitterX = frame.JitterX;
        shared.JitterY = frame.JitterY;
        shared.MotionScaleX = static_cast<float>(frame.Width);
        shared.MotionScaleY = static_cast<float>(frame.Height);
        shared.CameraNear = frame.CameraNear;
        shared.CameraFar = frame.CameraFar;
        shared.CameraFovY = frame.CameraFovY;
        shared.FrameTimeMs = frame.FrameTimeMs;
        shared.Sharpness = frame.Sharpness;
        shared.Reset = frame.Reset ? 1 : 0;
        for (int i = 0; i < 3; ++i)
        {
            shared.CameraPosition[i] = frame.CameraPosition[i];
            shared.CameraUp[i] = frame.CameraUp[i];
            shared.CameraRight[i] = frame.CameraRight[i];
            shared.CameraForward[i] = frame.CameraForward[i];
        }
        shared.FrameId = ++frameId;
        shared.HudLess = frame.HudLess ? 1 : 0;

        // The helper prepares the frame generation with this frame
        bool prepared = bridge->frameGeneration && !generationFailed;
        auto prepare = [&]()
        {
            if (!prepared)
                return;
            preparedFrameId = frameId;
            preparedReset = frame.Reset;
            preparedHudLess = frame.HudLess;
        };

        // Synchronized on the GPU: the copy of the output waits there for outputValue, and the answer is collected
        // next time
        if (!bridge->WaitOnCpu())
        {
            Post(Protocol::Command::Evaluate, outputValue);
            prepare();
            return bridge->SubmitOutput(frame.Output, Protocol::Texture::Output, outputValue);
        }

        // On the CPU: the helper answers once its GPU work, which signals outputValue, has finished
        if (!helper.Request(Protocol::Command::Evaluate, 500))
        {
            static uint32_t reported = 0;
            bool exited = WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0;
            if (Report(reported))
                Log("Evaluate failed%s", exited ? ", the helper exited" : "");
            if (exited)
                Lost();
            else
                configureFailed = true;
            return false;
        }

        prepare();
        return bridge->SubmitOutput(frame.Output, Protocol::Texture::Output, outputValue);
    }

    // The frame generation is set up and prepared with this frame's Evaluate
    bool IsFrameGenerationReady()
    {
        return state == State::Ready && preparedFrameId != 0 && bridge && bridge->frameGeneration && !generationFailed;
    }

    // The last generated frame has nothing of the previous one: not worth showing
    bool WasGenerateReset()
    {
        return generatedReset;
    }

    bool IsFrameGenerationAvailable()
    {
        return frameGenerationAvailable.load();
    }

    // Render thread, after the frame is finished: present is the frame at the output size (A16B16G16R16F, sRGB encoded
    // or scRGB), hudLess the same before the HUD, when Evaluate was told it comes; generated receives the frame between
    // the previous one and it. False leaves generated untouched.
    bool Generate(IDirect3DTexture9* present, IDirect3DTexture9* hudLess, IDirect3DTexture9* generated, float maxLuminance)
    {
        if (!IsFrameGenerationReady() || !present || !generated)
            return false;
        auto id = preparedFrameId;
        preparedFrameId = 0;

        if (!CollectPending() || generationFailed)
            return false;

        auto inputValue = ++fenceValue;
        auto outputValue = ++fenceValue;

        Textures inputs{};
        inputs[static_cast<size_t>(Protocol::Texture::Present)] = present;
        // What Evaluate was told: without the frame before the HUD, the HUD can't be told apart and is interpolated
        inputs[static_cast<size_t>(Protocol::Texture::HudLess)] = preparedHudLess ? (hudLess ? hudLess : present) : nullptr;
        if (!bridge->SubmitInputs(inputs, inputValue))
        {
            static uint32_t reported = 0;
            if (Report(reported))
                Log("The frame for the frame generation could not be submitted");
            generationFailed = true;
            return false;
        }

        auto& shared = *helper.shared;
        shared.WaitValue = inputValue;
        shared.SignalValue = outputValue;
        shared.FrameId = id;
        shared.MaxLuminance = maxLuminance;
        // The frame before this one was not generated from: there is nothing to interpolate from
        shared.GenerateReset = preparedReset || generatedFrameId + 1 != id ? 1 : 0;
        generatedReset = shared.GenerateReset != 0;
        generatedFrameId = id;

        if (!bridge->WaitOnCpu())
        {
            Post(Protocol::Command::Generate, outputValue);
            return bridge->SubmitOutput(generated, Protocol::Texture::Generated, outputValue);
        }

        if (!helper.Request(Protocol::Command::Generate, 500))
        {
            static uint32_t reported = 0;
            bool exited = WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0;
            if (Report(reported))
                Log("Generate failed%s", exited ? ", the helper exited" : "");
            if (exited)
                Lost();
            else
                generationFailed = true;
            return false;
        }
        return bridge->SubmitOutput(generated, Protocol::Texture::Generated, outputValue);
    }

    // The job object ends the helper with the game, and the helper also watches the game process
    void Shutdown()
    {
        helper.Stop(false);
    }
}
