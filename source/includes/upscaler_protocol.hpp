#pragma once

// Shared between the game plugin (x86) and GTAIV.EFLC.FusionFix.exe (x64), which runs DLSS and FSR.
//
// The game creates a file mapping holding UpscalerProtocol::Shared and two auto-reset events, and starts
// the helper with their base name. Every request is: the game writes the parameters and the command,
// signals <name>.Request and waits for <name>.Response. The helper answers each request exactly once.
//
// GPU synchronization uses a D3D12 fence created by the helper, which the game opens on its own queue:
// imported as a Vulkan timeline semaphore with DXVK, or opened on the D3D12 device of D3D9on12. The game
// signals WaitValue once the inputs are copied into the shared textures, the helper waits for it on its
// queue, runs the upscaler and signals SignalValue, which the game waits for before copying the output
// back. The helper answers Evaluate after its GPU work is submitted, so the game never waits on the GPU
// for a value that nobody will signal.
//
// Under Wine (ConfigureFlags::Wine) the shared textures have no UAV flag, and the game imports them as opaque
// Win32 handles, which every Wine supports. Only textures can be shared: vkd3d-proton gives a buffer an empty
// handle. Wine can't import a D3D12 fence of another process (it crashes the importer), so the game shares a
// Vulkan timeline semaphore of its own instead (ConfigureFlags::GameFence), which the helper opens as its fence.
// Where the helper can't open it (older Proton), it clears GameFence in Flags, and then both sides wait for their
// GPU work on the CPU: the game for its copies before Evaluate, the helper before it answers Evaluate.
//
// Frame generation (ConfigureFlags::FrameGeneration, FSR only): Evaluate also prepares the frame generation with
// the depth and motion vectors of the frame. Generate then takes the finished frame (Present, the size of the
// output) and writes the frame between it and the previous one into Generated, synchronized like Evaluate.

#include <cstdint>
#include <cstddef>

namespace UpscalerProtocol
{
    constexpr uint32_t Version = 8;
    constexpr uint32_t PathLength = 520;

    constexpr const wchar_t* ArgumentName = L"--upscaler";

    enum class Command : uint32_t
    {
        None,
        Configure,      // (re)create the shared textures and the upscaler for Backend, Width x Height to OutputWidth x OutputHeight
        Evaluate,       // upscale one frame
        Generate,       // the frame between the last Present and the one before, after an Evaluate of the same frame
        Shutdown,
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
    // Frame generation only, at the output size, otherwise their handles are 0:
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
        constexpr uint32_t GameFence = 1 << 2;      // FenceHandle is the game's semaphore; cleared by the helper if it can't open it
        constexpr uint32_t FrameGeneration = 1 << 3; // FSR frame generation; cleared by the helper if it can't create it
        constexpr uint32_t HighDynamicRange = 1 << 4; // Present is scRGB
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

        // Response
        uint32_t ResponseSerial;
        Status ResponseStatus;

        // Configure results, handles already duplicated into the game process
        uint64_t TextureHandles[static_cast<size_t>(Texture::Count)];
        uint64_t TextureSizes[static_cast<size_t>(Texture::Count)];
        uint64_t FenceHandle;         // the helper's fence; with GameFence, the game's semaphore duplicated into the helper
    };
#pragma pack(pop)

    static_assert(sizeof(wchar_t) == 2);
    static_assert(offsetof(Shared, WaitValue) % 8 == 0);
    static_assert(offsetof(Shared, TextureHandles) % 8 == 0);
    static_assert(sizeof(Shared) == 3976, "The layout must be identical in the x86 and x64 builds");

    inline const wchar_t* MappingSuffix = L".Mapping";
    inline const wchar_t* RequestSuffix = L".Request";
    inline const wchar_t* ResponseSuffix = L".Response";
}
