#include "Tools/Lz2kProbe/LzhDecoder.hpp"

#include <array>
#include <vector>

namespace gamecore::lz2k {
namespace {

constexpr unsigned kMaxMatch = 256;
constexpr unsigned kThreshold = 3;
constexpr unsigned kCharCodes = 256 + kMaxMatch - kThreshold + 1;  // 510
constexpr unsigned kCharBits = 9;
constexpr unsigned kTempCodes = 19;
constexpr unsigned kTempBits = 5;
constexpr unsigned kMaxCodeLength = 16;

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> in) : in_(in) {}

    std::uint32_t peek(unsigned count) const {
        if (count == 0) {
            return 0;
        }
        const std::uint64_t byte = bitPos_ >> 3;
        const unsigned shift = static_cast<unsigned>(bitPos_ & 7);
        std::uint64_t window = 0;
        for (std::uint64_t k = 0; k < 5; ++k) {
            const std::uint64_t at = byte + k;
            window = (window << 8) | (at < in_.size() ? in_[static_cast<std::size_t>(at)] : 0u);
        }
        return static_cast<std::uint32_t>((window >> (40 - shift - count)) & ((1ull << count) - 1));
    }

    void skip(unsigned count) { bitPos_ += count; }

    std::uint32_t get(unsigned count) {
        const auto value = peek(count);
        skip(count);
        return value;
    }

    std::uint64_t position() const { return bitPos_; }
    bool overrun() const { return bitPos_ > static_cast<std::uint64_t>(in_.size()) * 8; }

private:
    std::span<const std::uint8_t> in_;
    std::uint64_t bitPos_ = 0;
};

// Canonical Huffman code, MSB-first, codes assigned by (length, symbol) order as in ar002's make_table.
class Huffman {
public:
    bool build(const std::vector<std::uint8_t>& lengths, std::string& failure) {
        single_ = false;
        count_.fill(0);
        for (const auto length : lengths) {
            if (length > kMaxCodeLength) {
                failure = "code length above 16";
                return false;
            }
            ++count_[length];
        }
        count_[0] = 0;
        int left = 1;
        for (unsigned len = 1; len <= kMaxCodeLength; ++len) {
            left <<= 1;
            left -= count_[len];
            if (left < 0) {
                failure = "over-subscribed Huffman table";
                return false;
            }
        }
        if (left != 0) {
            failure = "incomplete Huffman table";
            return false;
        }
        std::array<std::uint16_t, kMaxCodeLength + 2> offsets{};
        for (unsigned len = 1; len <= kMaxCodeLength; ++len) {
            offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + count_[len]);
        }
        symbols_.assign(lengths.size(), 0);
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] != 0) {
                symbols_[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
            }
        }
        return true;
    }

    void buildSingle(std::uint16_t symbol) {
        single_ = true;
        singleSymbol_ = symbol;
    }

    int decode(BitReader& bits) const {
        if (single_) {
            return singleSymbol_;
        }
        int code = 0;
        int first = 0;
        int index = 0;
        for (unsigned len = 1; len <= kMaxCodeLength; ++len) {
            code |= static_cast<int>(bits.get(1));
            const int count = count_[len];
            if (code - first < count) {
                return symbols_[static_cast<std::size_t>(index + code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

private:
    std::array<std::uint16_t, kMaxCodeLength + 1> count_{};
    std::vector<std::uint16_t> symbols_;
    bool single_ = false;
    std::uint16_t singleSymbol_ = 0;
};

bool readTempLengths(BitReader& bits, unsigned codes, unsigned countBits, int special, Huffman& out,
                     std::string& failure) {
    const unsigned n = bits.get(countBits);
    if (n == 0) {
        const unsigned symbol = bits.get(countBits);
        if (symbol >= codes) {
            failure = "single-symbol table outside alphabet";
            return false;
        }
        out.buildSingle(static_cast<std::uint16_t>(symbol));
        return true;
    }
    if (n > codes) {
        failure = "length count above alphabet size";
        return false;
    }
    std::vector<std::uint8_t> lengths(codes, 0);
    unsigned i = 0;
    while (i < n) {
        unsigned length = bits.get(3);
        if (length == 7) {
            while (bits.get(1) == 1) {
                if (++length > kMaxCodeLength) {
                    failure = "unary length escape above 16";
                    return false;
                }
            }
        }
        lengths[i++] = static_cast<std::uint8_t>(length);
        if (static_cast<int>(i) == special) {
            unsigned zeros = bits.get(2);
            while (zeros-- > 0 && i < codes) {
                lengths[i++] = 0;
            }
        }
        if (bits.overrun()) {
            failure = "input ended inside a length table";
            return false;
        }
    }
    return out.build(lengths, failure);
}

bool readCharLengths(BitReader& bits, const Huffman& temp, Huffman& out, std::string& failure) {
    const unsigned n = bits.get(kCharBits);
    if (n == 0) {
        const unsigned symbol = bits.get(kCharBits);
        if (symbol >= kCharCodes) {
            failure = "single-symbol char table outside alphabet";
            return false;
        }
        out.buildSingle(static_cast<std::uint16_t>(symbol));
        return true;
    }
    if (n > kCharCodes) {
        failure = "char length count above alphabet size";
        return false;
    }
    std::vector<std::uint8_t> lengths(kCharCodes, 0);
    unsigned i = 0;
    while (i < n) {
        const int symbol = temp.decode(bits);
        if (symbol < 0) {
            failure = "undecodable symbol in char length table";
            return false;
        }
        if (symbol <= 2) {
            unsigned run = symbol == 0 ? 1u : symbol == 1 ? bits.get(4) + 3u : bits.get(kCharBits) + 20u;
            if (i + run > kCharCodes) {
                failure = "zero run past char alphabet";
                return false;
            }
            while (run-- > 0) {
                lengths[i++] = 0;
            }
        } else {
            lengths[i++] = static_cast<std::uint8_t>(symbol - 2);
        }
        if (bits.overrun()) {
            failure = "input ended inside the char length table";
            return false;
        }
    }
    return out.build(lengths, failure);
}

}  // namespace

DecodeResult decodeLzh(std::span<const std::uint8_t> in, std::span<std::uint8_t> out, const LzhParams& params) {
    DecodeResult result;
    BitReader bits(in);
    Huffman temp;
    Huffman chars;
    Huffman positions;
    std::uint32_t remaining = 0;
    std::size_t pos = 0;

    while (pos < out.size()) {
        if (remaining == 0) {
            remaining = bits.get(16);
            if (remaining == 0) {
                result.failure = "zero block size";
                break;
            }
            ++result.blocks;
            if (!readTempLengths(bits, kTempCodes, kTempBits, 3, temp, result.failure) ||
                !readCharLengths(bits, temp, chars, result.failure) ||
                !readTempLengths(bits, params.posCodes(), params.posBits, -1, positions, result.failure)) {
                break;
            }
        }
        --remaining;
        const int symbol = chars.decode(bits);
        if (symbol < 0) {
            result.failure = "undecodable char symbol";
            break;
        }
        if (symbol < 256) {
            out[pos++] = static_cast<std::uint8_t>(symbol);
        } else {
            const std::size_t length = static_cast<std::size_t>(symbol) - 256 + kThreshold;
            const int slot = positions.decode(bits);
            if (slot < 0) {
                result.failure = "undecodable position symbol";
                break;
            }
            std::size_t distance = 1;
            if (slot != 0) {
                distance += (std::size_t{1} << (slot - 1)) + bits.get(static_cast<unsigned>(slot - 1));
            }
            if (distance > pos) {
                result.failure = "match reaches before the chunk start";
                break;
            }
            if (length > out.size() - pos) {
                result.failure = "match runs past the declared raw size";
                break;
            }
            for (std::size_t k = 0; k < length; ++k, ++pos) {
                out[pos] = out[pos - distance];
            }
        }
        if (bits.overrun()) {
            result.failure = "input exhausted before the declared raw size";
            break;
        }
    }

    result.produced = pos;
    result.bitsConsumed = bits.position();
    result.codesLeftInBlock = remaining;
    result.ok = result.failure.empty() && pos == out.size() && !bits.overrun();
    return result;
}

}  // namespace gamecore::lz2k
