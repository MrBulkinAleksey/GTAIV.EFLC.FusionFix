#pragma once

// Shared between the game plugin (x86) and GTAIV.EFLC.FusionFix.exe (x64), which runs DLSS and FSR.
//
// The base is the upscaler bridge protocol of the fixes (UpscalerBridge, <upscaler/upscaler_bridge_protocol.h>): the game
// creates its file mapping (Shared) and two auto-reset events and starts the helper with their base name; every request is
// the parameters and the command written, <name>.Request signalled, <name>.Response waited for, and the helper answers each
// exactly once. The helper serves a plain bridge client too, as FusionFixHub.exe would: Configure, Evaluate and Shutdown
// on one fence (Shared::FenceHandle), which the client signals WaitValue on and the helper SignalValue.
//
// What the bridge has no room for is in a second mapping, <name>.FusionFix (Extension), made by the game next to the
// first; the helper opens it when it is there. Its commands (Generate, Present, Hide) take numbers the bridge leaves free.
//
// GPU synchronization with the extension uses two D3D12 fences created by the helper, which the game opens on its own
// queue: imported as Vulkan timeline semaphores with DXVK, or opened on the D3D12 device of D3D9on12. The game signals
// WaitValue on the input fence (Extension::InputFenceHandle) once the inputs are copied into the shared textures, the
// helper waits for it on its queue, runs the upscaler and signals SignalValue on the output fence (Shared::FenceHandle),
// which the game waits for before copying the output back. One writer each, so each fence only goes up: with a single
// fence the game's next inputs could signal it past a Generate still running on the helper's compute queue, which let the
// copy of the generated frame through early and then took the fence back down. The helper answers Evaluate after its GPU
// work is submitted, so the game never waits on the GPU for a value that nobody will signal.
//
// Under Wine (ExtensionFlags::Wine) the shared textures have no UAV flag, and the game imports them as opaque Win32
// handles, which every Wine supports. Only textures can be shared: vkd3d-proton gives a buffer an empty handle. Wine can't
// import a D3D12 fence of another process (it crashes the importer), so the game shares two Vulkan timeline semaphores of
// its own instead (ExtensionFlags::GameFence: the input one in InputFenceHandle, the output one in Shared::FenceHandle),
// which the helper opens as its fences. Where the helper can't open them (older Proton), it clears GameFence, and then both
// sides wait for their GPU work on the CPU: the game for its copies before Evaluate, the helper before it answers Evaluate.
//
// Presentation by the helper (ExtensionFlags::HelperPresent, not under Wine): the helper puts a child window of its own
// over the game's window (PresentWindow) and a flip model swap chain on it. Present takes the finished frame (Present, as
// for the frame generation) and shows it from a thread of its own; the game leaves its own Present out and asks for Hide
// when it presents a frame itself: the helper hides its window (ChildWindow), and a Present still queued then does not
// show it again. With PresentGenerate the helper also generates the frame between the one before and this one (HudLess
// too, FrameId, MaxLuminance and GenerateReset as for Generate) and shows it first, the rendered frame PresentDelay of the
// frame time after it. The helper signals SignalValue on its present fence (PresentFenceHandle) once it is done with
// Present and HudLess, which the game's next copy into them waits for. With AmdPacing, AMD's frame generation swap chain
// generates the frame in its Present and paces both (PresentDelay unused).
//
// Frame generation (ExtensionFlags::FrameGeneration, FSR only): Evaluate also prepares the frame generation with the depth
// and motion vectors of the frame. Generate then takes the finished frame (Present, the size of the output) and writes the
// frame between it and the previous one into Generated, synchronized like Evaluate.

#include <cstdint>
#include <cstddef>
#include <upscaler/upscaler_bridge_protocol.h>

namespace UpscalerProtocol
{
    using UpscalerBridge::Shared;
    using UpscalerBridge::Backend;
    using UpscalerBridge::Status;
    using UpscalerBridge::BackendBit;
    using UpscalerBridge::PathLength;
    using UpscalerBridge::ArgumentName;
    using UpscalerBridge::MappingSuffix;
    using UpscalerBridge::RequestSuffix;
    using UpscalerBridge::ResponseSuffix;
    namespace ConfigureFlags = UpscalerBridge::ConfigureFlags;

    constexpr uint32_t Version = UpscalerBridge::Version;
    constexpr uint32_t ExtensionVersion = 1;
    inline const wchar_t* ExtensionSuffix = L".FusionFix";

    // The bridge's commands with their numbers, then the extension's
    enum class Command : uint32_t
    {
        None = static_cast<uint32_t>(UpscalerBridge::Command::None),
        Configure = static_cast<uint32_t>(UpscalerBridge::Command::Configure),
        Evaluate = static_cast<uint32_t>(UpscalerBridge::Command::Evaluate),
        Shutdown = static_cast<uint32_t>(UpscalerBridge::Command::Shutdown),
        Generate = 0x100,   // the frame between the last Present and the one before, after an Evaluate of the same frame
        Present,            // ExtensionFlags::HelperPresent: Present to the screen, through the helper's swap chain
        Hide,               // ExtensionFlags::HelperPresent: the game presents itself, the helper's window hidden until the next Present
    };

    // The bridge's textures (their handles in Shared), then the extension's, at the output size, in Extension; their
    // handles are 0 without the frame generation or presentation by the helper (Present only). DXGI_FORMAT_B8G8R8A8_UNORM
    // instead of the float format with ExtensionFlags::EightBitFrames:
    // Present   DXGI_FORMAT_R16G16B16A16_FLOAT  the finished frame, sRGB encoded or scRGB with HDR output
    // Generated DXGI_FORMAT_R16G16B16A16_FLOAT  written by the frame generation
    // HudLess   DXGI_FORMAT_R16G16B16A16_FLOAT  Present before the HUD was drawn, which tells the HUD apart
    enum class Texture : uint32_t
    {
        Color, Depth, Motion, Reactive, Output, Present, Generated, HudLess, Count
    };

    constexpr uint32_t BridgeTextureCount = static_cast<uint32_t>(UpscalerBridge::Texture::Count);
    constexpr uint32_t ExtensionTextureCount = static_cast<uint32_t>(Texture::Count) - BridgeTextureCount;
    static_assert(static_cast<uint32_t>(Texture::Output) == static_cast<uint32_t>(UpscalerBridge::Texture::Output));

    // The inputs the game copies every frame
    constexpr uint32_t InputCount = UpscalerBridge::InputCount;

    // The textures of the frame generation, the last ones
    constexpr bool IsFrameGenerationTexture(Texture texture)
    {
        return texture == Texture::Present || texture == Texture::Generated || texture == Texture::HudLess;
    }

    // At the output size, the others at the render size
    constexpr bool IsOutputSize(Texture texture)
    {
        return texture == Texture::Output || IsFrameGenerationTexture(texture);
    }

    namespace ExtensionFlags
    {
        constexpr uint32_t Wine = 1 << 0;             // textures without the UAV flag, for opaque handles
        constexpr uint32_t GameFence = 1 << 1;        // the fence handles are the game's semaphores; cleared by the helper if it can't open them
        constexpr uint32_t FrameGeneration = 1 << 2;  // FSR frame generation; cleared by the helper if it can't create it
        constexpr uint32_t HighDynamicRange = 1 << 3; // Present is scRGB
        constexpr uint32_t EightBitFrames = 1 << 4;   // the frame generation's textures are B8G8R8A8_UNORM; cleared by the helper if the GPU can't write them
        constexpr uint32_t AsyncGeneration = 1 << 5;  // Generate runs on a compute queue of the helper; cleared if it has none
        constexpr uint32_t HelperPresent = 1 << 6;    // the helper presents the frames, with the frame generation too; cleared if it can't
        constexpr uint32_t AmdPacing = 1 << 7;        // with HelperPresent and the frame generation, AMD's swap chain generates and paces the frames; cleared if it can't
    }

#pragma pack(push, 8)
    struct Extension
    {
        uint32_t Version;             // ExtensionVersion, written by the game
        uint32_t Flags;               // ExtensionFlags, Configure; cleared by the helper as above

        // Written by the game before starting the helper: the vendor runtimes are looked for here too
        wchar_t PluginsDirectory[PathLength];

        // Written by the helper, answering the implicit start request
        uint32_t FrameGenerationAvailable;
        uint32_t Reserved0;

        // Evaluate, world space, for the frame generation
        float CameraPosition[3];
        float CameraUp[3];
        float CameraRight[3];
        float CameraForward[3];
        uint64_t FrameId;             // +1 every frame, anything else resets the frame generation
        uint32_t HudLess;             // Generate of this frame comes with HudLess
        uint32_t Reserved1;

        // Generate (WaitValue and SignalValue of Shared as for Evaluate)
        float MaxLuminance;           // nits, HDR output
        uint32_t GenerateReset;

        // Presentation by the helper. Configure: the game's window; answer: the helper's child window over it and its
        // present fence, duplicated into the game.
        uint64_t PresentWindow;
        uint64_t ChildWindow;
        uint64_t PresentFenceHandle;
        // Present (WaitValue on the input fence; SignalValue on the present fence once Present and HudLess are copied out)
        uint32_t SyncInterval;        // 0: no wait for the vertical blank, torn where the display allows it
        uint32_t PresentGenerate;     // the generated frame first
        float PresentDelay;           // of the frame time, from the generated frame to the rendered one
        uint32_t Reserved2;

        // Configure results, handles already duplicated into the game process: Present, Generated, HudLess
        uint64_t TextureHandles[ExtensionTextureCount];
        uint64_t TextureSizes[ExtensionTextureCount];
        // Signalled by the game (WaitValue): the helper's input fence, or with GameFence the game's semaphore duplicated
        // into the helper. The output fence is Shared::FenceHandle.
        uint64_t InputFenceHandle;
    };
#pragma pack(pop)

    static_assert(sizeof(wchar_t) == 2);
    static_assert(offsetof(Extension, FrameId) % 8 == 0);
    static_assert(offsetof(Extension, TextureHandles) % 8 == 0);
    static_assert(sizeof(Extension) == 1224, "The layout must be identical in the x86 and x64 builds");

    // A texture's handle and size, in Shared or Extension by its number
    inline uint64_t& TextureHandle(Shared& shared, Extension& extension, Texture texture)
    {
        auto i = static_cast<uint32_t>(texture);
        return i < BridgeTextureCount ? shared.TextureHandles[i] : extension.TextureHandles[i - BridgeTextureCount];
    }

    inline uint64_t& TextureSize(Shared& shared, Extension& extension, Texture texture)
    {
        auto i = static_cast<uint32_t>(texture);
        return i < BridgeTextureCount ? shared.TextureSizes[i] : extension.TextureSizes[i - BridgeTextureCount];
    }
}
