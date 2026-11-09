#include "zip.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace resorep {
namespace {
constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr uint16_t kFlagUtf8 = 0x0800, kFlagEncrypted = 0x0001;

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    Put16(v, uint16_t(x));
    Put16(v, uint16_t(x >> 16));
}

// --- Inflate (canonical Huffman decoding, after zlib's "puff" reference decoder) -------

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];
};

struct Inflater {
    const uint8_t* in;
    size_t inSize, inPos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    std::vector<uint8_t>& out;
    size_t outPos = 0;
    bool failed = false;

    Inflater(const uint8_t* d, size_t n, std::vector<uint8_t>& o) : in(d), inSize(n), out(o) {}

    uint32_t Bits(int need) {
        uint32_t v = bitBuf;
        while (bitCount < need) {
            if (inPos >= inSize) {
                failed = true;
                return 0;
            }
            v |= uint32_t(in[inPos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = v >> need;
        bitCount -= need;
        return v & ((1u << need) - 1);
    }

    // Builds a decoding table from code lengths; returns false for an over-subscribed set.
    static bool Build(Huffman& h, const uint8_t* lengths, int n) {
        std::memset(h.count, 0, sizeof(h.count));
        for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
        if (h.count[0] == n) return true;  // no codes: only valid if never used
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) h.symbol[offs[lengths[i]]++] = uint16_t(i);
        return true;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= int(Bits(1));
            if (failed) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool Put(uint8_t byte) {
        if (outPos >= out.size()) return false;
        out[outPos++] = byte;
        return true;
    }

    bool Stored() {
        bitBuf = 0;
        bitCount = 0;
        if (inPos + 4 > inSize) return false;
        const uint16_t len = Get16(in + inPos), nlen = Get16(in + inPos + 2);
        inPos += 4;
        if (uint16_t(~nlen) != len || inPos + len > inSize || outPos + len > out.size()) return false;
        std::memcpy(out.data() + outPos, in + inPos, len);
        inPos += len;
        outPos += len;
        return true;
    }

    bool Codes(const Huffman& lencode, const Huffman& distcode) {
        static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                               33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                               1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lencode);
            if (sym < 0) return false;
            if (sym < 256) {
                if (!Put(uint8_t(sym))) return false;
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const size_t len = kLenBase[sym] + Bits(kLenExtra[sym]);
                const int ds = Decode(distcode);
                if (ds < 0 || ds >= 30) return false;
                const size_t dist = kDistBase[ds] + Bits(kDistExtra[ds]);
                if (failed || dist > outPos || outPos + len > out.size()) return false;
                for (size_t i = 0; i < len; ++i, ++outPos) out[outPos] = out[outPos - dist];
            }
        }
    }

    bool Fixed() {
        static Huffman lencode, distcode;
        static const bool built = [] {
            uint8_t lengths[288];
            int i = 0;
            for (; i < 144; ++i) lengths[i] = 8;
            for (; i < 256; ++i) lengths[i] = 9;
            for (; i < 280; ++i) lengths[i] = 7;
            for (; i < 288; ++i) lengths[i] = 8;
            Build(lencode, lengths, 288);
            for (i = 0; i < 30; ++i) lengths[i] = 5;
            Build(distcode, lengths, 30);
            return true;
        }();
        (void)built;
        return Codes(lencode, distcode);
    }

    bool Dynamic() {
        static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = int(Bits(5)) + 257, ndist = int(Bits(5)) + 1, ncode = int(Bits(4)) + 4;
        if (failed || nlen > 286 || ndist > 30) return false;
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = uint8_t(Bits(3));
        Huffman lencode, distcode;
        if (!Build(lencode, lengths, 19)) return false;

        int index = 0;
        while (index < nlen + ndist) {
            int sym = Decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[index++] = uint8_t(sym);
                continue;
            }
            uint8_t len = 0;
            int repeat;
            if (sym == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + int(Bits(2));
            } else if (sym == 17) {
                repeat = 3 + int(Bits(3));
            } else {
                repeat = 11 + int(Bits(7));
            }
            if (failed || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = len;
        }
        if (lengths[256] == 0) return false;  // no end-of-block code
        if (!Build(lencode, lengths, nlen) || !Build(distcode, lengths + nlen, ndist)) return false;
        return Codes(lencode, distcode);
    }

    bool Run() {
        for (;;) {
            const uint32_t last = Bits(1), type = Bits(2);
            if (failed) return false;
            const bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok || failed) return false;
            if (last) return outPos == out.size();
        }
    }
};

uint32_t g_crcTable[256];
const bool g_crcReady = [] {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[i] = c;
    }
    return true;
}();
}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = g_crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    Inflater inf(data, size, out);
    return inf.Run();
}

// --- Writer -------------------------------------------------------------------------

bool ZipWriter::Open(const std::filesystem::path& path) {
    if (out_.is_open()) out_.close();
    entries_.clear();
    error_.clear();
    offset_ = 0;
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        error_ = "cannot create the file";
        return false;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    dosTime_ = uint16_t((t.wHour << 11) | (t.wMinute << 5) | (t.wSecond / 2));
    dosDate_ = uint16_t(((t.wYear - 1980) << 9) | (t.wMonth << 5) | t.wDay);
    return true;
}

bool ZipWriter::Add(const std::string& name, const uint8_t* data, size_t size) {
    if (!out_ || !error_.empty()) return false;
    if (name.size() > UINT16_MAX) {
        error_ = "entry name too long";
        return false;
    }
    if (offset_ + size + name.size() + 64 > 0xFFFFFFF0ull || entries_.size() >= 0xFFFF) {
        error_ = "the package would be larger than 4 GB";
        return false;
    }
    const uint32_t crc = Crc32(data, size);
    std::vector<uint8_t> h;
    Put32(h, kLocalSig);
    Put16(h, 20);
    Put16(h, kFlagUtf8);
    Put16(h, 0);  // stored
    Put16(h, dosTime_);
    Put16(h, dosDate_);
    Put32(h, crc);
    Put32(h, uint32_t(size));
    Put32(h, uint32_t(size));
    Put16(h, uint16_t(name.size()));
    Put16(h, 0);
    h.insert(h.end(), name.begin(), name.end());
    out_.write(reinterpret_cast<const char*>(h.data()), std::streamsize(h.size()));
    if (size) out_.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    entries_.push_back({name, crc, uint32_t(size), uint32_t(offset_)});
    offset_ += h.size() + size;
    if (!out_) error_ = "write failed (disk full?)";
    return bool(out_);
}

bool ZipWriter::AddFile(const std::string& name, const uint8_t* data, size_t size) { return Add(name, data, size); }

bool ZipWriter::AddDirectory(const std::string& name) {
    return Add(name.empty() || name.back() == '/' ? name : name + "/", nullptr, 0);
}

bool ZipWriter::Finish() {
    if (!out_ || !error_.empty()) return false;
    std::vector<uint8_t> cd;
    for (const Central& e : entries_) {
        Put32(cd, kCentralSig);
        Put16(cd, 20);
        Put16(cd, 20);
        Put16(cd, kFlagUtf8);
        Put16(cd, 0);
        Put16(cd, dosTime_);
        Put16(cd, dosDate_);
        Put32(cd, e.crc);
        Put32(cd, e.size);
        Put32(cd, e.size);
        Put16(cd, uint16_t(e.name.size()));
        Put16(cd, 0);  // extra
        Put16(cd, 0);  // comment
        Put16(cd, 0);  // disk
        Put16(cd, 0);  // internal attributes
        Put32(cd, (!e.name.empty() && e.name.back() == '/') ? 0x10 : 0);  // MS-DOS directory bit
        Put32(cd, e.offset);
        cd.insert(cd.end(), e.name.begin(), e.name.end());
    }
    const uint32_t cdSize = uint32_t(cd.size());  // before the end record is appended
    Put32(cd, kEndSig);
    Put16(cd, 0);
    Put16(cd, 0);
    Put16(cd, uint16_t(entries_.size()));
    Put16(cd, uint16_t(entries_.size()));
    Put32(cd, cdSize);
    Put32(cd, uint32_t(offset_));
    Put16(cd, 0);
    out_.write(reinterpret_cast<const char*>(cd.data()), std::streamsize(cd.size()));
    out_.close();
    if (out_.fail()) error_ = "write failed (disk full?)";
    return !out_.fail();
}

// --- Reader -------------------------------------------------------------------------

bool ZipReader::Open(const std::filesystem::path& path) {
    if (in_.is_open()) in_.close();
    entries_.clear();
    error_.clear();
    in_.open(path, std::ios::binary | std::ios::ate);
    if (!in_) {
        error_ = "cannot open the file";
        return false;
    }
    fileSize_ = uint64_t(in_.tellg());
    // The end record is in the last 22 + 65535 (max comment) bytes.
    const uint64_t tail = std::min<uint64_t>(fileSize_, 22 + 0xFFFF);
    std::vector<uint8_t> buf(static_cast<size_t>(tail));
    in_.seekg(std::streamoff(fileSize_ - tail));
    in_.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    if (!in_ || tail < 22) {
        error_ = "not a zip file";
        return false;
    }
    // Prefer the record whose comment ends the file: the signature can also appear inside a
    // comment. Failing that, take the last record whose comment fits, for zips with bytes
    // appended after them.
    size_t end = std::string::npos, loose = std::string::npos;
    for (size_t i = buf.size() - 22 + 1; i-- > 0;) {
        if (Get32(&buf[i]) != kEndSig) continue;
        const size_t recordEnd = i + 22 + Get16(&buf[i + 20]);
        if (recordEnd == buf.size()) {
            end = i;
            break;
        }
        if (recordEnd < buf.size() && loose == std::string::npos) loose = i;
    }
    if (end == std::string::npos) end = loose;
    if (end == std::string::npos) {
        error_ = "not a zip file";
        return false;
    }
    const uint16_t count = Get16(&buf[end + 10]);
    const uint32_t cdSize = Get32(&buf[end + 12]), cdOffset = Get32(&buf[end + 16]);
    if (cdOffset == 0xFFFFFFFF || uint64_t(cdOffset) + cdSize > fileSize_ - tail + end) {
        error_ = "unsupported zip (ZIP64 or damaged)";
        return false;
    }
    std::vector<uint8_t> cd(cdSize);
    in_.seekg(cdOffset);
    if (!in_.read(reinterpret_cast<char*>(cd.data()), cdSize)) {
        error_ = "could not read zip directory";
        return false;
    }
    size_t p = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || Get32(&cd[p]) != kCentralSig) {
            error_ = "damaged zip directory";
            return false;
        }
        Entry e;
        e.flags = Get16(&cd[p + 8]);
        e.method = Get16(&cd[p + 10]);
        e.crc = Get32(&cd[p + 16]);
        e.compressedSize = Get32(&cd[p + 20]);
        e.size = Get32(&cd[p + 24]);
        const uint16_t nameLen = Get16(&cd[p + 28]), extraLen = Get16(&cd[p + 30]), commentLen = Get16(&cd[p + 32]);
        e.localOffset = Get32(&cd[p + 42]);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) {
            error_ = "damaged zip directory";
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(cd.data() + p + 46), nameLen);
        p += 46 + size_t(nameLen) + extraLen + commentLen;
        entries_.push_back(std::move(e));
    }
    return true;
}

bool ZipReader::Extract(const Entry& e, std::vector<uint8_t>& out, uint32_t maxSize) {
    if (e.flags & kFlagEncrypted) {
        error_ = "password-protected zip entries are not supported";
        return false;
    }
    if (e.method != 0 && e.method != 8) {
        error_ = "unsupported compression method";
        return false;
    }
    if (e.size > maxSize || uint64_t(e.localOffset) + 30 > fileSize_) {
        error_ = "entry too large or damaged";
        return false;
    }
    uint8_t local[30];
    in_.clear();
    in_.seekg(e.localOffset);
    in_.read(reinterpret_cast<char*>(local), 30);
    if (!in_ || Get32(local) != kLocalSig) {
        error_ = "damaged zip entry";
        return false;
    }
    const uint64_t dataOffset = uint64_t(e.localOffset) + 30 + Get16(local + 26) + Get16(local + 28);
    if (dataOffset + e.compressedSize > fileSize_) {
        error_ = "damaged zip entry";
        return false;
    }
    if (e.method == 0 && e.compressedSize != e.size) {
        error_ = "damaged zip entry";
        return false;
    }
    std::vector<uint8_t> packed(e.compressedSize);
    in_.seekg(std::streamoff(dataOffset));
    in_.read(reinterpret_cast<char*>(packed.data()), std::streamsize(packed.size()));
    if (!in_) {
        error_ = "read failed";
        return false;
    }
    if (e.method == 0) {
        out = std::move(packed);
    } else {
        out.assign(e.size, 0);
        if (!Inflate(packed.data(), packed.size(), out)) {
            error_ = "corrupt compressed data";
            return false;
        }
    }
    if (Crc32(out.data(), out.size()) != e.crc) {
        error_ = "checksum mismatch (damaged file)";
        return false;
    }
    return true;
}

}  // namespace resorep
