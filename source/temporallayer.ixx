module;

#include <common.hxx>
#include "FusionLog.hpp"

export module temporallayer;

export import temporal;
import common;
import comvars;
import coronas;
import d3dx9_43;
import screenquad;
import framegeneration;
import hdr;
import renderscale;
import settings;
import upscaler;
import inireload;

#define IDR_TEMPORAL_PS_OPAQUE_LUMA     3109
#define IDR_TEMPORAL_PS_REACTIVE        3110
#define IDR_TEMPORAL_PS_RAIN_LAYER      3111
#define IDR_TEMPORAL_PS_RAIN_COMPOSITE  3112
#define IDR_TEMPORAL_PS_STEADY_DEPTH    3113

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) { if (p) { (p)->Release(); (p)=NULL; } }
#endif

// What Fusion Fix adds to upstream's temporal anti-aliasing (temporal.ixx, kept as upstream has it but for the lines
// marked "FusionFix layer", which call in here through TemporalHooks).
//
// temporal.ixx jitters the camera, marks the scene's moving entities, gives every G-buffer draw its previous transform
// and bones, so the deferred shaders write their motion next to the material ID (_STENCIL_BUFFER_ g and b,
// shaders/patches/aa_motion_vectors.patch), copies the depth at the G-buffer's end and resolves temporal AA at the
// render size. This layer adds:
// - The camera of the scene for the other effects (CurrentCamera, PreviousCamera, SceneFrame: SSR, SSGI, AO and the
//   rest reproject their histories with it), whatever the antialiasing.
// - The motion vectors as a texture (GetMotionVectors), previous minus current position in texture coordinates without
//   jitter, made from the G-buffer's with temporal.ixx's BridgeMotion right after its depth copy: for DLSS and FSR and
//   the other effects' histories.
// - DLAA and FSR through our helper (upscaler.ixx), at a render scale too, with frame generation; temporal.ixx's own
//   bridge to FusionFixHub is left to it when that is the one there.
// - The jitter only while something resolves it (IsJitterActive), with as many phases as DLSS, FSR and the render
//   scale want.
// - The draws of each marked entity recorded (DrawnWorldMatrix: box shadows, car mirrors).
// - Particles, glass and other transparent geometry have no motion vectors of their own. The luminance of the scene
//   is copied right after the fog pass, which comes after water and before transparent geometry and visual effects;
//   where the final scene differs from it, FSR follows the current frame more (reactive mask).
// - The rain has no motion vectors either, and the streaks of a whole screen of it smeared or vanished. It's drawn
//   into a copy of the scene instead, and what it changed there is added to the picture after the resolve, DLAA or
//   FSR (see RenderRain).
// - What reads the depth after the resolve, or samples it at a few points, sees the jitter in it: on edges it flips
//   between the near and the far surface. Depth of field, sun shafts and the coronas' occlusion read a depth averaged
//   over the jitter instead (see StabilizeDepth).

namespace TemporalMath
{
    struct Matrix
    {
        double m[4][4]{};

        static Matrix Identity()
        {
            Matrix r;
            for (int i = 0; i < 4; ++i)
                r.m[i][i] = 1.0;
            return r;
        }

        static Matrix From(const float(&f)[4][4])
        {
            Matrix r;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r.m[i][j] = f[i][j];
            return r;
        }

        void To(float* f) const
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    f[i * 4 + j] = static_cast<float>(m[i][j]);
        }

        Matrix operator*(const Matrix& b) const
        {
            Matrix r;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
            return r;
        }

        Matrix Inverse() const
        {
            // Gauss-Jordan with partial pivoting
            Matrix a = *this;
            Matrix r = Identity();
            for (int c = 0; c < 4; ++c)
            {
                int pivot = c;
                for (int i = c + 1; i < 4; ++i)
                    if (std::abs(a.m[i][c]) > std::abs(a.m[pivot][c]))
                        pivot = i;
                if (std::abs(a.m[pivot][c]) < 1e-12)
                    return Identity();
                std::swap(a.m[c], a.m[pivot]);
                std::swap(r.m[c], r.m[pivot]);
                double inv = 1.0 / a.m[c][c];
                for (int j = 0; j < 4; ++j)
                {
                    a.m[c][j] *= inv;
                    r.m[c][j] *= inv;
                }
                for (int i = 0; i < 4; ++i)
                {
                    if (i == c)
                        continue;
                    double f = a.m[i][c];
                    for (int j = 0; j < 4; ++j)
                    {
                        a.m[i][j] -= f * a.m[c][j];
                        r.m[i][j] -= f * r.m[c][j];
                    }
                }
            }
            return r;
        }
    };
}

export namespace TemporalAA
{
    // Called at the end of the camera scene's G-buffer pass, whatever the antialiasing, with the device state saved
    // around it: the depth there reads as the G-buffer pass wrote it, which it does not in the lighting phase.
    // The same, before the motion vectors are made from the depth: what is drawn into the G-buffer there (GPU grass)
    // gets the camera's motion, and its depth reaches the resolve, DLAA and FSR. OnGBufferEnd follows, after them.
    inline void (*OnGBufferBeforeMotion)(IDirect3DDevice9* device) = nullptr;
    // Game thread, as the camera scene's G-buffer list is closed: commands appended here run in the pass, after its
    // entities and before the end above (Far Prop Instancing draws its batches there, so they reach the depth and motion)
    inline void (*AppendBeforeSceneEnd)() = nullptr;
    inline void (*OnGBufferEnd)(IDirect3DDevice9* device) = nullptr;
    // Marks the motion vectors' passes for the PostFx profiler (begin, then end), when it is set.
    inline void (*ProfileMotion)(IDirect3DDevice9* device, bool begin) = nullptr;

    struct FrameCamera
    {
        TemporalMath::Matrix View;
        TemporalMath::Matrix Projection;        // as rendered, jittered
        TemporalMath::Matrix ProjectionNoJitter;
        TemporalMath::Matrix ViewProjectionNoJitter;
        float JitterNdc[2]{};                   // P[2][0] and P[2][1] minus their unjittered values
        float JitterPixels[2]{};                // how far the rendered content moved, in pixels (y down)
        float Near = 0.0f;
        float Far = 0.0f;
        float FovY = 0.0f;                      // radians
        int32_t Width = 0;
        int32_t Height = 0;
        uint32_t Frame = 0;
        bool Valid = false;
    };

    // Filled on the render thread when the G-buffer pass starts
    FrameCamera CurrentCamera;
    FrameCamera PreviousCamera;

    // Render thread frame counter, advanced once per scene
    uint32_t SceneFrame = 0;

    bool IsMotionVectorsReady();
    IDirect3DTexture9* GetMotionVectors();
    // [ANTIALIASING] ObjectDepthBias: how much nearer than the G-buffer's depth a pass drawing motion vectors over it
    // writes its own, so the depth test keeps it where its surface is what is seen
    float ObjectDepthBias();
    bool HistoryValid();
    void OnFogDrawn();
}

class TemporalLayer
{
public:
    // Settings from the ini
    static inline int32_t nJitterPhases = 8;
    static inline float fObjectDepthBias = 0.00002f;
    static inline float fCameraCutDistance = 25.0f;
    static inline int32_t nDLSSPreset = 0;
    static inline float fFSRSharpness = 0.0f;
    static inline bool bReactiveMask = true;
    static inline float fReactiveScale = 2.0f;
    static inline float fReactiveMax = 0.75f;
    static inline bool bRainLayer = true;
    static inline bool bSteadyDepth = true;
    // temporal.ixx's temporal AA, which reads [TEMPORAL]: ours are in [ANTIALIASING] (ApplyCoreSettings)
    static inline float fCurrentWeightMin = 0.08f;
    static inline float fCurrentWeightMax = 0.2f;
    static inline float fCurrentWeightPerPixel = 0.1f;
    static inline float fVarianceClipGamma = 1.25f;
    static inline bool bObjectMotionVectors = true;
    static inline bool bSkinnedMotionVectors = true;

    static inline HMODULE hm = NULL;

    // ---------------------------------------------------------------------------------------------
    // Resources, owned by PostFX device callbacks (through temporal.ixx's)

    static inline rage::grcRenderTargetPC* MotionRT = nullptr;      // previous - current position, texture coordinates
    static inline rage::grcRenderTargetPC* OpaqueRT = nullptr;      // scene luminance before transparent geometry
    static inline rage::grcRenderTargetPC* ReactiveRT = nullptr;    // reactive mask for FSR
    static inline rage::grcRenderTargetPC* SteadyDepthRT[2] = {};   // logarithmic depth averaged over the jitter
    static inline uint32_t OpaqueFrame = 0;                          // SceneFrame OpaqueRT was written in
    static inline int32_t HistoryWidth = 0;                          // the render size
    static inline int32_t HistoryHeight = 0;

    static inline IDirect3DPixelShader9* OpaqueLumaPS = nullptr;
    static inline IDirect3DPixelShader9* ReactivePS = nullptr;
    static inline IDirect3DPixelShader9* RainLayerPS = nullptr;
    static inline IDirect3DPixelShader9* RainCompositePS = nullptr;
    static inline IDirect3DPixelShader9* SteadyDepthPS = nullptr;

    static void LoadShaders(IDirect3DDevice9* pDevice)
    {
        auto loadCompiledShader = [&](int resourceID, IDirect3DPixelShader9*& shader) -> bool
        {
            if (shader)
                return true;
            HRSRC hRes = FindResourceW(hm, MAKEINTRESOURCEW(resourceID), RT_RCDATA);
            if (!hRes) return false;
            HGLOBAL hGlob = LoadResource(hm, hRes);
            if (!hGlob) return false;
            void* buffer = LockResource(hGlob);
            if (!buffer) return false;
            return pDevice->CreatePixelShader((DWORD*)buffer, &shader) == S_OK && shader;
        };

        loadCompiledShader(IDR_TEMPORAL_PS_OPAQUE_LUMA, OpaqueLumaPS);
        loadCompiledShader(IDR_TEMPORAL_PS_REACTIVE, ReactivePS);
        loadCompiledShader(IDR_TEMPORAL_PS_RAIN_LAYER, RainLayerPS);
        loadCompiledShader(IDR_TEMPORAL_PS_RAIN_COMPOSITE, RainCompositePS);
        loadCompiledShader(IDR_TEMPORAL_PS_STEADY_DEPTH, SteadyDepthPS);
    }

    static void CreateResources(uint32_t width, uint32_t height)
    {
        auto desc = rage::OwnRenderTargetDesc(rage::GRCFMT_G16R16F);
        MotionRT = rage::CreateEmptyRenderTarget("TemporalMotion", width, height, 32, desc);

        if (bSteadyDepth)
        {
            desc.mFormat = rage::GRCFMT_R32F;
            SteadyDepthRT[0] = rage::CreateEmptyRenderTarget("TemporalSteadyDepth0", width, height, 32, desc);
            SteadyDepthRT[1] = rage::CreateEmptyRenderTarget("TemporalSteadyDepth1", width, height, 32, desc);
        }

        if (bReactiveMask)
        {
            desc.mFormat = rage::GRCFMT_R16F;
            OpaqueRT = rage::CreateEmptyRenderTarget("TemporalOpaque", width, height, 16, desc);
            ReactiveRT = rage::CreateEmptyRenderTarget("TemporalReactive", width, height, 16, desc);
        }

        HistoryWidth = width;
        HistoryHeight = height;
    }

    static void ReleaseResources()
    {
        auto destroy = [](rage::grcRenderTargetPC*& rt)
        {
            if (rt)
            {
                rt->Destroy();
                rt = nullptr;
            }
        };
        destroy(MotionRT);
        destroy(OpaqueRT);
        destroy(ReactiveRT);
        destroy(SteadyDepthRT[0]);
        destroy(SteadyDepthRT[1]);
        SteadyFrame = 0;
        ReleaseSceneTargets();
        UpscalerFrame = 0;
        OpaqueFrame = 0;
        MotionFrame = 0;
        // Declarations by address: one made after the reset at the address of a released one is another. Emptied on
        // the render thread, which reads them; the reset runs on the main thread
        bDeclCacheStale = true;
    }

    // The motion vectors can be made: temporal.ixx's depth of this scene and its BridgeMotion
    static bool ResourcesReady()
    {
        return MotionRT && MotionRT->mD3DTexture && Temporal::DepthRT && Temporal::BridgeMotionPS && Temporal::StencilRT &&
            Temporal::StencilRT->mD3DTexture;
    }

    // ---------------------------------------------------------------------------------------------
    // Jitter, main thread: temporal.ixx's, while something resolves it

    static inline std::atomic<uint32_t> ResolveFrame = 0;   // SceneFrame of the last resolve
    static inline std::atomic<uint32_t> PresentFrame = 1;   // every frame, a scene drawn or not; 0 is "never"
    static inline std::atomic<uint32_t> ResolvePresent = 0; // PresentFrame of the last resolve

    static bool IsJitterActive()
    {
        if (TemporalAA::GetMode() == TemporalAA::Mode::Off)
            return false;

        // Never jitter an image that is not resolved: e.g. post processing is off or failed to start. Counted in
        // frames, not scenes: frames without a scene would keep the last resolve recent forever.
        auto resolved = ResolvePresent.load();
        if (resolved == 0)
            return false;
        auto since = PresentFrame.load() - resolved;

        // Menus: only while the scene behind them is resolved every frame, as in the graphics settings, where without
        // the jitter DLSS and FSR had nothing to add and the scene stayed at the render size. A menu without the scene
        // stops the frame after the last resolve. Never on the map screen (3): the game still resolves a scene there,
        // and the map, drawn with the camera's projection, shook with FSR.
        if (CMenuManager::m_MenuActive && *CMenuManager::m_MenuActive)
            return since < 2 && !(pMenuTab && *pMenuTab == 3);
        return since < 10;
    }

    // temporal.ixx's phases, read as the camera is set up. NVIDIA recommends at least 16 for DLSS, 32 or more
    // preferred, and for a lower render size 8 per output pixel area of a rendered pixel.
    static void UpdateJitterPhases()
    {
        auto phases = static_cast<uint32_t>(std::clamp(nJitterPhases, 2, 128));
        auto mode = TemporalAA::GetMode();
        if (mode == TemporalAA::Mode::DLAA)
            phases = std::max(phases, 32u);
        else if (mode == TemporalAA::Mode::FSR)
            phases = std::max(phases, 16u);
        if (RenderScale::IsActive())
        {
            auto scale = RenderScale::GetScale();
            phases = std::max(phases, static_cast<uint32_t>(std::ceil(8.0f / (scale * scale))));
        }
        Temporal::JitterPhases = phases;
    }

    // Our [ANTIALIASING] settings into temporal.ixx's, every frame: its init reads [TEMPORAL], before or after ours
    static void ApplyCoreSettings()
    {
        Temporal::WeightMin = fCurrentWeightMin;
        Temporal::WeightMax = fCurrentWeightMax;
        Temporal::MotionWeight = fCurrentWeightPerPixel;
        Temporal::ClipGamma = fVarianceClipGamma;
        Temporal::CameraCutDistance = fCameraCutDistance;
        Temporal::FSRSharpness = fFSRSharpness;
        Temporal::TrackObjects = bObjectMotionVectors;
        Temporal::TrackBones = bSkinnedMotionVectors;
    }

    // ---------------------------------------------------------------------------------------------
    // Render thread: scene camera

    static void OnSceneBegin()
    {
        using namespace TemporalAA;

        RenderScale::BeginScene();

        auto viewport = rage::GetCurrentViewport();
        if (!viewport)
            return;

        ++SceneFrame;

        if (CurrentCamera.Valid)
            PreviousCamera = CurrentCamera;

        FrameCamera camera;
        camera.View = TemporalMath::Matrix::From(viewport->mViewMatrix);
        camera.Projection = TemporalMath::Matrix::From(viewport->mProjectionMatrix);
        camera.ProjectionNoJitter = camera.Projection;
        // The size the scene renders at
        camera.Width = static_cast<int32_t>(RenderScale::ToRenderWidth(static_cast<uint32_t>(viewport->mWidth)));
        camera.Height = static_cast<int32_t>(RenderScale::ToRenderHeight(static_cast<uint32_t>(viewport->mHeight)));
        camera.Near = viewport->mNearClip;
        camera.Far = viewport->mFarClip;
        camera.FovY = viewport->mFov * 0.017453292f;
        camera.Frame = SceneFrame;

        if (viewport->mIsPerspective)
        {
            camera.JitterNdc[0] = viewport->mProjectionMatrix[2][0] - viewport->field_2D8;
            camera.JitterNdc[1] = viewport->mProjectionMatrix[2][1] - viewport->field_2DC;
            camera.ProjectionNoJitter.m[2][0] = viewport->field_2D8;
            camera.ProjectionNoJitter.m[2][1] = viewport->field_2DC;
        }

        camera.JitterPixels[0] = -camera.JitterNdc[0] * 0.5f * static_cast<float>(camera.Width);
        camera.JitterPixels[1] = camera.JitterNdc[1] * 0.5f * static_cast<float>(camera.Height);
        camera.ViewProjectionNoJitter = camera.View * camera.ProjectionNoJitter;
        camera.Valid = viewport->mIsPerspective && camera.Width > 0 && camera.Height > 0 && camera.Near > 0.0f && camera.Far > camera.Near;
        CurrentCamera = camera;

        Upscaler::Update();
        if (bDeclCacheStale.exchange(false))
            DeclCache.clear();
    }

    // ---------------------------------------------------------------------------------------------
    // Render thread: the draws of the entities temporal.ixx marks, for DrawnWorldMatrix

    struct DeclInfo
    {
        uint32_t Streams = 0;
        bool Skinned = false;
    };

    struct DrawRecord
    {
        IDirect3DIndexBuffer9* IndexBuffer = nullptr;
        IDirect3DVertexBuffer9* VertexBuffer = nullptr;
        UINT StartIndex = 0;
        UINT PrimitiveCount = 0;
        UINT NumVertices = 0;
        float World[16]{};
        int32_t Bones = -1;            // offset in the bone pool of the same frame
    };

    struct EntityRecord
    {
        std::vector<DrawRecord> Draws;
    };

    static inline std::unordered_map<IDirect3DVertexDeclaration9*, DeclInfo> DeclCache;
    static inline std::atomic<bool> bDeclCacheStale = false;
    static inline std::unordered_map<void*, EntityRecord> EntitiesCurrent;
    static inline std::unordered_map<void*, EntityRecord> EntitiesPrevious;
    static inline std::vector<float> BonesCurrent;
    static inline std::vector<float> BonesPrevious;
    static inline thread_local bool bInternalDraw = false;

    static const DeclInfo& GetDeclInfo(IDirect3DVertexDeclaration9* decl)
    {
        auto it = DeclCache.find(decl);
        if (it != DeclCache.end())
            return it->second;

        DeclInfo info;
        D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH]{};
        UINT count = 0;
        if (decl && decl->GetDeclaration(elements, &count) == S_OK)
        {
            for (UINT i = 0; i < count && elements[i].Stream != 0xFF; ++i)
            {
                info.Streams = std::max<uint32_t>(info.Streams, elements[i].Stream + 1u);
                if (elements[i].Usage == D3DDECLUSAGE_BLENDINDICES)
                    info.Skinned = true;
            }
        }
        return DeclCache.emplace(decl, info).first->second;
    }

    // Where a draw stands, to tell draws of one mesh apart: skinned, its first bone's offset from the entity (a car's
    // draws hold only the car's position, each wheel's own place is in its bone), which the car's speed does not move;
    // otherwise its world matrix's position (rows or columns, whichever holds it)
    static void DrawPosition(const float* world, const float* bones, float out[3])
    {
        const bool rows = std::abs(world[12]) + std::abs(world[13]) + std::abs(world[14]) >= std::abs(world[3]) + std::abs(world[7]) + std::abs(world[11]);
        for (int k = 0; k < 3; ++k)
            out[k] = bones ? bones[k * 4 + 3] : rows ? world[12 + k] : world[k * 4 + 3];
    }

    static const float* PreviousBones(const DrawRecord& d)
    {
        return d.Bones >= 0 && static_cast<size_t>(d.Bones + BoneTexels * 4) <= BonesPrevious.size() ? &BonesPrevious[d.Bones] : nullptr;
    }

    // temporal.ixx's draw hook, for a draw of the entity it marks last
    static void RecordDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT, UINT, UINT numVertices, UINT startIndex, UINT primitiveCount)
    {
        if (bInternalDraw || (type != D3DPT_TRIANGLELIST && type != D3DPT_TRIANGLESTRIP) || Temporal::EntityStack.empty())
            return;
        auto entity = Temporal::EntityStack.back();

        IDirect3DVertexDeclaration9* decl = nullptr;
        if (device->GetVertexDeclaration(&decl) != S_OK || !decl)
            return;
        IDirect3DIndexBuffer9* indices = nullptr;
        device->GetIndices(&indices);
        IDirect3DVertexBuffer9* stream0 = nullptr;
        UINT offset0 = 0, stride0 = 0;
        device->GetStreamSource(0, &stream0, &offset0, &stride0);
        const bool skinned = GetDeclInfo(decl).Skinned;
        decl->Release();
        // The buffers only tell the draws apart: not kept
        auto vertices = stream0;
        SAFE_RELEASE(stream0);
        if (!indices)
            return;
        indices->Release();

        DrawRecord draw;
        draw.IndexBuffer = indices;
        draw.VertexBuffer = vertices;
        draw.StartIndex = startIndex;
        draw.PrimitiveCount = primitiveCount;
        draw.NumVertices = numVertices;
        device->GetVertexShaderConstantF(0, draw.World, 4);
        if (skinned)
        {
            draw.Bones = static_cast<int32_t>(BonesCurrent.size());
            BonesCurrent.resize(BonesCurrent.size() + BoneTexels * 4);
            device->GetVertexShaderConstantF(64, &BonesCurrent[draw.Bones], BoneTexels);
        }
        EntitiesCurrent[entity].Draws.push_back(draw);
    }

    static constexpr int32_t BoneTexels = 144;        // 48 bones, 3 rows each

    // ---------------------------------------------------------------------------------------------
    // Render thread: device state of our passes

    struct StateBackup
    {
        IDirect3DDevice9* device = nullptr;
        IDirect3DSurface9* rt[4]{};
        IDirect3DSurface9* ds = nullptr;
        D3DVIEWPORT9 viewport{};
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        DWORD fvf = 0;
        IDirect3DVertexBuffer9* streams[MaxStreams]{};
        UINT offsets[MaxStreams]{};
        UINT strides[MaxStreams]{};
        IDirect3DIndexBuffer9* indices = nullptr;
        IDirect3DBaseTexture9* texture0 = nullptr;
        IDirect3DBaseTexture9* vertexTexture0 = nullptr;
        float vsConstants[21 * 4]{};
        float vsBones[BoneTexels * 4]{};
        float psConstants[7 * 4]{};

        static constexpr D3DRENDERSTATETYPE RenderStates[] =
        {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
            D3DRS_STENCILENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1,
            D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FILLMODE, D3DRS_CLIPPLANEENABLE,
            D3DRS_POINTSIZE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSPRITEENABLE, D3DRS_DEPTHBIAS,
            D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_FOGENABLE, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_SEPARATEALPHABLENDENABLE,
            D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
        };
        DWORD renderStates[std::size(RenderStates)]{};

        static constexpr D3DSAMPLERSTATETYPE SamplerStates[] =
        {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE,
        };
        DWORD samplerStates0[std::size(SamplerStates)]{};
        DWORD vertexSamplerStates0[std::size(SamplerStates)]{};

        explicit StateBackup(IDirect3DDevice9* dev) : device(dev)
        {
            for (DWORD i = 0; i < 4; ++i)
                device->GetRenderTarget(i, &rt[i]);
            device->GetDepthStencilSurface(&ds);
            device->GetViewport(&viewport);
            device->GetVertexShader(&vs);
            device->GetPixelShader(&ps);
            device->GetVertexDeclaration(&decl);
            device->GetFVF(&fvf);
            for (DWORD i = 0; i < MaxStreams; ++i)
                device->GetStreamSource(i, &streams[i], &offsets[i], &strides[i]);
            device->GetIndices(&indices);
            device->GetTexture(0, &texture0);
            device->GetTexture(D3DVERTEXTEXTURESAMPLER0, &vertexTexture0);
            device->GetVertexShaderConstantF(0, vsConstants, 21);
            device->GetVertexShaderConstantF(64, vsBones, BoneTexels);
            device->GetPixelShaderConstantF(0, psConstants, 7);
            for (size_t i = 0; i < std::size(RenderStates); ++i)
                device->GetRenderState(RenderStates[i], &renderStates[i]);
            for (size_t i = 0; i < std::size(SamplerStates); ++i)
            {
                device->GetSamplerState(0, SamplerStates[i], &samplerStates0[i]);
                device->GetSamplerState(D3DVERTEXTEXTURESAMPLER0, SamplerStates[i], &vertexSamplerStates0[i]);
            }
        }

        ~StateBackup()
        {
            for (DWORD i = 0; i < 4; ++i)
                device->SetRenderTarget(i, rt[i]);
            device->SetDepthStencilSurface(ds);
            device->SetViewport(&viewport);
            device->SetVertexShader(vs);
            device->SetPixelShader(ps);
            if (decl)
                device->SetVertexDeclaration(decl);
            else
                device->SetFVF(fvf);
            for (DWORD i = 0; i < MaxStreams; ++i)
                device->SetStreamSource(i, streams[i], offsets[i], strides[i]);
            device->SetIndices(indices);
            device->SetTexture(0, texture0);
            device->SetTexture(D3DVERTEXTEXTURESAMPLER0, vertexTexture0);
            device->SetVertexShaderConstantF(0, vsConstants, 21);
            device->SetVertexShaderConstantF(64, vsBones, BoneTexels);
            device->SetPixelShaderConstantF(0, psConstants, 7);
            for (size_t i = 0; i < std::size(RenderStates); ++i)
                device->SetRenderState(RenderStates[i], renderStates[i]);
            for (size_t i = 0; i < std::size(SamplerStates); ++i)
            {
                device->SetSamplerState(0, SamplerStates[i], samplerStates0[i]);
                device->SetSamplerState(D3DVERTEXTEXTURESAMPLER0, SamplerStates[i], vertexSamplerStates0[i]);
            }

            for (auto& s : rt)
                SAFE_RELEASE(s);
            SAFE_RELEASE(ds);
            SAFE_RELEASE(vs);
            SAFE_RELEASE(ps);
            SAFE_RELEASE(decl);
            for (auto& s : streams)
                SAFE_RELEASE(s);
            SAFE_RELEASE(indices);
            SAFE_RELEASE(texture0);
            SAFE_RELEASE(vertexTexture0);
        }
    };

    // A sampler's texture and states, put back when it goes out of scope (StateBackup keeps sampler 0 only)
    struct SamplerBackup
    {
        IDirect3DDevice9* device = nullptr;
        DWORD sampler = 0;
        IDirect3DBaseTexture9* texture = nullptr;
        DWORD states[std::size(StateBackup::SamplerStates)]{};

        SamplerBackup(IDirect3DDevice9* dev, DWORD s) : device(dev), sampler(s)
        {
            device->GetTexture(sampler, &texture);
            for (size_t i = 0; i < std::size(states); ++i)
                device->GetSamplerState(sampler, StateBackup::SamplerStates[i], &states[i]);
        }

        ~SamplerBackup()
        {
            device->SetTexture(sampler, texture);
            for (size_t i = 0; i < std::size(states); ++i)
                device->SetSamplerState(sampler, StateBackup::SamplerStates[i], states[i]);
            SAFE_RELEASE(texture);
        }
    };

    static void BindSampler(IDirect3DDevice9* device, DWORD sampler, IDirect3DBaseTexture9* texture, D3DTEXTUREFILTERTYPE filter)
    {
        device->SetTexture(sampler, texture);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, filter);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, filter);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    static void DrawFullscreen(IDirect3DDevice9* device, float width, float height)
    {
        device->SetVertexShader(nullptr);
        device->SetFVF(kScreenVertexFVF);
        ScreenQuad::Full(width, height).Draw(device);
    }

    // Render states of a plain fullscreen pass, StateBackup restores them
    static void SetFullscreenStates(IDirect3DDevice9* device, DWORD colorWrite = 0x0F)
    {
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    }

    // temporal.ixx's scene end, before its depth copy: what is drawn into the G-buffer here (GPU grass) reaches it
    static void BeforeSceneEnd()
    {
        auto device = RealDevice();
        if (device && TemporalAA::OnGBufferBeforeMotion)
        {
            bInternalDraw = true;
            {
                StateBackup backup(device);
                TemporalAA::OnGBufferBeforeMotion(device);
            }
            bInternalDraw = false;
        }
    }

    // And after it, whatever the antialiasing: the motion vectors, then the other effects
    static void AfterSceneEnd()
    {
        using namespace TemporalAA;

        auto device = RealDevice();
        if (device && GetMode() != Mode::Off && CurrentCamera.Valid && ResourcesReady() && Temporal::DepthFrame == Temporal::Frame)
        {
            bInternalDraw = true;
            {
                StateBackup backup(device);
                if (ProfileMotion)
                    ProfileMotion(device, true);
                RenderMotion(device);
                if (ProfileMotion)
                    ProfileMotion(device, false);
            }
            bInternalDraw = false;
            MotionFrame = SceneFrame;
        }

        if (device && OnGBufferEnd)
        {
            bInternalDraw = true;
            {
                StateBackup backup(device);
                OnGBufferEnd(device);
            }
            bInternalDraw = false;
        }

        // Keep the draws of this frame for the next one. The records of the frame before are reused, their memory
        // kept; those of entities not drawn this frame go.
        std::swap(EntitiesPrevious, EntitiesCurrent);
        for (auto it = EntitiesCurrent.begin(); it != EntitiesCurrent.end();)
        {
            auto drawn = EntitiesPrevious.find(it->first);
            if (drawn == EntitiesPrevious.end() || drawn->second.Draws.empty())
                it = EntitiesCurrent.erase(it);
            else
            {
                it->second.Draws.clear();
                ++it;
            }
        }
        std::swap(BonesPrevious, BonesCurrent);
        BonesCurrent.clear();
    }

    static inline uint32_t MotionFrame = 0;

    // The G-buffer's motion (and the camera's, where the sky has none) as a texture, by temporal.ixx's BridgeMotion:
    // its constants as its Resolve sets them
    static void RenderMotion(IDirect3DDevice9* device)
    {
        IDirect3DSurface9* motionSurface = nullptr;
        MotionRT->mD3DTexture->GetSurfaceLevel(0, &motionSurface);
        if (!motionSurface)
            return;

        auto width = static_cast<float>(MotionRT->mWidth);
        auto height = static_cast<float>(MotionRT->mHeight);
        SamplerBackup motionSampler(device, 1), depthSampler(device, 2);
        device->SetRenderTarget(0, motionSurface);
        for (DWORD i = 1; i < 4; ++i)
            device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(nullptr);
        SetFullscreenStates(device, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN);
        BindSampler(device, 1, Temporal::StencilRT->mD3DTexture, D3DTEXF_POINT);
        BindSampler(device, 2, Temporal::DepthRT->mD3DTexture, D3DTEXF_POINT);

        float pixel[4] = { 1.0f / width, 1.0f / height, 0.0f, 0.0f };
        device->SetPixelShaderConstantF(0, pixel, 1);
        float previousClip[16];
        Temporal::CameraReprojection.To(previousClip);
        device->SetPixelShaderConstantF(7, previousClip, 4);
        float jitter[4] = { Temporal::Current.Jitter.X / width, Temporal::Current.Jitter.Y / height, 0.0f, 0.0f };
        device->SetPixelShaderConstantF(11, jitter, 1);

        device->SetPixelShader(Temporal::BridgeMotionPS);
        DrawFullscreen(device, width, height);
        motionSurface->Release();
    }


    // ---------------------------------------------------------------------------------------------
    // Render thread, right after the fog pass of the scene

    static void CaptureOpaque()
    {
        using namespace TemporalAA;

        // Once per frame, for the scene the motion vectors were rendered for; read by temporal AA and FSR's reactive
        // mask, not by DLSS, which goes without one
        if (!bReactiveMask || GetMode() == Mode::Off || GetMode() == Mode::DLAA || OpaqueFrame == SceneFrame || MotionFrame != SceneFrame)
            return;
        if (!OpaqueRT || !OpaqueRT->mD3DTexture || !OpaqueLumaPS)
            return;

        auto device = RageDirect3DDevice9::GetRuntimeDevice();
        if (!device)
            return;

        // The fog was drawn into the scene target
        IDirect3DSurface9* sceneSurface = nullptr;
        IDirect3DTexture9* sceneTexture = nullptr;
        IDirect3DSurface9* opaqueSurface = nullptr;
        D3DSURFACE_DESC sceneDesc{};
        device->GetRenderTarget(0, &sceneSurface);
        if (sceneSurface && SUCCEEDED(sceneSurface->GetDesc(&sceneDesc)) &&
            sceneDesc.Width == static_cast<UINT>(OpaqueRT->mWidth) && sceneDesc.Height == static_cast<UINT>(OpaqueRT->mHeight))
        {
            sceneSurface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&sceneTexture));
        }
        OpaqueRT->mD3DTexture->GetSurfaceLevel(0, &opaqueSurface);

        if (sceneTexture && opaqueSurface)
        {
            bInternalDraw = true;
            {
                StateBackup backup(device);
                device->SetRenderTarget(0, opaqueSurface);
                for (DWORD i = 1; i < 4; ++i)
                    device->SetRenderTarget(i, nullptr);
                device->SetDepthStencilSurface(nullptr);
                SetFullscreenStates(device);

                device->SetTexture(0, sceneTexture);
                device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

                device->SetPixelShader(OpaqueLumaPS);
                DrawFullscreen(device, static_cast<float>(OpaqueRT->mWidth), static_cast<float>(OpaqueRT->mHeight));
                device->SetTexture(0, nullptr);
            }
            bInternalDraw = false;
            OpaqueFrame = SceneFrame;
        }

        SAFE_RELEASE(opaqueSurface);
        SAFE_RELEASE(sceneTexture);
        SAFE_RELEASE(sceneSurface);
    }

    static bool IsOpaqueReady()
    {
        return bReactiveMask && OpaqueRT && OpaqueRT->mD3DTexture && OpaqueFrame == TemporalAA::SceneFrame;
    }

    // Reactive mask for FSR, on the device of post processing
    static IDirect3DTexture9* RenderReactive(IDirect3DDevice9* device, IDirect3DTexture9* scene)
    {
        if (!IsOpaqueReady() || !ReactiveRT || !ReactiveRT->mD3DTexture || !ReactivePS)
            return nullptr;

        IDirect3DSurface9* reactiveSurface = nullptr;
        ReactiveRT->mD3DTexture->GetSurfaceLevel(0, &reactiveSurface);
        if (!reactiveSurface)
            return nullptr;

        bInternalDraw = true;
        {
            StateBackup backup(device);

            // StateBackup keeps sampler 0, the mask also uses 1
            IDirect3DBaseTexture9* oldTextures[2]{};
            DWORD samplerStates[2][6]{};
            constexpr D3DSAMPLERSTATETYPE states[6] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
            for (DWORD s = 0; s < 2; ++s)
            {
                device->GetTexture(s, &oldTextures[s]);
                for (DWORD i = 0; i < 6; ++i)
                    device->GetSamplerState(s, states[i], &samplerStates[s][i]);
            }

            IDirect3DBaseTexture9* textures[2] = { scene, OpaqueRT->mD3DTexture };
            for (DWORD s = 0; s < 2; ++s)
            {
                device->SetTexture(s, textures[s]);
                device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
            }

            float constants[4] = { fReactiveScale, fReactiveMax, 0.0f, 0.0f };
            device->SetRenderTarget(0, reactiveSurface);
            for (DWORD i = 1; i < 4; ++i)
                device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            SetFullscreenStates(device);
            device->SetPixelShader(ReactivePS);
            device->SetPixelShaderConstantF(0, constants, 1);
            DrawFullscreen(device, static_cast<float>(ReactiveRT->mWidth), static_cast<float>(ReactiveRT->mHeight));

            for (DWORD s = 0; s < 2; ++s)
            {
                device->SetTexture(s, oldTextures[s]);
                for (DWORD i = 0; i < 6; ++i)
                    device->SetSamplerState(s, states[i], samplerStates[s][i]);
                SAFE_RELEASE(oldTextures[s]);
            }
        }
        bInternalDraw = false;
        reactiveSurface->Release();
        return ReactiveRT->mD3DTexture;
    }

    // ---------------------------------------------------------------------------------------------
    // Targets of the scene's size, render thread: made on first use, given back at a device reset

    static inline IDirect3DTexture9* SceneCopyTexture = nullptr;  // the scene's format: the rain draws into it, the stipple filter writes it
    static inline IDirect3DSurface9* SceneCopySurface = nullptr;
    static inline IDirect3DTexture9* RainLayerTexture = nullptr;  // what the rain changed
    static inline IDirect3DSurface9* RainLayerSurface = nullptr;
    static inline bool bSceneTargetsFailed = false;               // not tried again until the next reset

    static IDirect3DDevice9* RealDevice()
    {
        return RageDirect3DDevice9::GetRuntimeDevice();
    }

    static void ReleaseSceneTargets()
    {
        SAFE_RELEASE(SceneCopySurface);
        SAFE_RELEASE(SceneCopyTexture);
        SAFE_RELEASE(RainLayerSurface);
        SAFE_RELEASE(RainLayerTexture);
        bSceneTargetsFailed = false;
        RainFrame = 0;
    }

    static IDirect3DSurface9* GetSceneTarget(IDirect3DDevice9* device, IDirect3DTexture9*& texture, IDirect3DSurface9*& surface, UINT width, UINT height, D3DFORMAT format)
    {
        if (texture)
        {
            D3DSURFACE_DESC desc{};
            texture->GetLevelDesc(0, &desc);
            if (desc.Width == width && desc.Height == height && desc.Format == format)
                return surface;
            SAFE_RELEASE(surface);
            SAFE_RELEASE(texture);
        }
        // Not made while the device is being reset: it would be alive at the Reset
        if (bSceneTargetsFailed || rage::grcDevice::IsDeviceLost())
            return nullptr;
        if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture, nullptr)) || !texture ||
            FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface)
        {
            SAFE_RELEASE(texture);
            bSceneTargetsFailed = true;
            return nullptr;
        }
        return surface;
    }

    static IDirect3DSurface9* GetSceneCopy(IDirect3DDevice9* device, const D3DSURFACE_DESC& scene)
    {
        return GetSceneTarget(device, SceneCopyTexture, SceneCopySurface, scene.Width, scene.Height, scene.Format);
    }

    // ---------------------------------------------------------------------------------------------
    // Rain after the resolve, render thread
    //
    // CRain::Render draws the rain, the last of the visual effects. While the resolve runs, the scene is copied and the
    // rain drawn into the copy; the difference to the scene becomes the layer, at the render size, and the scene goes
    // on without the rain. The resolve adds the layer to its result, moved back by the jitter the rain was drawn with,
    // or to the scene when it did not resolve.

    static inline SafetyHookInline shRenderRain{};
    static inline IDirect3DSurface9* RainScene = nullptr;   // the scene's target while the rain draws into the copy
    static inline D3DVIEWPORT9 RainViewport{};
    static inline uint32_t RainFrame = 0;                    // SceneFrame of the layer, 0 once it was added

    static void __fastcall RenderRain(float* rain, void* edx)
    {
        // The amount of rain comes first, nothing is drawn without it
        bool layer = rain && *rain > 0.0f && BeginRain();
        shRenderRain.unsafe_thiscall(rain);
        if (layer)
            EndRain();
    }

    static bool BeginRain()
    {
        using namespace TemporalAA;

        // Only where the resolve adds it back, once a scene
        if (!bRainLayer || !RainLayerPS || !RainCompositePS || !IsJitterActive() || MotionFrame != SceneFrame || RainFrame == SceneFrame)
            return false;
        auto device = RealDevice();
        if (!device)
            return false;

        // The camera's scene: the other phases draw into smaller targets
        IDirect3DSurface9* scene = nullptr;
        D3DSURFACE_DESC desc{};
        device->GetRenderTarget(0, &scene);
        if (!scene || FAILED(scene->GetDesc(&desc)) || desc.Width != static_cast<UINT>(HistoryWidth) || desc.Height != static_cast<UINT>(HistoryHeight))
        {
            SAFE_RELEASE(scene);
            return false;
        }

        auto copy = GetSceneCopy(device, desc);
        auto layer = GetSceneTarget(device, RainLayerTexture, RainLayerSurface, desc.Width, desc.Height, D3DFMT_A16B16G16R16F);
        if (!copy || !layer || FAILED(device->StretchRect(scene, nullptr, copy, nullptr, D3DTEXF_POINT)))
        {
            scene->Release();
            return false;
        }

        device->GetViewport(&RainViewport);
        device->SetRenderTarget(0, copy);
        device->SetViewport(&RainViewport);
        RainScene = scene;
        return true;
    }

    static void EndRain()
    {
        using namespace TemporalAA;

        auto device = RealDevice();
        auto scene = RainScene;
        RainScene = nullptr;
        device->SetRenderTarget(0, scene);
        device->SetViewport(&RainViewport);

        IDirect3DTexture9* sceneTexture = nullptr;
        scene->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&sceneTexture));
        if (sceneTexture)
        {
            bInternalDraw = true;
            {
                StateBackup backup(device);
                SamplerBackup sampler1(device, 1);
                device->SetRenderTarget(0, RainLayerSurface);
                for (DWORD i = 1; i < 4; ++i)
                    device->SetRenderTarget(i, nullptr);
                device->SetDepthStencilSurface(nullptr);
                SetFullscreenStates(device);
                BindSampler(device, 0, SceneCopyTexture, D3DTEXF_POINT);
                BindSampler(device, 1, sceneTexture, D3DTEXF_POINT);
                device->SetPixelShader(RainLayerPS);
                DrawFullscreen(device, static_cast<float>(HistoryWidth), static_cast<float>(HistoryHeight));
            }
            bInternalDraw = false;
            sceneTexture->Release();
            RainFrame = SceneFrame;
        }
        else
        {
            // Nothing to take the difference to: the rain goes into the scene as the game drew it
            device->StretchRect(SceneCopySurface, nullptr, scene, nullptr, D3DTEXF_POINT);
        }
        scene->Release();
    }

    // Adds the layer to target: the resolved picture, at any size, or the scene that was not resolved
    static void AddRain(IDirect3DSurface9* target, bool resolved)
    {
        using namespace TemporalAA;

        if (RainFrame != SceneFrame || !RainLayerTexture || !target)
            return;
        RainFrame = 0;
        auto device = RealDevice();
        D3DSURFACE_DESC desc{};
        if (!device || FAILED(target->GetDesc(&desc)))
            return;

        // The rain was drawn with the jitter, which the resolved picture no longer has
        float offset[4] =
        {
            resolved ? CurrentCamera.JitterPixels[0] / static_cast<float>(HistoryWidth) : 0.0f,
            resolved ? CurrentCamera.JitterPixels[1] / static_cast<float>(HistoryHeight) : 0.0f,
            0.0f, 0.0f,
        };

        bInternalDraw = true;
        {
            StateBackup backup(device);
            device->SetRenderTarget(0, target);
            for (DWORD i = 1; i < 4; ++i)
                device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            SetFullscreenStates(device, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            BindSampler(device, 0, RainLayerTexture, resolved ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            device->SetPixelShader(RainCompositePS);
            device->SetPixelShaderConstantF(0, offset, 1);
            DrawFullscreen(device, static_cast<float>(desc.Width), static_cast<float>(desc.Height));
        }
        bInternalDraw = false;
    }

    // ---------------------------------------------------------------------------------------------
    // Stipple before the resolve, render thread
    //
    // Fading objects are drawn stippled, a pattern of pixels, and PostFX's stipple filter smooths it. The resolve, DLAA
    // and FSR used to come first and kept the pattern, or blurred it into a shimmer that left the filter nothing to find.
    // With them the filter runs on the scene at the render size, before they do, into the copy, which the resolve
    // then reads; the scene itself gets it only when nothing resolved it (KeepStipple).

    static inline uint32_t StippleFrame = 0;    // SceneFrame the scene was filtered in

    static IDirect3DTexture9* FilterStipple(IDirect3DTexture9* scene, IDirect3DPixelShader9* filter)
    {
        using namespace TemporalAA;

        if (!scene || !filter || StippleFrame == SceneFrame)
            return nullptr;
        auto device = RealDevice();
        // GBUFFER_2.w marks the stippled pixels, which the filter keeps to with Definition on (c223.z)
        auto gbuffer2 = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_2_");
        if (!device || !gbuffer2 || !gbuffer2->mD3DTexture)
            return nullptr;

        IDirect3DSurface9* sceneSurface = nullptr;
        D3DSURFACE_DESC desc{};
        if (FAILED(scene->GetSurfaceLevel(0, &sceneSurface)) || !sceneSurface || FAILED(sceneSurface->GetDesc(&desc)))
        {
            SAFE_RELEASE(sceneSurface);
            return nullptr;
        }
        auto copy = GetSceneCopy(device, desc);
        if (!copy)
        {
            sceneSurface->Release();
            return nullptr;
        }

        auto width = static_cast<float>(desc.Width);
        auto height = static_cast<float>(desc.Height);
        float texelSize[4] = { 1.0f / width, 1.0f / height, width, height };   // TexelSize, c76
        float oldTexelSize[4]{};
        device->GetPixelShaderConstantF(76, oldTexelSize, 1);

        bInternalDraw = true;
        {
            StateBackup backup(device);
            SamplerBackup sampler2(device, 2);
            device->SetRenderTarget(0, copy);
            for (DWORD i = 1; i < 4; ++i)
                device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            SetFullscreenStates(device);
            BindSampler(device, 0, gbuffer2->mD3DTexture, D3DTEXF_POINT);
            // The filter reads its neighbours half a texel off, two texels at once
            BindSampler(device, 2, scene, D3DTEXF_LINEAR);
            device->SetPixelShader(filter);
            device->SetPixelShaderConstantF(76, texelSize, 1);
            DrawFullscreen(device, width, height);
            device->SetPixelShaderConstantF(76, oldTexelSize, 1);
        }
        bInternalDraw = false;

        sceneSurface->Release();
        StippleFrame = SceneFrame;
        return SceneCopyTexture;
    }

    // The filtered copy into the scene, for a scene nothing resolved
    static void KeepStipple(IDirect3DTexture9* scene)
    {
        auto device = RealDevice();
        IDirect3DSurface9* sceneSurface = nullptr;
        if (!device || !scene || !SceneCopySurface || FAILED(scene->GetSurfaceLevel(0, &sceneSurface)) || !sceneSurface)
            return;
        device->StretchRect(SceneCopySurface, nullptr, sceneSurface, nullptr, D3DTEXF_POINT);
        sceneSurface->Release();
    }

    // ---------------------------------------------------------------------------------------------
    // Steady depth, render thread, right after the fog pass
    //
    // The logarithmic depth of _DEFERRED_GBUFFER_3_, on edges blended with its reprojected history clamped to the
    // neighbourhood, so it holds still where the jitter flips it between two surfaces. The coronas test their
    // occlusion against it at a dozen points around their centre, which made the partly hidden ones flicker; depth of
    // field and sun shafts read it after the resolve.

    static inline SafetyHookInline shRenderCoronas{};
    static inline uint32_t SteadyIndex = 0;
    static inline uint32_t SteadyFrame = 0;     // SceneFrame of SteadyDepthRT[SteadyIndex]

    static void StabilizeDepth()
    {
        using namespace TemporalAA;

        if (!bSteadyDepth || !SteadyDepthPS || !IsJitterActive() || SteadyFrame == SceneFrame || MotionFrame != SceneFrame || !CurrentCamera.Valid)
            return;
        if (!SteadyDepthRT[0] || !SteadyDepthRT[0]->mD3DTexture || !SteadyDepthRT[1] || !SteadyDepthRT[1]->mD3DTexture || !MotionRT || !MotionRT->mD3DTexture)
            return;
        auto device = RealDevice();
        auto depthRT = rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_3_");
        if (!device || !depthRT || !depthRT->mD3DTexture || CurrentCamera.Near <= 0.0f || CurrentCamera.Far <= CurrentCamera.Near)
            return;

        auto previousIndex = SteadyIndex;
        auto currentIndex = SteadyIndex ^ 1u;
        IDirect3DSurface9* surface = nullptr;
        SteadyDepthRT[currentIndex]->mD3DTexture->GetSurfaceLevel(0, &surface);
        if (!surface)
            return;

        bool historyValid = SteadyFrame != 0 && SteadyFrame + 1 == SceneFrame && PreviousCamera.Valid && !IsCameraCut();
        auto width = static_cast<float>(HistoryWidth);
        auto height = static_cast<float>(HistoryHeight);
        // A neighbourhood more than 2% of the distance deep is an edge
        auto edge = std::log2(1.02f) / std::log2(CurrentCamera.Far / CurrentCamera.Near);
        float constants[3 * 4] =
        {
            1.0f / width, 1.0f / height, width, height,
            0.1f, 0.5f, 0.25f, historyValid ? 1.0f : 0.0f,
            edge, 0.0f, 0.0f, 0.0f,
        };

        bInternalDraw = true;
        {
            StateBackup backup(device);
            SamplerBackup sampler1(device, 1);
            SamplerBackup sampler2(device, 2);
            device->SetRenderTarget(0, surface);
            for (DWORD i = 1; i < 4; ++i)
                device->SetRenderTarget(i, nullptr);
            device->SetDepthStencilSurface(nullptr);
            SetFullscreenStates(device);
            BindSampler(device, 0, depthRT->mD3DTexture, D3DTEXF_POINT);
            BindSampler(device, 1, SteadyDepthRT[previousIndex]->mD3DTexture, D3DTEXF_POINT);
            BindSampler(device, 2, MotionRT->mD3DTexture, D3DTEXF_POINT);
            device->SetPixelShader(SteadyDepthPS);
            device->SetPixelShaderConstantF(0, constants, 3);
            DrawFullscreen(device, width, height);
        }
        bInternalDraw = false;
        surface->Release();

        SteadyIndex = currentIndex;
        SteadyFrame = SceneFrame;
    }

    static IDirect3DTexture9* GetSteadyDepth()
    {
        auto rt = SteadyDepthRT[SteadyIndex];
        return SteadyFrame != 0 && SteadyFrame == TemporalAA::SceneFrame && rt ? rt->mD3DTexture : nullptr;
    }

    // CCoronas::Render, a draw list callback after the fog pass: its effect reads _DEFERRED_GBUFFER_3_, which holds the
    // steady depth while it draws
    static void __cdecl RenderCoronas()
    {
        auto steady = GetSteadyDepth();
        auto depthRT = steady ? rage::grcTextureFactoryPC::GetRTByName("_DEFERRED_GBUFFER_3_") : nullptr;
        IDirect3DTexture9* own = nullptr;
        if (depthRT && depthRT->mD3DTexture)
        {
            own = depthRT->mD3DTexture;
            depthRT->mD3DTexture = steady;
        }
        if (!CoronaRender::Draw())
            shRenderCoronas.unsafe_ccall();
        if (own)
            depthRT->mD3DTexture = own;
    }

    // ---------------------------------------------------------------------------------------------
    // DLAA and FSR

    static inline uint32_t UpscalerFrame = 0;             // SceneFrame of the last upscaled frame
    static inline uint32_t UpscaleTried = 0;              // SceneFrame DLSS or FSR was last asked to resolve
    static inline LARGE_INTEGER UpscalerTime{};

    // Ctrl+Shift+F8: the upscaler's inputs and output of one frame as DDS files next to the plugin, and the jitter of
    // the frames before it in GTAIV.EFLC.FusionFix.Upscaler.log, to tell what the upscaler was given from what it made
    static inline float RecentJitter[32][2]{};
    static inline uint32_t RecentJitterCount = 0;

    static void DumpUpscalerFrame(const Upscaler::Frame& frame)
    {
        auto save = [](IDirect3DTexture9* texture, const wchar_t* name)
        {
            if (!texture)
                return;
            auto path = FusionLog::PathFor("Upscaler", std::wstring(L".") + name + L".dds");
            auto hr = D3DXSaveTextureToFileW(path.c_str(), D3DXIFF_DDS, texture, nullptr);
            D3DSURFACE_DESC desc{};
            texture->GetLevelDesc(0, &desc);
            FusionLog::Write("Upscaler", "Dump", "%ls: %ux%u format %d, saved 0x%08lX", name, desc.Width, desc.Height,
                int(desc.Format), hr);
        };
        save(frame.Color, L"Color");
        save(frame.Depth, L"Depth");
        save(frame.Motion, L"Motion");
        save(frame.Reactive, L"Reactive");
        save(frame.Output, L"Output");
        FusionLog::Write("Upscaler", "Dump", "render %ux%u, output %ux%u, jitter %.4f %.4f, near %.4f far %.1f fovY %.4f, reset %d, "
            "sharpness %.2f, render scale %.4f", frame.Width, frame.Height, frame.OutputWidth, frame.OutputHeight, frame.JitterX,
            frame.JitterY, frame.CameraNear, frame.CameraFar, frame.CameraFovY, int(frame.Reset), frame.Sharpness, RenderScale::GetScale());
        auto count = std::min<uint32_t>(RecentJitterCount, 32);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& j = RecentJitter[(RecentJitterCount - count + i) % 32];
            FusionLog::Write("Upscaler", "Dump", "jitter -%u: %.4f %.4f", count - 1 - i, j[0], j[1]);
        }
    }

    static bool Upscale(IDirect3DDevice9* device, Upscaler::Backend backend, IDirect3DTexture9* scene, IDirect3DTexture9* output)
    {
        using namespace TemporalAA;

        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        auto frameTime = UpscalerTime.QuadPart ? static_cast<float>(static_cast<double>(now.QuadPart - UpscalerTime.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart)) : 16.6f;
        UpscalerTime = now;

        Upscaler::Frame frame;
        frame.Color = scene;
        frame.Depth = Temporal::DepthRT->mD3DTexture;
        frame.Motion = MotionRT->mD3DTexture;
        // NVIDIA advises against a mask for DLSS
        if (Upscaler::Profile)
            Upscaler::Profile(device, Upscaler::ProfilePart::Reactive, true);
        frame.Reactive = backend == Upscaler::Backend::FSR ? RenderReactive(device, scene) : nullptr;
        if (Upscaler::Profile)
            Upscaler::Profile(device, Upscaler::ProfilePart::Reactive, false);
        frame.Output = output;
        frame.Width = static_cast<uint32_t>(HistoryWidth);
        frame.Height = static_cast<uint32_t>(HistoryHeight);
        frame.OutputWidth = RenderScale::GetDisplayWidth();
        frame.OutputHeight = RenderScale::GetDisplayHeight();
        frame.JitterX = CurrentCamera.JitterPixels[0];
        frame.JitterY = CurrentCamera.JitterPixels[1];
        frame.CameraNear = CurrentCamera.Near;
        frame.CameraFar = CurrentCamera.Far;
        frame.CameraFovY = CurrentCamera.FovY;
        frame.FrameTimeMs = std::clamp(frameTime, 1.0f, 200.0f);
        frame.Sharpness = backend == Upscaler::Backend::FSR ? fFSRSharpness : 0.0f;
        frame.DLSSPreset = static_cast<uint32_t>(nDLSSPreset);
        frame.Reset = !(UpscalerFrame != 0 && UpscalerFrame + 1 == SceneFrame && PreviousCamera.Valid && !IsCameraCut());

        // The camera in world space: rows of the inverse view, which looks along -z
        frame.FrameGeneration = FrameGeneration::IsEnabled();
        frame.HighDynamicRange = HDROutput::IsActive();
        frame.HudLess = FrameGeneration::UsesHudLess();
        frame.AsyncGeneration = FrameGeneration::UsesAsyncCompute();
        frame.PresentWindow = FrameGeneration::HelperPresentWindow();
        frame.HelperPresent = frame.PresentWindow != nullptr;
        frame.AmdPacing = FrameGeneration::UsesAmdPacing();
        auto world = CurrentCamera.View.Inverse();
        auto normalized = [&](int row, float sign, float (&out)[3])
        {
            auto length = std::sqrt(world.m[row][0] * world.m[row][0] + world.m[row][1] * world.m[row][1] + world.m[row][2] * world.m[row][2]);
            for (int i = 0; i < 3; ++i)
                out[i] = length > 0.0 ? static_cast<float>(sign * world.m[row][i] / length) : 0.0f;
        };
        normalized(0, 1.0f, frame.CameraRight);
        normalized(1, 1.0f, frame.CameraUp);
        normalized(2, -1.0f, frame.CameraForward);
        for (int i = 0; i < 3; ++i)
            frame.CameraPosition[i] = static_cast<float>(world.m[3][i]);

        if (Upscaler::Profile)
            Upscaler::Profile(device, Upscaler::ProfilePart::Upscale, true);
        bool evaluated = Upscaler::Evaluate(backend, frame);
        if (Upscaler::Profile)
            Upscaler::Profile(device, Upscaler::ProfilePart::Upscale, false);
        if (!evaluated)
            return false;

        RecentJitter[RecentJitterCount % 32][0] = frame.JitterX;
        RecentJitter[RecentJitterCount % 32][1] = frame.JitterY;
        ++RecentJitterCount;
        static bool dumpKeyDown = false;
        bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (down && !dumpKeyDown)
            DumpUpscalerFrame(frame);
        dumpKeyDown = down;

        UpscalerFrame = SceneFrame;
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // Resolve, called by PostFX on the HDR scene before any other post processing. Writes the result
    // into output (texture and its surface) and returns true, or leaves it untouched. The rain goes into whichever
    // the caller shows.

    static bool Resolve(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DTexture9* outputTexture, IDirect3DSurface9* output)
    {
        if (ResolveScene(device, scene, outputTexture, output))
        {
            AddRain(output, true);
            return true;
        }
        // Into the scene itself only DLSS and FSR resolve: the caller tries again with a target of its own
        if (outputTexture == scene)
            return false;

        IDirect3DSurface9* sceneSurface = nullptr;
        if (RainFrame == TemporalAA::SceneFrame && scene && SUCCEEDED(scene->GetSurfaceLevel(0, &sceneSurface)))
            AddRain(sceneSurface, false);
        SAFE_RELEASE(sceneSurface);
        return false;
    }

private:
    static bool ResolveScene(IDirect3DDevice9* device, IDirect3DTexture9* scene, IDirect3DTexture9* outputTexture, IDirect3DSurface9* output)
    {
        using namespace TemporalAA;

        if (!scene || !output || !CurrentCamera.Valid)
            return false;

        // Once a scene: a second call after a failed one, into a target of the caller's, goes to temporal AA
        auto mode = GetMode();
        if ((mode == Mode::DLAA || mode == Mode::FSR) && UpscaleTried != SceneFrame && MotionFrame == SceneFrame)
        {
            UpscaleTried = SceneFrame;
            auto backend = mode == Mode::DLAA ? Upscaler::Backend::DLSS : Upscaler::Backend::FSR;
            if (outputTexture && Upscale(device, backend, scene, outputTexture))
            {
                // Resolved for temporal.ixx too (the jitter comes off what is drawn after the post processing), whose
                // history did not follow these frames
                Temporal::ResolvedFrame = Temporal::Frame;
                Temporal::LastBridged = true;
                ResolveFrame = SceneFrame;
                ResolvePresent = PresentFrame.load();
                return true;
            }
        }
        UpscalerFrame = 0;

        // Temporal AA resolves at the render size only, the post processing stretches the scene instead; nor into the
        // scene it reads, which only DLSS and FSR write
        if (RenderScale::IsActive() || outputTexture == scene || !Temporal::Resolve(device, scene, output))
            return false;
        ResolveFrame = SceneFrame;
        ResolvePresent = PresentFrame.load();
        return true;
    }

public:
    static bool IsCameraCut()
    {
        using namespace TemporalAA;

        auto currentInverse = CurrentCamera.View.Inverse();
        auto previousInverse = PreviousCamera.View.Inverse();
        auto dx = currentInverse.m[3][0] - previousInverse.m[3][0];
        auto dy = currentInverse.m[3][1] - previousInverse.m[3][1];
        auto dz = currentInverse.m[3][2] - previousInverse.m[3][2];
        if (dx * dx + dy * dy + dz * dz > fCameraCutDistance * fCameraCutDistance)
            return true;

        // forward vectors more than ~60 degrees apart
        auto dot = currentInverse.m[2][0] * previousInverse.m[2][0] + currentInverse.m[2][1] * previousInverse.m[2][1] + currentInverse.m[2][2] * previousInverse.m[2][2];
        return dot < 0.5;
    }

    // At startup and on Ctrl+Shift+F10 (IniReload): each of these is looked at every frame; the ones that decide which
    // targets are made are read at startup alone. Temporal AA's own are temporal.ixx's ([TEMPORAL]).
    static void ReadSettings(SettingsReader& reader)
    {
        nJitterPhases = std::clamp(reader.ReadInteger("ANTIALIASING", "JitterPhases", 8), 2, 128);
        fCurrentWeightMin = std::clamp(reader.ReadFloat("ANTIALIASING", "CurrentFrameWeightMin", 0.08f), 0.01f, 1.0f);
        fCurrentWeightMax = std::clamp(reader.ReadFloat("ANTIALIASING", "CurrentFrameWeightMax", 0.2f), fCurrentWeightMin, 1.0f);
        fCurrentWeightPerPixel = std::max(reader.ReadFloat("ANTIALIASING", "CurrentFrameWeightPerPixelOfMotion", 0.1f), 0.0f);
        fVarianceClipGamma = std::clamp(reader.ReadFloat("ANTIALIASING", "VarianceClipGamma", 1.25f), 0.5f, 4.0f);
        bObjectMotionVectors = reader.ReadInteger("ANTIALIASING", "ObjectMotionVectors", 1) != 0;
        bSkinnedMotionVectors = reader.ReadInteger("ANTIALIASING", "SkinnedMotionVectors", 1) != 0;
        fObjectDepthBias = std::clamp(reader.ReadFloat("ANTIALIASING", "ObjectDepthBias", 0.00002f), 0.0f, 0.01f);
        fCameraCutDistance = std::max(reader.ReadFloat("ANTIALIASING", "CameraCutDistance", 25.0f), 1.0f);
        nDLSSPreset = std::clamp(reader.ReadInteger("ANTIALIASING", "DLSSPreset", 0), 0, 15);
        fFSRSharpness = std::clamp(reader.ReadFloat("ANTIALIASING", "FSRSharpness", 0.0f), 0.0f, 1.0f);
        fReactiveScale = std::clamp(reader.ReadFloat("ANTIALIASING", "ReactiveMaskScale", 2.0f), 0.0f, 64.0f);
        fReactiveMax = std::clamp(reader.ReadFloat("ANTIALIASING", "ReactiveMaskMaximum", 0.75f), 0.0f, 1.0f);
        bRainLayer = reader.ReadInteger("ANTIALIASING", "RainAfterAntialiasing", 1) != 0;
    }

    TemporalLayer()
    {
        // temporal.ixx calls these from its init on; set before any init event
        TemporalHooks::SceneBegin = OnSceneBegin;
        TemporalHooks::BeforeSceneEnd = BeforeSceneEnd;
        TemporalHooks::AfterSceneEnd = AfterSceneEnd;
        TemporalHooks::AppendBeforeSceneEnd = []()
        {
            if (TemporalAA::AppendBeforeSceneEnd)
                TemporalAA::AppendBeforeSceneEnd();
        };
        TemporalHooks::JitterAllowed = IsJitterActive;
        TemporalHooks::Draw = RecordDraw;
        TemporalHooks::Resolve = Resolve;
        TemporalHooks::LoadShaders = LoadShaders;
        TemporalHooks::CreateResources = CreateResources;
        TemporalHooks::ReleaseResources = ReleaseResources;
        TemporalHooks::OwnsAvailability = true;

        FusionFix::onInitEventAsync() += []()
        {
            SettingsReader reader;
            // These size the targets (CreateResources): read at startup only
            bReactiveMask = reader.ReadInteger("ANTIALIASING", "ReactiveMask", 1) != 0;
            bSteadyDepth = reader.ReadInteger("ANTIALIASING", "SteadyDepth", 1) != 0;
            ReadSettings(reader);
            IniReload::Add(ReadSettings);

            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&Resolve, &hm);

            // CRain::Render, called by the visual effects' draw list callback
            auto pattern = hook::pattern("55 8B EC 83 E4 F0 83 EC 6C B9 ? ? ? ? 56 FF 35");
            if (!pattern.empty())
                shRenderRain = safetyhook::create_inline(pattern.get_first(0), RenderRain);

            // CCoronas::Render, called by its draw list callback
            pattern = hook::pattern("53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 56 57 E8");
            if (!pattern.empty())
                shRenderCoronas = safetyhook::create_inline(pattern.get_first(0), RenderCoronas);

            FusionFixSettings.SetAvailability("PREF_ANTIALIASING", [](int32_t value) -> bool
            {
                switch (value)
                {
                case FusionFixSettings.AntialiasingText.eTAA:
                    // resolved by Fusion Fix post processing
                    return GetD3DX9_43DLL() != nullptr;
                // While the helper starts they count as there, as for the render scale: moved to TAA meanwhile, the
                // scale was decided at 1.0 and put right with a device reset in the menu once the helper answered,
                // which made the next loading much longer
                case FusionFixSettings.AntialiasingText.eDLAA:
                    return Upscaler::MayBeAvailable(Upscaler::Backend::DLSS);
                case FusionFixSettings.AntialiasingText.eFSR:
                    return Upscaler::MayBeAvailable(Upscaler::Backend::FSR);
                default:
                    return true;
                }
            });
            FusionFixSettings.RefreshAvailability("PREF_ANTIALIASING");

            // DLAA and FSR become available once the helper has started, a saved choice comes back then. The first
            // call comes after the device was created, when it's known whether SMAA can run.
            static auto refreshAvailability = []()
            {
                static uint32_t generation = UINT32_MAX;
                if (generation != Upscaler::Generation())
                {
                    generation = Upscaler::Generation();
                    FusionFixSettings.RefreshAvailability("PREF_ANTIALIASING");
                }
            };
            FusionFix::onGameProcessEvent() += []() { refreshAvailability(); UpdateJitterPhases(); ApplyCoreSettings(); };
            FusionFix::onMenuDrawingEvent() += []() { refreshAvailability(); };

            FusionFix::onShutdownEvent() += []()
            {
                Upscaler::Shutdown();
            };

            FusionFix::onAfterEndScene() += []()
            {
                ++PresentFrame;
                // The main menu draws no scene: without this the helper started, and DLAA and FSR became
                // available, only once the world was loaded
                Upscaler::Update();
            };

            FusionFix::onBeforeReset() += []()
            {
                ReleaseSceneTargets();
            };
        };
    }
} TemporalLayer;

namespace TemporalDraws
{
    // A draw's world matrix as CEntity holds one (right, forward, up, position rows). The registers hold it either as it
    // is or transposed: the translation is in the last row or column. A skinned draw (a car's body) has only its
    // position there, the rotation is its bones': the first, the root (a car's chassis), turns it, its registers
    // holding the columns of a 4x3, x y z of each axis and of an offset added to the position
    inline bool DrawMatrix(const TemporalLayer::DrawRecord& draw, float m[16])
    {
        const float* w = draw.World;
        const bool rows = std::abs(w[12]) + std::abs(w[13]) + std::abs(w[14]) >= std::abs(w[3]) + std::abs(w[7]) + std::abs(w[11]);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[r * 4 + c] = rows ? w[r * 4 + c] : w[c * 4 + r];
        if (draw.Bones >= 0 && size_t(draw.Bones) + 12 <= TemporalLayer::BonesPrevious.size())
        {
            const float* b = &TemporalLayer::BonesPrevious[draw.Bones];
            for (int axis = 0; axis < 4; ++axis)
                for (int k = 0; k < 3; ++k)
                    m[axis * 4 + k] = axis < 3 ? b[k * 4 + axis] : m[12 + k] + b[k * 4 + 3];
        }
        return std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]);
    }
}

export namespace TemporalAA
{
    bool IsMotionVectorsReady()
    {
        return TemporalLayer::MotionFrame == SceneFrame && TemporalLayer::MotionRT;
    }

    float ObjectDepthBias()
    {
        return TemporalLayer::fObjectDepthBias;
    }

    // Render thread, after the G-buffer pass: the world matrix this frame's G-buffer drew the entity with, in CEntity's
    // layout (right, forward, up, position rows). Of its draws, the one standing nearest `around` (the entity's position
    // as known), within `reach` metres. False when object motion vectors are off or the entity was not drawn.
    // drawCount and nearest, where given: how many draws the entity had (-1 none recorded) and how far the nearest
    // draw's matrix stood from around, for the box shadows' log
    // heading, where given (x, y of the forward expected): only draws facing within about 25 degrees of it count, the
    // car's own parts drawn with its rotation (a skinned body may carry none of its own; wheels turn and doors swing)
    // skinned, where given: whether the draw taken was skinned, its matrix the entity's own (a car's body by its chassis)
    // bodyReach: how far a skinned draw may stand from around. All the draws are the entity's own, the reach kept other
    // parts out; a log found the body 3 to 9 m from the place moved on by speed when driven fast. Of the skinned draws
    // the one with the most primitives is taken, the body: not every draw's first bone is the chassis (a small one's
    // stood turned and 0.3 m off)
    bool DrawnWorldMatrix(void* entity, const float around[3], float reach, float out[16], int* drawCount, float* nearest,
        const float* heading, bool* skinned, float bodyReach)
    {
        // Swapped at the G-buffer's end: the draws just made are the "previous" ones until the next scene
        auto it = TemporalLayer::EntitiesPrevious.find(entity);
        if (drawCount)
            *drawCount = it == TemporalLayer::EntitiesPrevious.end() ? -1 : int(it->second.Draws.size());
        if (nearest)
            *nearest = -1.0f;
        if (it == TemporalLayer::EntitiesPrevious.end())
            return false;
        float best = reach * reach;
        bool found = false;
        UINT bodyPrimitives = 0;
        for (auto& draw : it->second.Draws)
        {
            float m[16];
            if (!TemporalDraws::DrawMatrix(draw, m))
                continue;
            if (heading)
            {
                // A skinned draw is turned by its root bone, the car's own heading: within 60 degrees of the one
                // expected, which a car spun or turned sharply had left by 27 degrees (the expected lagging it). Another
                // part within 25 degrees, wheels and doors turned their own way
                const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
                if (!(flat > 0.5f) || (m[4] * heading[0] + m[5] * heading[1]) / flat < (draw.Bones >= 0 ? 0.5f : 0.9f))
                    continue;
            }
            const float dx = m[12] - around[0], dy = m[13] - around[1], dz = m[14] - around[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (nearest && (*nearest < 0.0f || d2 < *nearest * *nearest))
                *nearest = std::sqrt(d2);
            if (draw.Bones >= 0 && d2 < bodyReach * bodyReach)
            {
                if (draw.PrimitiveCount > bodyPrimitives)
                {
                    bodyPrimitives = draw.PrimitiveCount;
                    std::copy_n(m, 16, out);
                    found = true;
                    if (skinned)
                        *skinned = true;
                }
                continue;
            }
            if (!bodyPrimitives && d2 < best)
            {
                best = d2;
                std::copy_n(m, 16, out);
                found = true;
                if (skinned)
                    *skinned = draw.Bones >= 0;
            }
        }
        return found;
    }

    // Render thread, after the G-buffer pass: where this frame's G-buffer drew the entity, its world matrix's position
    // (a ped's draws, skinned, hold the ped's own there, the pose in the bones), of its draws the one nearest `around`
    // within `reach` metres. nearest, where given: how far the nearest draw stood (-1 none recorded), for the log.
    bool DrawnPosition(void* entity, const float around[3], float reach, float out[3], float* nearest)
    {
        if (nearest)
            *nearest = -1.0f;
        auto it = TemporalLayer::EntitiesPrevious.find(entity);
        if (it == TemporalLayer::EntitiesPrevious.end())
            return false;
        float best = -1.0f;
        for (auto& draw : it->second.Draws)
        {
            float p[3];
            TemporalLayer::DrawPosition(draw.World, nullptr, p);
            const float dx = p[0] - around[0], dy = p[1] - around[1], dz = p[2] - around[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(d2) && (best < 0.0f || d2 < best))
            {
                best = d2;
                std::copy_n(p, 3, out);
            }
        }
        if (nearest && best >= 0.0f)
            *nearest = std::sqrt(best);
        return best >= 0.0f && best < reach * reach;
    }

    // The box shadows' log: the entity's draws from the last G-buffer, nearest `around` first, at most 12, each with its
    // distance, whether skinned, how its registers held the matrix and its axes (x y z of right, forward and up, a
    // skinned draw's turned by its root bone), the cosine between its forward on the ground and `heading`, and a
    // skinned draw's root bone registers as they were
    std::string DescribeDraws(void* entity, const float around[3], const float* heading)
    {
        auto it = TemporalLayer::EntitiesPrevious.find(entity);
        if (it == TemporalLayer::EntitiesPrevious.end())
            return "no draws";
        struct Line { float d; std::string text; };
        std::vector<Line> lines;
        for (auto& draw : it->second.Draws)
        {
            const float* w = draw.World;
            const bool rows = std::abs(w[12]) + std::abs(w[13]) + std::abs(w[14]) >= std::abs(w[3]) + std::abs(w[7]) + std::abs(w[11]);
            float m[16];
            TemporalDraws::DrawMatrix(draw, m);
            const float dx = m[12] - around[0], dy = m[13] - around[1], dz = m[14] - around[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float flat = std::sqrt(m[4] * m[4] + m[5] * m[5]);
            const float cosine = heading && flat > 0.0f ? (m[4] * heading[0] + m[5] * heading[1]) / flat : 0.0f;
            char text[256];
            std::snprintf(text, sizeof(text), "%.2fm %s%s r(%.2f %.2f %.2f) f(%.2f %.2f %.2f) u(%.2f %.2f %.2f) cos %.2f prims %u",
                d, draw.Bones >= 0 ? "skinned " : "", rows ? "rows" : "cols", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                cosine, draw.PrimitiveCount);
            std::string line = text;
            if (draw.Bones >= 0 && size_t(draw.Bones) + 12 <= TemporalLayer::BonesPrevious.size())
            {
                const float* b = &TemporalLayer::BonesPrevious[draw.Bones];
                std::snprintf(text, sizeof(text), " bone0 (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f) (%.2f %.2f %.2f %.2f)",
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
                line += text;
            }
            lines.push_back({ std::isfinite(d) ? d : 1e9f, line });
        }
        std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.d < b.d; });
        char head[96];
        std::snprintf(head, sizeof(head), "%zu draws, heading %.2f %.2f:", lines.size(), heading ? heading[0] : 0.0f, heading ? heading[1] : 0.0f);
        std::string out = head;
        for (size_t i = 0; i < lines.size() && i < 12; ++i)
        {
            out += " | ";
            out += lines[i].text;
        }
        return out;
    }

    IDirect3DTexture9* GetMotionVectors()
    {
        return TemporalLayer::MotionRT ? TemporalLayer::MotionRT->mD3DTexture : nullptr;
    }

    bool HistoryValid()
    {
        return Temporal::HistoryValid;
    }

    // PostFX's stipple filter on the scene at the render size, right before the resolve; false if it did not run
    // The filtered scene for the resolve to read, or null if the filter did not run
    IDirect3DTexture9* FilterStipple(IDirect3DTexture9* scene, IDirect3DPixelShader9* filter)
    {
        return TemporalLayer::FilterStipple(scene, filter);
    }

    // When nothing resolved the scene: the filtered copy goes into it
    void KeepStipple(IDirect3DTexture9* scene)
    {
        TemporalLayer::KeepStipple(scene);
    }

    // The scene of this frame was filtered before the resolve
    bool IsStippleFiltered()
    {
        return SceneFrame != 0 && TemporalLayer::StippleFrame == SceneFrame;
    }

    // Depth averaged over the jitter for what reads it after the resolve (depth of field, sun shafts), at the render
    // size, or null when this frame has none
    IDirect3DTexture9* GetSteadyDepth()
    {
        return TemporalLayer::GetSteadyDepth();
    }

    // Called by PostFX right after the fog pass of the scene
    void OnFogDrawn()
    {
        TemporalLayer::CaptureOpaque();
        TemporalLayer::StabilizeDepth();
    }
}
