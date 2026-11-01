#pragma once

// Shared between the game plugin (x86) and GTAIV.EFLC.FusionFix.exe (x64), which runs DLSS and FSR.
//
// The game creates a file mapping holding UpscalerProtocol::Shared and two auto-reset events, and starts
// the helper with their base name. Every request is: the game writes the parameters and the command,
// signals <name>.Request and waits for <name>.Response. The helper answers each request exactly once.
//
// GPU synchronization uses two D3D12 fences created by the helper, which the game opens on its own queue:
// imported as Vulkan timeline semaphores with DXVK, or opened on the D3D12 device of D3D9on12. The game
// signals WaitValue on the input fence once the inputs are copied into the shared textures, the helper waits
// for it on its queue, runs the upscaler and signals SignalValue on the output fence, which the game waits for
// before copying the output back. One writer each, so each fence only goes up: with a single fence the game's
// next inputs could signal it past a Generate still running on the helper's compute queue, which let the copy
// of the generated frame through early and then took the fence back down. The helper answers Evaluate after
// its GPU work is submitted, so the game never waits on the GPU for a value that nobody will signal.
//
// Under Wine (ConfigureFlags::Wine) the shared textures have no UAV flag, and the game imports them as opaque
// Win32 handles, which every Wine supports. Only textures can be shared: vkd3d-proton gives a buffer an empty
// handle. Wine can't import a D3D12 fence of another process (it crashes the importer), so the game shares two
// Vulkan timeline semaphores of its own instead (ConfigureFlags::GameFence), which the helper opens as its fences.
// Where the helper can't open them (older Proton), it clears GameFence in Flags, and then both sides wait for their
// GPU work on the CPU: the game for its copies before Evaluate, the helper before it answers Evaluate.
//
// Presentation by the helper (ConfigureFlags::HelperPresent, not under Wine): the helper puts a child window of its own over
// the game's window (PresentWindow) and a flip model swap chain on it. Present takes the finished frame (Present, as for
// the frame generation) and shows it from a thread of its own; the game leaves its own Present out and asks for Hide
// when it presents a frame itself: the helper hides its window (ChildWindow), and a Present still queued then does not show it again. With PresentGenerate the helper also generates the frame between the one
// before and this one (HudLess too, FrameId, MaxLuminance and GenerateReset as for Generate) and shows it first, the
// rendered frame PresentDelay of the frame time after it. The helper signals SignalValue on its present fence
// (PresentFenceHandle) once it is done with Present and HudLess, which the game's next copy into them waits for. With
// AmdPacing, AMD's frame generation swap chain generates the frame in its Present and paces both (PresentDelay unused).
//
// Vulkan (ConfigureFlags::Vulkan, FSR with DXVK only): the helper runs FSR through AMD's Vulkan runtime on a Vulkan device of
// its own. The shared textures are Vulkan images exported as opaque Win32 handles with VulkanImageUsage, which the game's
// imports repeat, and the fences are the game's timeline semaphores (GameFence) as under Wine, or without them both sides
// wait on the CPU. No presentation and no compute queue there. Where the helper can't (no Vulkan 1.3, no Vulkan runtime of
// AMD's), it clears Vulkan, and GameFence too outside Wine, and goes on with D3D12.
//
// Frame generation (ConfigureFlags::FrameGeneration, FSR only): Evaluate also prepares the frame generation with
// the depth and motion vectors of the frame. Generate then takes the finished frame (Present, the size of the
// output) and writes the frame between it and the previous one into Generated, synchronized like Evaluate.

#include <cstdint>
#include <cstddef>

namespace UpscalerProtocol
{
    constexpr uint32_t Version = 18;
    constexpr uint32_t PathLength = 520;

    constexpr const wchar_t* ArgumentName = L"--upscaler";

    enum class Command : uint32_t
    {
        None,
        Configure,      // (re)create the shared textures and the upscaler for Backend, Width x Height to OutputWidth x OutputHeight
        Evaluate,       // upscale one frame
        Generate,       // the frame between the last Present and the one before, after an Evaluate of the same frame
        Shutdown,
        Present,        // ConfigureFlags::HelperPresent: Present to the screen, through the helper's swap chain
        Hide,           // ConfigureFlags::HelperPresent: the game presents itself, the helper's window hidden until the next Present
    };

    enum class Backend : uint32_t
    {
        None,
        DLSS,
        FSR,
    };

    enum class Status : uint32_t
    {
        Pending,
        Ok,
        Failed,
    };

    // Shared textures, the inputs at the render size (Width x Height), the output at OutputWidth x OutputHeight:
    // Color     DXGI_FORMAT_R16G16B16A16_FLOAT  HDR scene
    // Depth     DXGI_FORMAT_R32_FLOAT           standard [0, 1] depth
    // Motion    DXGI_FORMAT_R16G16_FLOAT        previous - current position in texture coordinates, no jitter
    // Reactive  DXGI_FORMAT_R16_FLOAT           0 to 1, how much a pixel should follow the current frame
    // Output    DXGI_FORMAT_R16G16B16A16_FLOAT  written by the upscaler
    // Frame generation or presentation by the helper (Present only) only, at the output size, otherwise their handles are 0; DXGI_FORMAT_B8G8R8A8_UNORM instead with
    // ConfigureFlags::EightBitFrames:
    // Present   DXGI_FORMAT_R16G16B16A16_FLOAT  the finished frame, sRGB encoded or scRGB with HDR output
    // Generated DXGI_FORMAT_R16G16B16A16_FLOAT  written by the frame generation
    // HudLess   DXGI_FORMAT_R16G16B16A16_FLOAT  Present before the HUD was drawn, which tells the HUD apart
    enum class Texture : uint32_t
    {
        Color, Depth, Motion, Reactive, Output, Present, Generated, HudLess, Count
    };

    // The inputs the game copies every frame
    constexpr uint32_t InputCount = static_cast<uint32_t>(Texture::Output);

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

    namespace ConfigureFlags
    {
        constexpr uint32_t ReactiveMask = 1 << 0;   // Reactive is written every frame and used by FSR
        constexpr uint32_t Wine = 1 << 1;           // textures without the UAV flag, for opaque handles
        constexpr uint32_t GameFence = 1 << 2;      // the fence handles are the game's semaphores; cleared by the helper if it can't open them
        constexpr uint32_t FrameGeneration = 1 << 3; // FSR frame generation; cleared by the helper if it can't create it
        constexpr uint32_t HighDynamicRange = 1 << 4; // Present is scRGB
        constexpr uint32_t EightBitFrames = 1 << 5;   // the frame generation's textures are B8G8R8A8_UNORM; cleared by the helper if the GPU can't write them
        constexpr uint32_t AsyncGeneration = 1 << 6;  // Generate runs on a compute queue of the helper; cleared if it has none
        constexpr uint32_t HelperPresent = 1 << 7;    // the helper presents the frames, with the frame generation too; cleared if it can't
        constexpr uint32_t AmdPacing = 1 << 8;        // with HelperPresent and the frame generation, AMD's swap chain generates and paces the frames; cleared if it can't
        constexpr uint32_t Vulkan = 1 << 9;           // FSR on the helper's Vulkan device; cleared if it can't, with GameFence outside Wine
    }

    // ConfigureFlags::Vulkan: the usage the helper makes a shared image with, which the game's import of it repeats, as
    // VkImageUsageFlags (transfer source 0x1, transfer destination 0x2, sampled 0x4, storage 0x8): the upscaler and the
    // frame generation write Output and Generated
    constexpr uint32_t VulkanImageUsage(Texture texture)
    {
        return 0x1u | 0x2u | 0x4u | (texture == Texture::Output || texture == Texture::Generated ? 0x8u : 0u);
    }

#pragma pack(push, 8)
    struct Shared
    {
        uint32_t Version;
        uint32_t GameProcessId;

        // Written by the game before starting the helper
        uint32_t AdapterLuidLow;
        int32_t AdapterLuidHigh;
        wchar_t GameDirectory[PathLength];
        wchar_t PluginsDirectory[PathLength];
        wchar_t LogPath[PathLength];

        // Written by the helper, answering the implicit start request
        uint32_t DLSSAvailable;
        uint32_t FSRAvailable;
        uint32_t FrameGenerationAvailable;
        uint32_t Reserved0;
        char Message[512];

        // Request
        Command RequestCommand;
        uint32_t RequestSerial;

        // Configure
        Backend ConfigureBackend;
        uint32_t Width;
        uint32_t Height;
        uint32_t DLSSPreset;          // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 is the default
        uint32_t Flags;               // ConfigureFlags, GameFence cleared by the helper when it falls back to the CPU
        uint32_t OutputWidth;         // upscaled size, Width x Height or larger
        uint32_t OutputHeight;
        uint32_t Reserved;

        // Evaluate
        uint64_t WaitValue;           // signalled by the game when the inputs are ready
        uint64_t SignalValue;         // signalled by the helper when the output is ready
        float JitterX;                // pixels, direction the rendered content moved (y down)
        float JitterY;
        float MotionScaleX;           // converts motion vectors to pixels
        float MotionScaleY;
        float CameraNear;
        float CameraFar;
        float CameraFovY;             // radians
        float FrameTimeMs;
        float Sharpness;
        uint32_t Reset;
        // World space, for the frame generation
        float CameraPosition[3];
        float CameraUp[3];
        float CameraRight[3];
        float CameraForward[3];
        uint64_t FrameId;             // +1 every frame, anything else resets the frame generation
        uint32_t HudLess;             // Generate of this frame comes with HudLess
        uint32_t Reserved1;

        // Generate (WaitValue and SignalValue as for Evaluate)
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

        // Response
        uint32_t ResponseSerial;
        Status ResponseStatus;

        // Configure results, handles already duplicated into the game process
        uint64_t TextureHandles[static_cast<size_t>(Texture::Count)];
        uint64_t TextureSizes[static_cast<size_t>(Texture::Count)];
        // The helper's fences; with GameFence, the game's semaphores duplicated into the helper
        uint64_t InputFenceHandle;    // signalled by the game: WaitValue
        uint64_t OutputFenceHandle;   // signalled by the helper: SignalValue
    };
#pragma pack(pop)

    static_assert(sizeof(wchar_t) == 2);
    static_assert(offsetof(Shared, WaitValue) % 8 == 0);
    static_assert(offsetof(Shared, TextureHandles) % 8 == 0);
    static_assert(sizeof(Shared) == 4024, "The layout must be identical in the x86 and x64 builds");

    inline const wchar_t* MappingSuffix = L".Mapping";
    inline const wchar_t* RequestSuffix = L".Request";
    inline const wchar_t* ResponseSuffix = L".Response";
}
