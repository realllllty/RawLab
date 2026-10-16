// Nikon High Efficiency (HE/HE*) decoder checks.
//
// Without arguments every check uses synthetic data or the 155-byte main
// header of a CC0 raw.pixls.us Z8 HE file, so no photograph is bundled. The
// synthetic codestreams come from a small encoder below and are compared with
// an independent whole-frame reference (dequantisation, 5/3 wavelet, ISO
// star-tetrix and transfer curve), not with the decoder's own helpers.
//
// `--decode FILE` unpacks a real NEF through the patched LibRaw and prints the
// facts run_nikon_he_samples.py compares with nikon-he-raw-samples.json.
#include "nikon_he_decoder.h"
#include <libraw/libraw.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace rawlab_nikon_he;

namespace {

int failures = 0;
void check(bool value, const std::string& name) {
    std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
    if (!value) ++failures;
}

// Main header and first slice header of raw.pixls.us
// Nikon/Z 8/Nikon_Z8_high_efficiency_low.NEF (CC0), strip offset 7245312.
const uint8_t kZ8Header[155] = {
    0xff, 0x10, 0xff, 0x50, 0x00, 0x22, 0x43, 0x4f, 0x4e, 0x54, 0x41, 0x43, 0x54, 0x5f, 0x49, 0x4e,
    0x54, 0x4f, 0x50, 0x49, 0x58, 0x5f, 0xef, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x12, 0x00, 0x27, 0x01, 0x05, 0x86, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20, 0x58, 0x15, 0x90, 0x00, 0x00, 0x00, 0x10, 0x04, 0x04, 0x08, 0x12, 0x44, 0x10,
    0x51, 0x14, 0x50, 0x88, 0x70, 0x83, 0xf0, 0x15, 0x23, 0xd1, 0x49, 0xcd, 0x3f, 0x7f, 0x07, 0xff,
    0x13, 0x00, 0x0e, 0x0e, 0x11, 0x51, 0x0e, 0x11, 0x51, 0x0e, 0x11, 0x00, 0x0e, 0x11, 0x51, 0xff,
    0x14, 0x00, 0x34, 0x03, 0x02, 0x03, 0x13, 0x02, 0x09, 0x02, 0x16, 0x01, 0x0a, 0x01, 0x15, 0x02,
    0x05, 0x01, 0x03, 0x01, 0x11, 0x00, 0x01, 0x00, 0x07, 0x00, 0x0d, 0x00, 0x0b, 0x02, 0x06, 0x01,
    0x04, 0x01, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x0e, 0x01, 0x14, 0x00, 0x12, 0x00, 0x0f, 0x00,
    0x17, 0x00, 0x0c, 0x00, 0x18, 0xff, 0x20, 0x00, 0x04, 0x00, 0x00};
const size_t kPihLcod = 42;      // offset of Lcod inside kZ8Header
const size_t kPihFlags = 63;     // Fslc/Ppoc/Cpih byte
const size_t kPihTail = 65;      // Lh/Rl/Qpih/Fs/Rm byte
const size_t kPihExtension = 66; // 13-byte vendor extension
const size_t kCdtBody = 83;
const size_t kWgtMarker = 95;

std::vector<uint8_t> z8Header() {
    std::vector<uint8_t> h(kZ8Header, kZ8Header + sizeof(kZ8Header));
    // Pretend the codestream ends here so the main header parses on its own.
    h[kPihLcod] = 0;
    h[kPihLcod + 1] = 0;
    h[kPihLcod + 2] = 0;
    h[kPihLcod + 3] = uint8_t(h.size());
    return h;
}

Status parseStatus(const std::vector<uint8_t>& bytes) {
    Header h;
    return parse_header(bytes.data(), bytes.size(), h).status;
}

void testHeader() {
    std::vector<uint8_t> bytes = z8Header();
    Header h;
    const Error e = parse_header(bytes.data(), bytes.size(), h);
    check(e.status == kOk, "real Z8 HE main header parses");
    check(h.grid_width == 8280 && h.grid_height == 5520 && h.plane_width() == 4140 &&
              h.plane_height() == 2760,
          "CFA grid and component planes");
    check(h.slice_height == 16 && h.precinct_count() == 1380 && h.slice_count() == 87 &&
              h.first_slice == 149,
          "precinct and slice geometry");
    check(h.black == 1008 && h.gain == 84429 && h.knee == 16255,
          "transfer parameters from the PIH extension");
    bool components = h.component_count == 4;
    for (int c = 0; c < 4; ++c)
        components = components && h.components[c].bit_depth == 14 &&
                     h.components[c].suppressed() == (c == 2) &&
                     h.components[c].bands() == (c == 2 ? 1 : 8);
    check(components && h.weight_count == 25, "component table and 25 weighted bands");

    Topology t;
    check(build_topology(h, t).status == kOk, "topology builds");
    const int widths[8] = {130, 129, 259, 517, 1035, 2070, 2070, 2070};
    bool bandWidths = t.bands.size() == 25 && t.bands[16].width == 4140 && t.bands[16].lines == 2;
    for (int c : {0, 1, 3})
        for (int b = 0; b < 8; ++b)
            bandWidths = bandWidths && t.bands[t.component_first_band[c] + b].width == widths[b] &&
                         t.bands[t.component_first_band[c] + b].lines == 1;
    check(bandWidths, "band widths follow the 5-level split of 4140");
    const int packets[8][4] = {{0, 0, 6, 0}, {1, 8, 14, 0},  {2, 16, 17, 0}, {3, 17, 23, 0},
                               {0, 6, 8, 1}, {1, 14, 16, 1}, {2, 16, 17, 1}, {3, 23, 25, 1}};
    bool packetLayout = t.packets.size() == 8;
    for (int i = 0; packetLayout && i < 8; ++i)
        packetLayout = t.packets[i].component == packets[i][0] &&
                       t.packets[i].first_band == packets[i][1] &&
                       t.packets[i].end_band == packets[i][2] && t.packets[i].line == packets[i][3];
    check(packetLayout, "packets are subband-major");
    // WGT entries follow packet emission order, not band order.
    const int bandOfEntry[25] = {0,  1,  2,  3,  4,  5,  8, 9, 10, 11, 12, 13, 16,
                                 17, 18, 19, 20, 21, 22, 6, 7, 14, 15, 23, 24};
    bool weights = true;
    for (int i = 0; i < 25; ++i)
        weights = weights && t.bands[bandOfEntry[i]].gain == h.gains[i] &&
                  t.bands[bandOfEntry[i]].priority == h.priorities[i];
    check(weights, "weights map onto bands in packet order");
    // Truncation for Q = 3, R = 11 (dnglab's reference dump uses the same table).
    const int gtli[25] = {0, 0, 0, 1, 1, 2, 2, 3, 0, 1, 2, 2, 2,
                          3, 3, 3, 3, 0, 1, 2, 2, 2, 3, 3, 3};
    bool truncation = true;
    for (int b = 0; b < 25; ++b) truncation = truncation && t.bands[b].gtli(3, 11) == gtli[b];
    check(truncation, "GTLI = Q - gain - (priority < R), clamped");

    std::vector<uint8_t> real(kZ8Header, kZ8Header + sizeof(kZ8Header));
    check(parseStatus(real) == kInvalidData, "Lcod beyond the strip is rejected");
    std::vector<uint8_t> v = z8Header();
    v[10] ^= 0x20;
    check(parseStatus(v) == kUnsupported, "non-TicoRAW capability marker is unsupported");
    v = z8Header();
    v[kPihFlags] = 0x13;
    check(parseStatus(v) == kUnsupported, "explicit Cpih colour transform is unsupported");
    v = z8Header();
    v[kPihTail] = 0x04;
    check(parseStatus(v) == kUnsupported, "deadzone quantiser is unsupported");
    v = z8Header();
    v[kPihTail] = 0x10;
    check(parseStatus(v) == kUnsupported, "inline signs (Fs = 0) are unsupported");
    v = z8Header();
    v[kCdtBody + 8] = 0x51;
    check(parseStatus(v) == kUnsupported, "a wavelet on component 2 is unsupported");
    v = z8Header();
    v[kPihExtension + 9] = 0;
    v[kPihExtension + 8] = 0;
    v[kPihExtension + 7] &= ~3;
    check(parseStatus(v) == kUnsupported, "zero transfer gain is unsupported");
    v = z8Header();
    const uint8_t nlt[] = {0xff, 0x16, 0x00, 0x04, 0x01, 0x00};
    v.insert(v.begin() + kWgtMarker, nlt, nlt + sizeof(nlt));
    v[kPihLcod + 3] = uint8_t(v.size());
    check(parseStatus(v) == kUnsupported, "an NLT marker is unsupported");
    v = z8Header();
    const uint8_t com[] = {0xff, 0x15, 0x00, 0x05, 0x00, 0x00, 0x41};
    v.insert(v.begin() + kWgtMarker, com, com + sizeof(com));
    v[kPihLcod + 3] = uint8_t(v.size());
    check(parseStatus(v) == kOk, "COM segments are skipped");
    v = z8Header();
    v.resize(60);
    check(parseStatus(v) == kInvalidData, "truncated main header is invalid");
}

// Independent statement of the transfer rule published with LibRaw PR #826.
void testTransfer() {
    Header h;
    std::memset(&h, 0, sizeof(h));
    h.black = 1008;
    h.gain = 84429;
    h.knee = 16255;
    const int cases[][2] = {{-16513, 1008}, {0, 2734},     {1000, 2950},   {-1000, 2532},
                            {-16000, 1010}, {20000, 9449}, {32765, 16382}, {32767, 16383},
                            {-32768, 0},    {-40000, 0},   {40000, 16383}};
    bool values = true;
    for (const auto& c : cases) values = values && transfer(c[0], h) == c[1];
    check(values, "transfer curve matches independently computed values");
    h.gain = 65905;
    check(transfer(49023 - 32768, h) == 5151, "transfer gain rounds half up before squaring");
}

// ---------------------------------------------------------------------------
// Reference implementations, written independently of the decoder.

int mirror(int i, int n) { return i < 0 ? -i : (i >= n ? 2 * (n - 1) - i : i); }

// Forward 5/3 lifting with symmetric extension (test only).
void forward53(const std::vector<int32_t>& x, std::vector<int32_t>& lf, std::vector<int32_t>& hf) {
    const int n = int(x.size());
    hf.assign(n / 2, 0);
    lf.assign(n - n / 2, 0);
    for (int k = 0; k < n / 2; ++k)
        hf[k] = x[2 * k + 1] - ((x[2 * k] + x[mirror(2 * k + 2, n)]) >> 1);
    auto h = [&](int k) { return hf[std::min(std::max(k, 0), n / 2 - 1)]; };
    for (int k = 0; k < n - n / 2; ++k) lf[k] = x[2 * k] + ((h(k - 1) + h(k) + 2) >> 2);
}

// Inverse 5/3 lifting stated directly from symmetric extension.
std::vector<int32_t> inverse53(const std::vector<int32_t>& lf, const std::vector<int32_t>& hf) {
    const int n = int(lf.size() + hf.size());
    std::vector<int32_t> out(n);
    auto h = [&](int k) { return hf[std::min(std::max(k, 0), int(hf.size()) - 1)]; };
    for (int k = 0; 2 * k < n; ++k) out[2 * k] = lf[k] - ((h(k - 1) + h(k) + 2) >> 2);
    for (int k = 0; 2 * k + 1 < n; ++k)
        out[2 * k + 1] = hf[k] + ((out[2 * k] + out[mirror(2 * k + 2, n)]) >> 1);
    return out;
}

void testWaveletLifting() {
    std::mt19937 rng(7);
    bool ok = true;
    for (int n = 2; n <= 70; ++n) {
        std::vector<int32_t> x(n), lf, hf, out(n);
        for (auto& v : x) v = int32_t(rng() % 60001) - 30000;
        forward53(x, lf, hf);
        InverseWavelet<int>::idwt_horizontal(lf.data(), int(lf.size()), hf.data(), int(hf.size()),
                                             out.data());
        ok = ok && out == x && inverse53(lf, hf) == x;
    }
    check(ok, "inverse 5/3 lifting reverses every length and parity");
}

// Uniform inverse quantisation in the multiplicative form of LibRaw PR #826.
int32_t dequantise(uint16_t coded, int gcli, int gtli, int fq) {
    static const uint32_t scale[15] = {87381, 74898, 69905, 67650, 66576, 66052, 65793, 65664,
                                       65600, 65568, 65552, 65544, 65540, 65538, 65537};
    uint32_t m = coded & 0x7fffu;
    if (gtli > 0 && gcli > gtli && m)
        m = uint32_t((uint64_t(m >> gtli) * scale[gcli - gtli - 1]) >> (16 - gtli));
    const int32_t s = (coded & 0x8000u) ? -int32_t(m) : int32_t(m);
    return s * (1 << fq);
}

// One band line as the decoder stores it, plus the coded values it came from.
struct BandLine {
    std::vector<uint8_t> gcli;   // per group
    std::vector<uint16_t> coded; // 4 per group: value >> gtli, sign in bit 15
};

struct SyntheticPrecinct {
    int q = 0, r = 0;
    std::vector<uint8_t> modes;               // per band
    std::vector<std::vector<BandLine>> lines; // [band][line]
    std::vector<bool> rawPacket;              // per packet
};

struct Frame {
    Header header;
    Topology topology;
    std::vector<SyntheticPrecinct> precincts;
};

Header syntheticHeader(int planeWidth, int planeHeight, int nlx, int sliceHeight) {
    Header h;
    std::memset(&h, 0, sizeof(h));
    h.grid_width = 2 * planeWidth;
    h.grid_height = 2 * planeHeight;
    h.slice_height = sliceHeight;
    h.group_size = 4;
    h.sig_group_size = 8;
    h.bw = 18;
    h.fq = 4;
    h.br = 4;
    h.ppoc = 1;
    h.nlx = nlx;
    h.nly = 1;
    h.qpih = 1;
    h.fs = 1;
    h.black = 1008;
    h.gain = 84429;
    h.knee = 16255;
    h.component_count = 4;
    for (int c = 0; c < 4; ++c) {
        h.components[c].bit_depth = 14;
        h.components[c].sx = h.components[c].sy = 1;
        h.components[c].nlx = c == 2 ? 0 : nlx;
        h.components[c].nly = c == 2 ? 0 : 1;
    }
    h.weight_count = h.total_bands();
    for (int b = 0; b < h.weight_count; ++b) {
        h.gains[b] = uint8_t(b % 3);
        h.priorities[b] = uint8_t((b * 7) % h.weight_count);
    }
    return h;
}

// Random precinct content. GCLIs drift around the precinct above so vertical
// prediction sees small deltas, and some groups repeat their prediction so
// significance flags skip them.
SyntheticPrecinct randomPrecinct(const Frame& f, const SyntheticPrecinct* top, std::mt19937& rng) {
    const Topology& t = f.topology;
    SyntheticPrecinct p;
    p.q = int(rng() % 5);
    p.r = int(rng() % (t.bands.size() + 1));
    p.modes.resize(t.bands.size());
    for (auto& m : p.modes) m = uint8_t(rng() % 4);
    p.rawPacket.resize(t.packets.size());
    for (size_t i = 0; i < p.rawPacket.size(); ++i) p.rawPacket[i] = rng() % 5 == 0;
    p.lines.resize(t.bands.size());
    for (size_t b = 0; b < t.bands.size(); ++b) {
        const Band& band = t.bands[b];
        const int gtli = band.gtli(p.q, p.r);
        p.lines[b].resize(band.lines);
        for (int l = 0; l < band.lines; ++l) {
            BandLine& line = p.lines[b][l];
            line.gcli.resize(band.gcli_width);
            line.coded.assign(4 * band.gcli_width, 0);
            for (int g = 0; g < band.gcli_width; ++g) {
                int base = gtli + int(rng() % 6);
                if (top && rng() % 3 == 0) base = top->lines[b].back().gcli[g] + int(rng() % 3) - 1;
                const int gcli = std::min(std::max(base, gtli), 15);
                line.gcli[g] = uint8_t(gcli);
                for (int j = 0; j < 4; ++j) {
                    if (gcli <= gtli || rng() % 7 == 0) continue;
                    const uint32_t value = uint32_t(rng() % (1u << (gcli - gtli)));
                    line.coded[4 * g + j] =
                        uint16_t((value << gtli) | (value && rng() % 2 ? 0x8000u : 0u));
                }
            }
        }
    }
    return p;
}

class BitWriter {
public:
    void bit(unsigned v) {
        if (bits_ % 8 == 0) bytes.push_back(0);
        if (v) bytes.back() |= uint8_t(0x80u >> (bits_ % 8));
        ++bits_;
    }
    void bits(uint32_t v, int count) {
        for (int i = count - 1; i >= 0; --i) bit((v >> i) & 1u);
    }
    void unary(unsigned x) {
        for (unsigned i = 0; i < x; ++i) bit(1);
        bit(0);
    }
    void align() { bits_ = (bits_ + 7) & ~size_t(7); }
    std::vector<uint8_t> bytes;

private:
    size_t bits_ = 0;
};

// How often the synthetic streams reach each coding path.
struct Coverage {
    int skippedGroups = 0, rawPackets = 0, verticalCodes = 0, paddedPackets = 0, extraSigns = 0;
} coverage;

unsigned encodeVertical(int gcli, int mTop, int gtli) {
    const int threshold = std::max(mTop - gtli, 0);
    const int delta = gcli - mTop;
    if (delta > threshold) return unsigned(delta + threshold);
    return delta >= 0 ? unsigned(2 * delta) : unsigned(-2 * delta - 1);
}

// Mirror of the decoder's packet syntax: long packet header, significance,
// GCLI, data and sign sub-packets. Some data sub-packets carry padding.
void encodePrecinct(const Frame& f, const SyntheticPrecinct& p, const SyntheticPrecinct* top,
                    std::vector<uint8_t>& out, std::mt19937& rng) {
    const Topology& t = f.topology;
    std::vector<uint8_t> payload;
    for (size_t pi = 0; pi < t.packets.size(); ++pi) {
        const Packet& packet = t.packets[pi];
        const bool raw = p.rawPacket[pi];
        BitWriter sig, gcli, data, sign;
        for (int b = packet.first_band; b < packet.end_band; ++b) {
            const Band& band = t.bands[b];
            const int line = band.lines > 1 ? packet.line : 0;
            const BandLine& bl = p.lines[b][line];
            const int gtli = band.gtli(p.q, p.r);
            // Prediction reference, exactly as the format defines it.
            const uint8_t* ref = nullptr;
            int refGtli = 0;
            if (p.modes[b] & 1) {
                if (line > 0) {
                    ref = p.lines[b][line - 1].gcli.data();
                    refGtli = gtli;
                } else if (top) {
                    ref = top->lines[b].back().gcli.data();
                    refGtli = t.bands[b].gtli(top->q, top->r);
                }
            }
            auto predicted = [&](int g) {
                return ref ? std::max(int(ref[g]), std::max(refGtli, gtli)) : gtli;
            };
            std::vector<bool> skip((band.gcli_width + 7) / 8, false);
            if (!raw && (p.modes[b] & 2)) {
                for (int s = 0; s < int(skip.size()); ++s) {
                    bool same = true;
                    for (int g = 8 * s; g < std::min(8 * s + 8, band.gcli_width); ++g)
                        same = same && bl.gcli[g] == predicted(g);
                    skip[s] = same;
                    coverage.skippedGroups += same;
                    sig.bit(same ? 1 : 0);
                }
            }
            for (int g = 0; g < band.gcli_width; ++g) {
                if (raw)
                    gcli.bits(bl.gcli[g], 4);
                else if (!skip[g / 8]) {
                    coverage.verticalCodes += ref != nullptr;
                    gcli.unary(ref ? encodeVertical(bl.gcli[g], predicted(g), gtli)
                                   : unsigned(bl.gcli[g] - gtli));
                }
                for (int plane = bl.gcli[g] - gtli - 1; plane >= 0; --plane) {
                    unsigned nibble = 0;
                    for (int j = 0; j < 4; ++j)
                        nibble = (nibble << 1) |
                                 (((bl.coded[4 * g + j] & 0x7fffu) >> (gtli + plane)) & 1u);
                    data.bits(nibble, 4);
                }
            }
            for (size_t i = 0; i < bl.coded.size(); ++i) {
                if (!(bl.coded[i] & 0x7fffu)) continue;
                sign.bit(bl.coded[i] >> 15);
                coverage.extraSigns += int(i) >= band.width;
            }
        }
        sig.align();
        gcli.align();
        data.align();
        sign.align();
        coverage.rawPackets += raw;
        if (rng() % 4 == 0) {
            data.bytes.push_back(0); // declared padding
            ++coverage.paddedPackets;
        }
        const uint32_t dl = uint32_t(data.bytes.size()), gl = uint32_t(gcli.bytes.size()),
                       sl = uint32_t(sign.bytes.size());
        BitWriter header;
        header.bit(raw ? 1 : 0);
        header.bits(dl, 20);
        header.bits(gl, 20);
        header.bits(sl, 15);
        for (auto* part : {&header, &sig, &gcli, &data, &sign})
            payload.insert(payload.end(), part->bytes.begin(), part->bytes.end());
    }
    BitWriter head;
    head.bits(uint32_t(payload.size()), 24);
    head.bits(uint32_t(p.q), 8);
    head.bits(uint32_t(p.r), 8);
    for (uint8_t m : p.modes) head.bits(m, 2);
    head.align();
    out.insert(out.end(), head.bytes.begin(), head.bytes.end());
    out.insert(out.end(), payload.begin(), payload.end());
}

void put16(std::vector<uint8_t>& v, unsigned x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

std::vector<uint8_t> encodeFrame(const Frame& f, std::mt19937& rng) {
    const Header& h = f.header;
    std::vector<uint8_t> s;
    put16(s, marker::SOC);
    put16(s, marker::CAP);
    put16(s, 34);
    const char* vendor = "CONTACT_INTOPIX_";
    s.insert(s.end(), vendor, vendor + 16);
    s.insert(s.end(), {0xef, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    put16(s, marker::PIH);
    put16(s, 39);
    const size_t lcod = s.size();
    s.insert(s.end(), {0, 0, 0, 0, 0, 0, 0, 0});
    put16(s, unsigned(h.grid_width));
    put16(s, unsigned(h.grid_height));
    put16(s, 0);
    put16(s, unsigned(h.slice_height));
    s.insert(s.end(), {4, 4, 8, 18, 0x44, 0x10, uint8_t(h.nlx << 4 | 1), 0x14});
    s.insert(s.end(),
             {0x50, 0x88, 0x70, 0x83, 0xf0, 0x15, 0x23, 0xd1, 0x49, 0xcd, 0x3f, 0x7f, 0x07});
    put16(s, marker::CDT);
    put16(s, 14);
    for (int c = 0; c < 4; ++c)
        s.insert(s.end(), {14, 0x11, uint8_t(c == 2 ? 0 : (h.nlx << 4 | 1))});
    put16(s, marker::WGT);
    put16(s, unsigned(2 + 2 * h.weight_count));
    for (int i = 0; i < h.weight_count; ++i) s.insert(s.end(), {h.gains[i], h.priorities[i]});
    for (int slice = 0, p = 0; slice < h.slice_count(); ++slice) {
        put16(s, marker::SLH);
        put16(s, 4);
        put16(s, unsigned(slice));
        for (int i = 0; i < h.slice_height && p < h.precinct_count(); ++i, ++p)
            encodePrecinct(f, f.precincts[p], i ? &f.precincts[p - 1] : nullptr, s, rng);
    }
    put16(s, marker::EOC);
    for (int i = 0; i < 4; ++i) s[lcod + i] = uint8_t(s.size() >> (24 - 8 * i));
    return s;
}

// Whole-frame reference: dequantise, inverse wavelet per component, ISO
// star-tetrix on the CFA grid with reflection, then the transfer curve.
std::vector<uint16_t> referenceMosaic(const Frame& f) {
    const Header& h = f.header;
    const Topology& t = f.topology;
    const int w = h.plane_width(), ph = h.plane_height(), precincts = h.precinct_count();
    std::vector<std::vector<int32_t>> planes(4, std::vector<int32_t>(size_t(w) * ph));
    auto bandLine = [&](int p, int b, int line) {
        const Band& band = t.bands[b];
        const BandLine& bl = f.precincts[p].lines[b][line];
        const int gtli = band.gtli(f.precincts[p].q, f.precincts[p].r);
        std::vector<int32_t> v(band.width);
        for (int i = 0; i < band.width; ++i)
            v[i] = dequantise(bl.coded[i], bl.gcli[i / 4], gtli, h.fq);
        return v;
    };
    for (int c = 0; c < 4; ++c) {
        const int first = t.component_first_band[c];
        if (h.components[c].suppressed()) {
            for (int p = 0; p < precincts; ++p)
                for (int line = 0; line < 2; ++line) {
                    const auto v = bandLine(p, first, line);
                    std::copy(v.begin(), v.end(), planes[c].begin() + size_t(2 * p + line) * w);
                }
            continue;
        }
        const int nlx = h.components[c].nlx;
        std::vector<std::vector<int32_t>> low(precincts), high(precincts);
        for (int p = 0; p < precincts; ++p) {
            std::vector<int32_t> lo = bandLine(p, first, 0);
            for (int b = 1; b <= nlx; ++b) lo = inverse53(lo, bandLine(p, first + b, 0));
            low[p] = lo;
            high[p] = inverse53(bandLine(p, first + nlx + 1, 0), bandLine(p, first + nlx + 2, 0));
        }
        for (int x = 0; x < w; ++x) {
            std::vector<int32_t> lf(precincts), hf(precincts);
            for (int p = 0; p < precincts; ++p) {
                lf[p] = low[p][x];
                hf[p] = high[p][x];
            }
            const auto column = inverse53(lf, hf);
            for (int y = 0; y < ph; ++y) planes[c][size_t(y) * w + x] = column[y];
        }
    }
    // Codestream components (Y, Cr, delta, Cb) sit at the quad positions
    // (0,1), (0,0), (1,0), (1,1) in (row, column) terms.
    const int gw = 2 * w, gh = 2 * ph;
    std::vector<int64_t> g(size_t(gw) * gh);
    const int place[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < w; ++x)
                g[size_t(2 * y + place[c][0]) * gw + 2 * x + place[c][1]] =
                    planes[c][size_t(y) * w + x];
    auto at = [&](int y, int x) { return g[size_t(mirror(y, gh)) * gw + mirror(x, gw)]; };
    auto diag = [&](int y, int x) {
        return at(y - 1, x - 1) + at(y - 1, x + 1) + at(y + 1, x - 1) + at(y + 1, x + 1);
    };
    auto cross = [&](int y, int x) {
        return at(y, x - 1) + at(y, x + 1) + at(y - 1, x) + at(y + 1, x);
    };
    auto apply = [&](int py, int px, auto update) {
        for (int y = py; y < gh; y += 2)
            for (int x = px; x < gw; x += 2) g[size_t(y) * gw + x] += update(y, x);
    };
    apply(0, 1, [&](int y, int x) { return -(diag(y, x) >> 3); });  // inverse average step
    apply(1, 0, [&](int y, int x) { return diag(y, x) >> 2; });     // inverse delta step
    apply(0, 1, [&](int y, int x) { return -(cross(y, x) >> 3); }); // inverse Y step, e1 = e2 = 0
    apply(1, 0, [&](int y, int x) { return -(cross(y, x) >> 3); });
    apply(0, 0, [&](int y, int x) { return cross(y, x) >> 2; }); // inverse CbCr step
    apply(1, 1, [&](int y, int x) { return cross(y, x) >> 2; });
    std::vector<uint16_t> mosaic(g.size());
    for (size_t i = 0; i < g.size(); ++i) {
        const int64_t v = std::min<int64_t>(std::max<int64_t>(g[i], -2000000), 2000000);
        mosaic[i] = transfer(int32_t(v), h);
    }
    return mosaic;
}

void testSyntheticCodestreams() {
    std::mt19937 rng(20261016);
    const int sizes[][4] = {{24, 8, 5, 2}, {33, 10, 3, 3}, {18, 6, 2, 1}, {41, 14, 5, 16}};
    for (const auto& size : sizes) {
        Frame f;
        f.header = syntheticHeader(size[0], size[1], size[2], size[3]);
        if (build_topology(f.header, f.topology).status != kOk) {
            check(false, "synthetic topology builds");
            continue;
        }
        for (int p = 0; p < f.header.precinct_count(); ++p) {
            const bool sliceStart = p % f.header.slice_height == 0;
            f.precincts.push_back(
                randomPrecinct(f, sliceStart ? nullptr : &f.precincts.back(), rng));
        }
        const std::vector<uint8_t> stream = encodeFrame(f, rng);
        Header parsed;
        const bool headerOk = parse_header(stream.data(), stream.size(), parsed).status == kOk &&
                              parsed.slice_count() == f.header.slice_count();
        const int gw = f.header.grid_width, gh = f.header.grid_height;
        std::vector<uint16_t> mosaic(size_t(gw + 3) * gh, 0xffff);
        const Error e =
            rawlab_nikon_he::decode(stream.data(), stream.size(), gw, gh, mosaic.data(), gw + 3);
        const std::vector<uint16_t> expected = referenceMosaic(f);
        bool same = e.status == kOk;
        for (int y = 0; same && y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                same = same && mosaic[size_t(y) * (gw + 3) + x] == expected[size_t(y) * gw + x];
        const std::string name = "synthetic " + std::to_string(gw) + "x" + std::to_string(gh) +
                                 " NLx=" + std::to_string(size[2]) + " codestream";
        check(headerOk, name + " header round trips");
        check(same, name + " decodes to the whole-frame reference");
        if (e.status != kOk) std::cout << "  decoder: " << e.message << '\n';

        // Corruption must fail or decode, never run past the buffers.
        int failuresSeen = 0;
        for (int trial = 0; trial < 40; ++trial) {
            std::vector<uint8_t> broken = stream;
            const size_t at = 150 + rng() % (broken.size() - 152);
            broken[at] ^= uint8_t(1 + rng() % 255);
            failuresSeen +=
                rawlab_nikon_he::decode(broken.data(), broken.size(), gw, gh, mosaic.data(), gw + 3)
                    .status != kOk;
        }
        check(failuresSeen > 0, name + " rejects corrupted payloads");
        std::vector<uint8_t> truncated(stream.begin(), stream.end() - 3);
        check(rawlab_nikon_he::decode(truncated.data(), truncated.size(), gw, gh, mosaic.data(),
                                      gw + 3)
                      .status != kOk,
              name + " rejects truncation");
        check(
            rawlab_nikon_he::decode(stream.data(), stream.size(), gw - 2, gh, mosaic.data(), gw + 3)
                    .status == kInvalidData,
            name + " rejects a mismatched raw size");
    }
}

void testSyntheticCoverage() {
    check(coverage.skippedGroups > 0 && coverage.rawPackets > 0 && coverage.verticalCodes > 0 &&
              coverage.paddedPackets > 0 && coverage.extraSigns > 0,
          "synthetic streams cover skipped groups, raw GCLIs, prediction, padding and extra signs");
}

void testDecoderSequencing() {
    std::mt19937 rng(5);
    Frame f;
    f.header = syntheticHeader(20, 8, 2, 2);
    build_topology(f.header, f.topology);
    for (int p = 0; p < f.header.precinct_count(); ++p)
        f.precincts.push_back(randomPrecinct(f, p % 2 ? &f.precincts.back() : nullptr, rng));
    const std::vector<uint8_t> stream = encodeFrame(f, rng);
    std::vector<uint16_t> mosaic(size_t(40) * 16);
    Decoder d;
    bool ok = d.open(stream.data(), stream.size(), 40, 16, mosaic.data(), 40).status == kOk &&
              d.slice_count() == 2;
    ok = ok && d.decode_next_slice().status == kOk && d.finish().status != kOk;
    ok = ok && d.decode_next_slice().status == kOk && d.finish().status == kOk;
    ok = ok && d.decode_next_slice().status != kOk;
    check(ok, "slice-by-slice decoding and early finish are checked");
}

uint64_t fnv1a(const LibRaw& raw) {
    const auto& s = raw.imgdata.sizes;
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < s.raw_height; ++y) {
        const uint16_t* row = raw.imgdata.rawdata.raw_image + size_t(y) * (s.raw_pitch / 2);
        for (int x = 0; x < s.raw_width; ++x) {
            hash = (hash ^ (row[x] & 0xffu)) * 1099511628211ull;
            hash = (hash ^ (row[x] >> 8)) * 1099511628211ull;
        }
    }
    return hash;
}

int decodeFile(const char* path) {
    auto raw = std::make_unique<LibRaw>();
    int result = raw->open_file(path);
    if (result == LIBRAW_SUCCESS) result = raw->unpack();
    if (result != LIBRAW_SUCCESS) {
        std::cout << "error=" << libraw_strerror(result) << '\n';
        return 1;
    }
    const auto& s = raw->imgdata.sizes;
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv1a(*raw)));
    std::cout << "decoder=" << raw->unpack_function_name() << " width=" << s.raw_width
              << " height=" << s.raw_height << " errors=" << raw->error_count()
              << " black=" << raw->imgdata.color.black << " maximum=" << raw->imgdata.color.maximum
              << " fnv1a64=" << hash << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--decode") return decodeFile(argv[2]);
    testHeader();
    testTransfer();
    testWaveletLifting();
    testSyntheticCodestreams();
    testSyntheticCoverage();
    testDecoderSequencing();
    std::cout << (failures ? "FAILED " : "PASSED ") << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
