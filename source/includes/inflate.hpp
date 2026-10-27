#pragma once

// Inflate (RFC 1950 zlib / RFC 1951 deflate), for data the plugin carries compressed: the PDB that EmbedPDB appends
// to the ASI (profiler.ixx). Huffman codes decoded through a 10-bit table, longer ones bit by bit past it; no
// checksum. Returns false on damaged input or when the output would pass `limit` bytes.

#include <cstdint>
#include <cstring>
#include <vector>

namespace Inflate
{
    namespace detail
    {
        struct Bits
        {
            const uint8_t* data;
            size_t size, pos = 0;
            uint64_t buffer = 0;
            int count = 0;

            // At least n (≤ 32) bits in the buffer; past the end zeros come in (a stream that needs them is damaged and
            // is caught by the code tables or the lengths)
            void Need(int n)
            {
                while (count < n)
                {
                    buffer |= uint64_t(pos < size ? data[pos] : 0) << count;
                    ++pos;
                    count += 8;
                }
            }
            uint32_t Take(int n)
            {
                if (n == 0)
                    return 0;
                Need(n);
                auto v = uint32_t(buffer & ((uint64_t(1) << n) - 1));
                buffer >>= n;
                count -= n;
                return v;
            }
            void Drop(int n) { buffer >>= n; count -= n; }
            bool Overrun() const { return pos > size + 8; }
        };

        constexpr int kFast = 10;

        struct Huffman
        {
            uint16_t fast[1 << kFast];      // symbol << 4 | length, 0 for a longer code
            uint16_t count[16];             // codes of each length
            uint16_t symbol[320];           // symbols by code, canonical order

            bool Build(const uint8_t* lengths, int n)
            {
                std::memset(count, 0, sizeof(count));
                std::memset(fast, 0, sizeof(fast));
                for (int i = 0; i < n; ++i)
                    ++count[lengths[i]];
                count[0] = 0;
                int left = 1;
                for (int len = 1; len < 16; ++len)
                {
                    left = (left << 1) - count[len];
                    if (left < 0)
                        return false;
                }
                uint16_t offset[16]{};
                for (int len = 1; len < 15; ++len)
                    offset[len + 1] = uint16_t(offset[len] + count[len]);
                for (int i = 0; i < n; ++i)
                    if (lengths[i])
                        symbol[offset[lengths[i]]++] = uint16_t(i);
                // The short codes into the table, bit-reversed as they come in
                int code = 0, index = 0;
                for (int len = 1; len <= kFast; ++len)
                {
                    for (int k = 0; k < count[len]; ++k, ++code, ++index)
                    {
                        int reversed = 0;
                        for (int b = 0; b < len; ++b)
                            reversed |= ((code >> b) & 1) << (len - 1 - b);
                        for (int fill = reversed; fill < (1 << kFast); fill += 1 << len)
                            fast[fill] = uint16_t(symbol[index] << 4 | len);
                    }
                    code <<= 1;
                }
                return true;
            }

            int Decode(Bits& bits) const
            {
                bits.Need(16);
                auto entry = fast[bits.buffer & ((1 << kFast) - 1)];
                if (entry)
                {
                    bits.Drop(entry & 15);
                    return entry >> 4;
                }
                // Past the table: canonical decoding bit by bit
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; ++len)
                {
                    code |= int(bits.buffer & 1);
                    bits.Drop(1);
                    int n = count[len];
                    if (code - n < first)
                        return symbol[index + (code - first)];
                    index += n;
                    first += n;
                    first <<= 1;
                    code <<= 1;
                }
                return -1;
            }
        };

        constexpr uint16_t kLengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        constexpr uint8_t kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t kDistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        inline bool Codes(Bits& bits, const Huffman& lengths, const Huffman& distances, std::vector<uint8_t>& out, size_t limit)
        {
            while (true)
            {
                int symbol = lengths.Decode(bits);
                if (symbol < 0 || bits.Overrun())
                    return false;
                if (symbol < 256)
                {
                    if (out.size() >= limit)
                        return false;
                    out.push_back(uint8_t(symbol));
                    continue;
                }
                if (symbol == 256)
                    return true;
                symbol -= 257;
                if (symbol >= 29)
                    return false;
                size_t length = kLengthBase[symbol] + bits.Take(kLengthExtra[symbol]);
                int d = distances.Decode(bits);
                if (d < 0 || d >= 30)
                    return false;
                size_t distance = kDistanceBase[d] + bits.Take(kDistanceExtra[d]);
                if (distance > out.size() || out.size() + length > limit)
                    return false;
                size_t from = out.size() - distance;
                out.resize(out.size() + length);
                uint8_t* p = out.data();
                for (size_t i = 0; i < length; ++i)
                    p[out.size() - length + i] = p[from + i];
            }
        }
    }

    // Raw deflate data into `out` (appended)
    inline bool Raw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit)
    {
        using namespace detail;
        Bits bits{ data, size };
        static const Huffman fixedLengths = []()
        {
            uint8_t l[288];
            for (int i = 0; i < 288; ++i)
                l[i] = uint8_t(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            Huffman h{};
            h.Build(l, 288);
            return h;
        }();
        static const Huffman fixedDistances = []()
        {
            uint8_t l[30];
            std::memset(l, 5, sizeof(l));
            Huffman h{};
            h.Build(l, 30);
            return h;
        }();
        Huffman lengths{}, distances{};
        bool last = false;
        while (!last)
        {
            last = bits.Take(1) != 0;
            switch (bits.Take(2))
            {
            case 0:
            {
                // Stored: from the next byte, the length and its complement
                bits.Drop(bits.count & 7);
                uint32_t len = bits.Take(16), nlen = bits.Take(16);
                if ((len ^ 0xFFFF) != nlen)
                    return false;
                // Bytes still in the buffer first
                while (len && bits.count >= 8)
                {
                    if (out.size() >= limit)
                        return false;
                    out.push_back(uint8_t(bits.Take(8)));
                    --len;
                }
                if (bits.pos + len > size || out.size() + len > limit)
                    return false;
                out.insert(out.end(), data + bits.pos, data + bits.pos + len);
                bits.pos += len;
                break;
            }
            case 1:
                if (!Codes(bits, fixedLengths, fixedDistances, out, limit))
                    return false;
                break;
            case 2:
            {
                int nlen = int(bits.Take(5)) + 257, ndist = int(bits.Take(5)) + 1, ncode = int(bits.Take(4)) + 4;
                if (nlen > 286 || ndist > 30)
                    return false;
                static constexpr uint8_t kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t codeLengths[19]{};
                for (int i = 0; i < ncode; ++i)
                    codeLengths[kOrder[i]] = uint8_t(bits.Take(3));
                Huffman code{};
                if (!code.Build(codeLengths, 19))
                    return false;
                uint8_t l[320]{};
                for (int i = 0; i < nlen + ndist;)
                {
                    int symbol = code.Decode(bits);
                    if (symbol < 0)
                        return false;
                    if (symbol < 16)
                    {
                        l[i++] = uint8_t(symbol);
                        continue;
                    }
                    int repeat, value = 0;
                    if (symbol == 16)
                    {
                        if (i == 0)
                            return false;
                        value = l[i - 1];
                        repeat = 3 + int(bits.Take(2));
                    }
                    else if (symbol == 17)
                        repeat = 3 + int(bits.Take(3));
                    else
                        repeat = 11 + int(bits.Take(7));
                    if (i + repeat > nlen + ndist)
                        return false;
                    while (repeat--)
                        l[i++] = uint8_t(value);
                }
                if (!lengths.Build(l, nlen) || !distances.Build(l + nlen, ndist))
                    return false;
                if (!Codes(bits, lengths, distances, out, limit))
                    return false;
                break;
            }
            default:
                return false;
            }
            if (bits.Overrun())
                return false;
        }
        return true;
    }

    // A zlib stream (its two-byte header, deflate inside)
    inline bool Zlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit)
    {
        if (size < 2 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20))
            return false;
        return Raw(data + 2, size - 2, out, limit);
    }
}
