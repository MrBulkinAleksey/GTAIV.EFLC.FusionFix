#pragma once
#include <sstream>
#include <string>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Exact CE 1.2.0.59 image audited locally. These offsets must not be applied to
// another executable merely because a short headlight flag pattern matches.
namespace fusionfix::shadows::ce
{
    inline constexpr std::uint32_t ImageSize = 0x01BE6400;
    inline constexpr std::uint32_t Timestamp = 0x63D3E735;
    inline constexpr std::uint32_t PreferredBase = 0x00400000;
    inline constexpr std::uint32_t SubmitRva = 0x006BCC50;
    inline constexpr std::uint32_t FrameCounterRva = 0x00D73604;
    inline constexpr std::array<std::uint32_t, 4> CallRvas{
        0x0063DFEA, 0x0063E1CA, 0x0063DFFF, 0x0063E1DF
    };

    // Audited A3F5E0 vehicle submissions: both working headlights produce a
    // combined beam keyed by the vehicle; a single working lamp uses vehicle+1.
    // A selector entry therefore represents a beam request, not a physical lamp.
    inline bool IsVehicleBeam(std::uintptr_t key, std::uintptr_t vehicle) noexcept
    {
        return vehicle && (key == vehicle || (vehicle < UINTPTR_MAX && key == vehicle + 1));
    }

    inline bool Match(const std::uint8_t* image, std::size_t size, std::size_t offset,
                      const std::uint8_t* expected, std::size_t count) noexcept
    {
        return offset <= size && count <= size - offset &&
            std::memcmp(image + offset, expected, count) == 0;
    }

    inline std::uint32_t Read32(const std::uint8_t* p) noexcept
    {
        std::uint32_t result;
        std::memcpy(&result, p, sizeof(result));
        return result;
    }

    // Input is a mapped PE image, not raw on-disk section layout. Validate every
    // modified call and the actual frame source before installing any hook.
    inline bool ValidateMappedImage(const std::uint8_t* image, std::size_t size,
                                    std::uintptr_t loadedBase) noexcept
    {
        if (!image || size != ImageSize || loadedBase > UINT32_MAX - (ImageSize - 1) ||
            image[0] != 'M' || image[1] != 'Z')
            return false;
        const auto pe = Read32(image + 0x3C);
        if (pe > size - 0x80 || Read32(image + pe) != 0x4550 ||
            image[pe + 4] != 0x4C || image[pe + 5] != 0x01 ||
            Read32(image + pe + 8) != Timestamp ||
            image[pe + 24] != 0x0B || image[pe + 25] != 0x01 ||
            Read32(image + pe + 24 + 56) != ImageSize)
            return false;

        constexpr std::uint8_t submit[]{
            0xFF,0x74,0x24,0x40,0xF3,0x0F,0x10,0x44,0x24,0x38,
            0xFF,0x74,0x24,0x40,0x8B,0x44,0x24,0x14,
            0xFF,0x74,0x24,0x40,0x83,0xE0,0xF7
        };
        if (!Match(image, size, SubmitRva, submit, sizeof(submit)))
            return false;
        for (std::size_t i = 0; i < CallRvas.size(); ++i)
        {
            const auto call = CallRvas[i];
            const std::uint8_t prefix[]{0x68,0x04,static_cast<std::uint8_t>(i < 2 ? 0x05 : 0x01),0,0,0x6A,0x02,0x6A,0};
            constexpr std::uint8_t cleanup[]{0x83,0xC4,0x40};
            if (!Match(image, size, call - 9, prefix, sizeof(prefix)) || image[call] != 0xE8 ||
                !Match(image, size, call + 5, cleanup, sizeof(cleanup)) ||
                static_cast<std::uint32_t>(call + 5 + Read32(image + call + 1)) != SubmitRva)
                return false;
        }

        constexpr std::uint8_t compare[]{0x3B,0x05};
        constexpr std::uint8_t increment[]{0xFF,0x05};
        const auto counter = static_cast<std::uint32_t>(loadedBase + FrameCounterRva);
        return Match(image, size, 0x769F26, compare, sizeof(compare)) &&
            Match(image, size, 0x4D0F82, increment, sizeof(increment)) &&
            Read32(image + 0x769F28) == counter && Read32(image + 0x4D0F84) == counter;
    }

    // Diagnostics only: names the first check ValidateMappedImage fails, so a log can say
    // why the adapter stayed off. Reads nothing ValidateMappedImage would not.
    inline std::string DescribeMappedImage(const std::uint8_t* image, std::size_t size,
                                           std::uintptr_t loadedBase)
    {
        std::ostringstream out;
        out << std::hex << "image_size=" << size;
        if (!image || size != ImageSize || loadedBase > UINT32_MAX - (ImageSize - 1) ||
            image[0] != 'M' || image[1] != 'Z')
            return out.str() + " failed=image_size";
        const auto pe = Read32(image + 0x3C);
        if (pe > size - 0x80 || Read32(image + pe) != 0x4550)
            return out.str() + " failed=pe_header";
        out << " timestamp=" << Read32(image + pe + 8);
        if (image[pe + 4] != 0x4C || image[pe + 5] != 0x01 || Read32(image + pe + 8) != Timestamp ||
            image[pe + 24] != 0x0B || image[pe + 25] != 0x01 || Read32(image + pe + 24 + 56) != ImageSize)
            return out.str() + " failed=pe_timestamp";

        constexpr std::uint8_t submit[]{
            0xFF,0x74,0x24,0x40,0xF3,0x0F,0x10,0x44,0x24,0x38,
            0xFF,0x74,0x24,0x40,0x8B,0x44,0x24,0x14,
            0xFF,0x74,0x24,0x40,0x83,0xE0,0xF7
        };
        if (!Match(image, size, SubmitRva, submit, sizeof(submit)))
            return out.str() + " failed=submit_bytes";
        for (std::size_t i = 0; i < CallRvas.size(); ++i)
        {
            const auto call = CallRvas[i];
            const std::uint8_t prefix[]{0x68,0x04,static_cast<std::uint8_t>(i < 2 ? 0x05 : 0x01),0,0,0x6A,0x02,0x6A,0};
            constexpr std::uint8_t cleanup[]{0x83,0xC4,0x40};
            if (!Match(image, size, call - 9, prefix, sizeof(prefix)) || image[call] != 0xE8 ||
                !Match(image, size, call + 5, cleanup, sizeof(cleanup)))
                return out.str() + " failed=call_site_" + std::to_string(i);
            const auto target = static_cast<std::uint32_t>(call + 5 + Read32(image + call + 1));
            if (target != SubmitRva)
            {
                out << " call_site_" << i << "_target=" << target;
                return out.str() + " failed=call_site_redirected";
            }
        }

        constexpr std::uint8_t compare[]{0x3B,0x05};
        constexpr std::uint8_t increment[]{0xFF,0x05};
        const auto counter = static_cast<std::uint32_t>(loadedBase + FrameCounterRva);
        if (!Match(image, size, 0x769F26, compare, sizeof(compare)) || Read32(image + 0x769F28) != counter)
            return out.str() + " failed=frame_counter_compare";
        if (!Match(image, size, 0x4D0F82, increment, sizeof(increment)) || Read32(image + 0x4D0F84) != counter)
            return out.str() + " failed=frame_counter_increment";
        return out.str() + " ok";
    }
}
