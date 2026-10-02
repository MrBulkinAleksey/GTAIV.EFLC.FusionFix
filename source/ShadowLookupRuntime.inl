// OnyxOak modification project: Extra Night Shadows Fix and Better Headlights.
// Project direction, integration and visual testing by OnyxOak; Codex-assisted development.
// Modification notice: 2026-09-27. See ATTRIBUTION.md for upstream credits and GPL-3.0.
// Official release: https://www.nexusmods.com/gta4/mods/1459

namespace ShadowLookupGuard {
    static bool ready = false;
    static uintptr_t base = 0;
    static std::atomic<uint32_t> rejectedDynamic{0}, rejectedStatic{0}, changedBuffer{0};
    static int ReadBuffer() noexcept {
        return ready ? *reinterpret_cast<const int*>(base + shadow_lookup_layout::ReadBufferRva) : -1;
    }
    static void Initialize() noexcept {
        base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        ready = shadow_lookup_layout::Validate(base);
    }
    static int Filter(int nativeResult, uint32_t key, int cache, int before) noexcept {
        const auto traceResult=[&](int result,int buffer) noexcept {
            if(ShadowTrace34::Tracked(key)) ShadowTrace34::Emit({4,
                CTimer::m_frameCount?*CTimer::m_frameCount:0,GetTickCount(),key,
                nativeResult,result,cache,buffer,0,0,0,0});
            return result;
        };
        if (!ready || !bExtraNightShadows || nativeResult <= 0 || !key || key == UINT32_MAX)
            return traceResult(nativeResult,before);
        const int current = ReadBuffer();
        if (current < 0 || current > 1 || current != before) {
            ++changedBuffer;
            return traceResult(-1,current);
        }
        fusionfix::shadows::ShadowLookupSnapshot s{};
        const auto dynamic = base + shadow_lookup_layout::DynamicKey0Rva + current * 0x880;
        const auto cached = base + shadow_lookup_layout::StaticKey0Rva + current * 0x1000;
        if (nativeResult < 8) {
            const auto address = dynamic + nativeResult * 0x110;
            s.dynamicKeys[nativeResult] = *reinterpret_cast<const uint32_t*>(address);
            s.dynamicActive[nativeResult] = *reinterpret_cast<const uint8_t*>(address - 0xB);
            const int dependency=*reinterpret_cast<const int*>(address - 8);
            s.dynamicCache[nativeResult]=dependency;
            if(dependency>=0 && dependency<8)
                s.staticKeys[dependency]=*reinterpret_cast<const uint32_t*>(cached+dependency*0x100);
            if (cache >= 0 && cache < 8)
                s.staticKeys[cache] = *reinterpret_cast<const uint32_t*>(cached + cache * 0x100);
        } else if (nativeResult < 16) {
            s.staticKeys[nativeResult - 8] = *reinterpret_cast<const uint32_t*>(cached + (nativeResult - 8) * 0x100);
        }
        const int result = fusionfix::shadows::ValidateShadowLookup(nativeResult,key,cache,s);
        if (result != nativeResult) {
            if (nativeResult < 8) ++rejectedDynamic; else ++rejectedStatic;
        }
        return traceResult(result,current);
    }
}
