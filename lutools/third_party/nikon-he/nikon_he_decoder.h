// SPDX-License-Identifier: LGPL-2.1-only
//
// Nikon "High Efficiency" (HE and HE*) NEF decoder.
//
// HE and HE* store the CFA as one ISO/IEC 21122 (JPEG XS) codestream written
// by intoPIX TicoRAW. Both modes share every header and differ only in the
// per-precinct quantisation, so one decoder covers both.
//
// Pipeline, one precinct (two component lines) at a time:
//
//   entropy decoding -> uniform dequantisation -> inverse 5/3 DWT
//   -> inverse star-tetrix (full 2D, e1 = e2 = 0) -> vendor transfer curve
//
// The codestream deviates from stock JPEG XS in ways that only this profile
// needs: a three-byte CDT entry that lets component 2 skip the wavelet,
// subband-major packets whose WGT table follows packet order, implicit
// star-tetrix component roles, and a 13-byte PIH extension carrying the
// transfer parameters. Anything outside the verified profile is rejected.
//
// Provenance (no Nikon SDK material is used):
// * Codestream, packet and entropy layout: dnglab PR #835 by Nicolai Buchwitz
//   (rawler/src/decompressors/jpegxs, LGPL-2.1), ported to C++.
// * Inverse colour transform: ISO/IEC 21122-1 Annex F as implemented by
//   SVT-JPEG-XS, with the component placement and boundary handling of
//   LibRaw PR #826 (Dmitri Sotnikov) including the bottom-row correction
//   published by RdWing in that PR.
// * Transfer curve and its PIH parameters: RdWing, LibRaw PR #826, reported
//   to match Adobe DNG Converter on Z9, Z8, Z6III, Zf and Z5II files.
//
// Header only and C++11, so it can be compiled into LibRaw unchanged.

#pragma once

// LibRaw's internal headers define macros such as `width` and `black`.
#ifdef DCRAW_DEFS_H
#error "Include nikon_he_decoder.h before LibRaw's internal dcraw_defs.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <new>
#include <vector>

namespace rawlab_nikon_he {

enum Status {
    kOk = 0,
    kInvalidData = 1, // corrupt or truncated codestream
    kUnsupported = 2, // well-formed but outside the verified Nikon profile
    kNoMemory = 3
};

struct Error {
    Status status;
    const char* message;
};

namespace detail {

const int kMaxBands = 32;
const int kTruncationMax = 15;
const uint16_t kSignBit = 0x8000;

inline Error ok() {
    Error e = {kOk, ""};
    return e;
}
inline Error invalid(const char* m) {
    Error e = {kInvalidData, m};
    return e;
}
inline Error unsupported(const char* m) {
    Error e = {kUnsupported, m};
    return e;
}

// MSB-first bit reader with bounds checks on every read.
class BitReader {
public:
    BitReader(const uint8_t* buf, size_t len, size_t byte_pos)
        : buf_(buf), bit_len_(len * 8), pos_(byte_pos * 8) {}

    size_t byte_pos() const { return pos_ >> 3; }
    bool aligned() const { return (pos_ & 7) == 0; }

    bool bit(unsigned& v) {
        if (pos_ >= bit_len_) return false;
        v = (buf_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
        ++pos_;
        return true;
    }

    // Coefficient bit planes are 4-bit aligned by construction.
    bool nibble(unsigned& v) {
        if ((pos_ & 3) != 0 || pos_ + 4 > bit_len_) return false;
        const unsigned byte = buf_[pos_ >> 3];
        v = (pos_ & 4) ? (byte & 0x0fu) : (byte >> 4);
        pos_ += 4;
        return true;
    }

    // Number of 1-bits before the terminating 0-bit. 32 ones is corrupt.
    bool unary(unsigned& v) {
        unsigned n = 0, b = 0;
        for (;;) {
            if (!bit(b)) return false;
            if (!b) break;
            if (++n >= 32) return false;
        }
        v = n;
        return true;
    }

    bool bytes(size_t n, const uint8_t*& out) {
        if (!aligned() || pos_ + n * 8 > bit_len_) return false;
        out = buf_ + (pos_ >> 3);
        pos_ += n * 8;
        return true;
    }

    void align() { pos_ = (pos_ + 7) & ~size_t(7); }

    bool skip_bits(size_t n) {
        if (n > bit_len_ - pos_) return false;
        pos_ += n;
        return true;
    }

private:
    const uint8_t* buf_;
    size_t bit_len_;
    size_t pos_;
};

inline unsigned be16(const uint8_t* p) { return (unsigned(p[0]) << 8) | p[1]; }
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

} // namespace detail

struct Component {
    int bit_depth;
    int sx, sy;
    // Own decomposition; both zero means the component skips the wavelet.
    int nlx, nly;
    bool suppressed() const { return nlx == 0 && nly == 0; }
    int bands() const { return suppressed() ? 1 : 2 * nly + nlx + 1; }
};

struct Header {
    uint32_t lcod;
    int grid_width, grid_height; // CFA grid; each component plane is half
    int precinct_width;          // Cw, zero means full width
    int slice_height;            // Hsl, in precincts
    int group_size, sig_group_size;
    int bw, fq, br;
    int fslc, ppoc, cpih;
    int nlx, nly;
    int lh, rl, qpih, fs, rm;
    // Transfer parameters from the PIH vendor extension.
    int black;     // B, 12 bits
    uint32_t gain; // A, 18-bit Q16
    int knee;      // C, 16 bits
    Component components[4];
    int component_count;
    uint8_t gains[detail::kMaxBands];
    uint8_t priorities[detail::kMaxBands];
    int weight_count;
    size_t first_slice;

    int plane_width() const { return grid_width / 2; }
    int plane_height() const { return grid_height / 2; }
    int precinct_height() const { return 1 << nly; }
    int precinct_count() const {
        return (plane_height() + precinct_height() - 1) / precinct_height();
    }
    int slice_count() const { return (precinct_count() + slice_height - 1) / slice_height; }
    int total_bands() const {
        int n = 0;
        for (int i = 0; i < component_count; ++i) n += components[i].bands();
        return n;
    }
};

namespace marker {
const unsigned SOC = 0xff10, EOC = 0xff11, PIH = 0xff12, CDT = 0xff13, WGT = 0xff14, COM = 0xff15,
               NLT = 0xff16, CWD = 0xff17, CTS = 0xff18, CRG = 0xff19, SLH = 0xff20, CAP = 0xff50;
}

// Parse everything up to the first slice header and check that the stream is
// the Nikon profile this decoder was verified against.
inline Error parse_header(const uint8_t* buf, size_t len, Header& h) {
    using namespace detail;
    memset(&h, 0, sizeof(h));
    if (len < 4 || be16(buf) != marker::SOC) return invalid("no JPEG XS SOC marker");
    size_t pos = 2;
    bool have_cap = false, have_pih = false, have_cdt = false, have_wgt = false;
    for (;;) {
        if (pos + 2 > len) return invalid("codestream ends inside the main header");
        const unsigned code = be16(buf + pos);
        if (code == marker::SLH) {
            if (!have_cap || !have_pih || !have_cdt || !have_wgt)
                return unsupported("JPEG XS main header lacks CAP, PIH, CDT or WGT");
            h.first_slice = pos;
            break;
        }
        if (code == marker::EOC) return invalid("EOC before the first slice");
        if (pos + 4 > len) return invalid("truncated marker segment");
        const size_t seg = be16(buf + pos + 2);
        if (seg < 2 || pos + 2 + seg > len) return invalid("bad marker segment length");
        const uint8_t* body = buf + pos + 4;
        const size_t body_len = seg - 2;
        switch (code) {
        case marker::CAP:
            // TicoRAW streams carry a vendor string instead of a capability map.
            if (have_cap || body_len < 16 || memcmp(body, "CONTACT_INTOPIX_", 16) != 0)
                return unsupported("not a TicoRAW capability marker");
            have_cap = true;
            break;
        case marker::PIH: {
            // 24 standard payload bytes plus at least 12 bytes of vendor extension.
            if (have_pih || body_len < 36) return unsupported("PIH lacks the vendor extension");
            h.lcod = be32(body);
            const unsigned ppih = be16(body + 4), plev = be16(body + 6);
            h.grid_width = be16(body + 8);
            h.grid_height = be16(body + 10);
            h.precinct_width = be16(body + 12);
            h.slice_height = be16(body + 14);
            h.component_count = body[16];
            h.group_size = body[17];
            h.sig_group_size = body[18];
            h.bw = body[19];
            h.fq = body[20] >> 4;
            h.br = body[20] & 15;
            h.fslc = body[21] >> 7;
            h.ppoc = (body[21] >> 4) & 7;
            h.cpih = body[21] & 15;
            h.nlx = body[22] >> 4;
            h.nly = body[22] & 15;
            h.lh = body[23] >> 7;
            h.rl = (body[23] >> 6) & 1;
            h.qpih = (body[23] >> 4) & 3;
            h.fs = (body[23] >> 2) & 3;
            h.rm = body[23] & 3;
            const uint8_t* ext = body + 24;
            h.black = ((ext[3] & 15) << 8) | ext[4];
            h.gain = (uint32_t(ext[7] & 3) << 16) | (uint32_t(ext[8]) << 8) | ext[9];
            h.knee = (ext[10] << 8) | ext[11];
            if (ppih != 0 || plev != 0 || h.precinct_width != 0 || h.slice_height < 1 ||
                h.component_count != 4 || h.group_size != 4 || h.sig_group_size != 8 ||
                h.bw != 18 || h.fq != 4 || h.br != 4 || h.fslc != 0 || h.ppoc != 1 || h.cpih != 0 ||
                h.nlx < 1 || h.nlx > 8 || h.nly != 1 || h.lh != 0 || h.rl != 0 || h.qpih != 1 ||
                h.fs != 1 || h.rm != 0)
                return unsupported("JPEG XS picture header is outside the Nikon HE profile");
            if (h.grid_width < 8 || h.grid_height < 8 || (h.grid_width & 1) || (h.grid_height & 1))
                return unsupported("unsupported CFA grid size");
            if (h.gain == 0 || h.knee == 0) return unsupported("missing transfer parameters");
            have_pih = true;
            break;
        }
        case marker::CDT: {
            // Nikon writes three bytes per component: depth, sampling, decomposition.
            if (!have_pih || have_cdt || body_len != size_t(3 * h.component_count))
                return unsupported("unexpected component table");
            for (int c = 0; c < h.component_count; ++c) {
                Component& comp = h.components[c];
                comp.bit_depth = body[3 * c];
                comp.sx = body[3 * c + 1] >> 4;
                comp.sy = body[3 * c + 1] & 15;
                comp.nlx = body[3 * c + 2] >> 4;
                comp.nly = body[3 * c + 2] & 15;
                if (comp.bit_depth != 14 || comp.sx != 1 || comp.sy != 1)
                    return unsupported("unexpected component depth or sampling");
                // Component 2 (the star-tetrix delta plane) is the one coded without
                // a wavelet; the others share the frame decomposition.
                const bool expect_suppressed = (c == 2);
                if (expect_suppressed ? !comp.suppressed()
                                      : (comp.nlx != h.nlx || comp.nly != h.nly))
                    return unsupported("unexpected component decomposition");
            }
            have_cdt = true;
            break;
        }
        case marker::WGT:
            if (!have_cdt || have_wgt || (body_len & 1) || body_len / 2 > size_t(kMaxBands))
                return unsupported("unexpected weights table");
            h.weight_count = int(body_len / 2);
            for (int b = 0; b < h.weight_count; ++b) {
                h.gains[b] = body[2 * b];
                h.priorities[b] = body[2 * b + 1];
            }
            if (h.weight_count != h.total_bands())
                return invalid("weights table does not match the band count");
            have_wgt = true;
            break;
        case marker::NLT:
        case marker::CWD:
        case marker::CTS:
        case marker::CRG:
            return unsupported("JPEG XS marker outside the Nikon HE profile");
        default:
            // COM and unknown informational segments are skipped.
            break;
        }
        pos += 2 + seg;
    }
    if (h.lcod < h.first_slice + 2 || h.lcod > len)
        return invalid("codestream length does not match the strip");
    return ok();
}

// One wavelet band of one component, or the single band of the suppressed one.
struct Band {
    int component;
    int width;              // coefficients per band line
    int gcli_width;         // code groups per line
    int significance_width; // significance groups per line
    int lines;              // band lines per precinct
    uint8_t gain, priority;

    int gtli(int q, int r) const {
        int t = q - int(gain) - (int(priority) < r ? 1 : 0);
        return std::min(std::max(t, 0), detail::kTruncationMax);
    }
};

// A contiguous run of one component's bands; one line of each band.
struct Packet {
    int component;
    int first_band, end_band;
    int line;
};

struct Topology {
    std::vector<Band> bands;     // component-major, band 0 most decomposed
    std::vector<Packet> packets; // emission order within a precinct
    int component_first_band[4];
    int component_band_count[4];
};

// Horizontal widths of a line split nlx times; final low-pass first.
inline void horizontal_widths(int w, int nlx, std::vector<int>& out) {
    std::vector<int> highs;
    for (int i = 0; i < nlx; ++i) {
        const int high = w / 2;
        highs.push_back(high);
        w -= high;
    }
    out.clear();
    out.push_back(w);
    for (int i = nlx - 1; i >= 0; --i) out.push_back(highs[i]);
}

inline Error build_topology(const Header& h, Topology& t) {
    using namespace detail;
    const int w = h.plane_width();
    t.bands.clear();
    t.packets.clear();
    std::vector<int> widths;
    for (int c = 0; c < h.component_count; ++c) {
        const Component& comp = h.components[c];
        t.component_first_band[c] = int(t.bands.size());
        int lines;
        if (comp.suppressed()) {
            widths.assign(1, w);
            lines = h.precinct_height();
        } else {
            // Bands 0..NLx split the vertical low-pass; the vertical high-pass is
            // split once horizontally into the final two bands.
            horizontal_widths(w, comp.nlx, widths);
            widths.push_back(w - w / 2);
            widths.push_back(w / 2);
            lines = 1;
        }
        for (size_t i = 0; i < widths.size(); ++i) {
            if (widths[i] < 1) return unsupported("plane too narrow for the decomposition");
            Band b;
            b.component = c;
            b.width = widths[i];
            b.gcli_width = (b.width + h.group_size - 1) / h.group_size;
            b.significance_width = (b.gcli_width + h.sig_group_size - 1) / h.sig_group_size;
            b.lines = lines;
            b.gain = b.priority = 0;
            t.bands.push_back(b);
        }
        t.component_band_count[c] = int(t.bands.size()) - t.component_first_band[c];
    }
    if (int(t.bands.size()) != h.weight_count) return invalid("band count mismatch");

    // Packets are subband-major: for each vertical half, walk the components.
    for (int half = 0; half < 2; ++half) {
        for (int c = 0; c < h.component_count; ++c) {
            const int first = t.component_first_band[c];
            const int end = first + t.component_band_count[c];
            Packet p;
            p.component = c;
            p.line = half;
            if (h.components[c].suppressed()) {
                p.first_band = first;
                p.end_band = end;
            } else {
                // Split where the running coefficient count reaches half the total;
                // with NLy = 1 that is after the vertical low-pass bands.
                long total = 0, acc = 0;
                for (int b = first; b < end; ++b) total += t.bands[b].width;
                int split = end;
                for (int b = first; b < end; ++b) {
                    acc += t.bands[b].width;
                    if (2 * acc >= total) {
                        split = b + 1;
                        break;
                    }
                }
                p.first_band = half == 0 ? first : split;
                p.end_band = half == 0 ? split : end;
            }
            t.packets.push_back(p);
        }
    }

    // WGT entries follow packet emission order; each band takes the next
    // entry the first time a packet names it.
    std::vector<bool> seen(t.bands.size(), false);
    int assigned = 0;
    for (size_t i = 0; i < t.packets.size(); ++i) {
        for (int b = t.packets[i].first_band; b < t.packets[i].end_band; ++b) {
            if (seen[b]) continue;
            seen[b] = true;
            if (assigned >= h.weight_count) return invalid("weights table too short");
            t.bands[b].gain = h.gains[assigned];
            t.bands[b].priority = h.priorities[assigned];
            ++assigned;
        }
    }
    if (assigned != h.weight_count) return invalid("packets do not cover every band");
    return ok();
}

// Decoded GCLIs and sign-magnitude coefficients of every band of a precinct.
struct Precinct {
    struct BandData {
        int gtli;
        std::vector<uint8_t> gcli;   // lines x gcli_width
        std::vector<uint16_t> coeff; // lines x width; magnitude << gtli, sign in bit 15
    };
    std::vector<BandData> bands;
    std::vector<uint8_t> significance; // scratch for one packet

    void allocate(const Topology& t) {
        size_t sig = 0;
        for (size_t b = 0; b < t.bands.size(); ++b) sig += size_t(t.bands[b].significance_width);
        significance.assign(sig, 0);
        bands.resize(t.bands.size());
        for (size_t b = 0; b < t.bands.size(); ++b) {
            bands[b].gcli.assign(size_t(t.bands[b].lines) * t.bands[b].gcli_width, 0);
            bands[b].coeff.assign(size_t(t.bands[b].lines) * t.bands[b].width, 0);
        }
    }
};

namespace detail {

// Vertically predicted GCLI: zig-zag delta inside the room above GTLI.
inline int vertical_gcli(unsigned x, int m_top, int gtli) {
    const int xi = int(x);
    const int threshold = std::max(m_top - gtli, 0);
    int delta;
    if (xi > 2 * threshold)
        delta = xi - threshold;
    else if (xi & 1)
        delta = -((xi + 1) / 2);
    else
        delta = xi / 2;
    return m_top + delta;
}

inline bool finish_subpacket(BitReader& r, size_t start, size_t declared) {
    const size_t used = r.byte_pos() - start;
    if (used > declared) return false;
    return r.skip_bits((declared - used) * 8);
}

inline Error decode_packet(BitReader& r, const Header& h, const Topology& t, const uint8_t* modes,
                           Precinct& cur, const Packet& packet, const Precinct* top) {
    const uint8_t* m = 0;
    if (!r.bytes(7, m)) return invalid("truncated packet header");
    const bool raw_mode = (m[0] >> 7) != 0;
    const size_t data_len = (size_t(m[0] & 0x7f) << 13) | (size_t(m[1]) << 5) | (m[2] >> 3);
    const size_t gcli_len =
        (size_t(m[2] & 7) << 17) | (size_t(m[3]) << 9) | (size_t(m[4]) << 1) | (m[5] >> 7);
    const size_t sign_len = (size_t(m[5] & 0x7f) << 8) | m[6];
    const int ss = h.sig_group_size;

    // Each band contributes one line: the suppressed band's line is the
    // packet's half, every other band has a single line.
#define RAWLAB_HE_LINE(band) ((band).lines > 1 ? packet.line : 0)

    if (raw_mode) {
        // Raw GCLIs: Br-bit absolute values, no significance sub-packet.
        const size_t start = r.byte_pos();
        for (int b = packet.first_band; b < packet.end_band; ++b) {
            const Band& band = t.bands[b];
            uint8_t* out = &cur.bands[b].gcli[size_t(RAWLAB_HE_LINE(band)) * band.gcli_width];
            for (int g = 0; g < band.gcli_width; ++g) {
                unsigned v;
                if (!r.nibble(v)) return invalid("truncated raw GCLI");
                out[g] = uint8_t(v);
            }
        }
        r.align();
        if (!finish_subpacket(r, start, gcli_len)) return invalid("GCLI sub-packet overrun");
    } else {
        // Significance sub-packet: one flag per group of Ss GCLIs; set = skipped.
        const uint8_t* sig[kMaxBands];
        {
            uint8_t* next_flags = &cur.significance[0];
            for (int b = packet.first_band; b < packet.end_band; ++b) {
                const int k = b - packet.first_band;
                sig[k] = 0;
                if (!(modes[b] & 2)) continue;
                sig[k] = next_flags;
                for (int s = 0; s < t.bands[b].significance_width; ++s) {
                    unsigned v;
                    if (!r.bit(v)) return invalid("truncated significance flags");
                    next_flags[s] = uint8_t(v);
                }
                next_flags += t.bands[b].significance_width;
            }
        }
        r.align();

        // GCLI sub-packet: one unary bitstream across the packet's bands.
        const size_t start = r.byte_pos();
        for (int b = packet.first_band; b < packet.end_band; ++b) {
            const int k = b - packet.first_band;
            const Band& band = t.bands[b];
            const int line = RAWLAB_HE_LINE(band);
            Precinct::BandData& data = cur.bands[b];
            const int gtli = data.gtli;
            uint8_t* out = &data.gcli[size_t(line) * band.gcli_width];
            // Vertical prediction uses the previous line of this precinct, or the
            // last line of the precinct above; slices start without a reference.
            const uint8_t* ref = 0;
            int ref_gtli = 0;
            if (modes[b] & 1) {
                if (line > 0) {
                    ref = &data.gcli[size_t(line - 1) * band.gcli_width];
                    ref_gtli = gtli;
                } else if (top) {
                    ref = &top->bands[b].gcli[size_t(band.lines - 1) * band.gcli_width];
                    ref_gtli = top->bands[b].gtli;
                }
            }
            for (int g = 0; g < band.gcli_width; ++g) {
                const bool significant = !sig[k] || sig[k][g / ss] == 0;
                int value;
                if (ref) {
                    const int m_top = std::max(int(ref[g]), std::max(ref_gtli, gtli));
                    if (significant) {
                        unsigned x;
                        if (!r.unary(x)) return invalid("truncated GCLI");
                        value = vertical_gcli(x, m_top, gtli);
                    } else
                        value = h.rm ? gtli : m_top;
                } else if (significant) {
                    unsigned x;
                    if (!r.unary(x)) return invalid("truncated GCLI");
                    value = gtli + int(x);
                } else
                    value = gtli;
                if (value < 0 || value > kTruncationMax) return invalid("GCLI out of range");
                out[g] = uint8_t(value);
            }
        }
        r.align();
        if (!finish_subpacket(r, start, gcli_len)) return invalid("GCLI sub-packet overrun");
    }

    // Data sub-packet: per group, (gcli - gtli) bit planes of 4 bits, MSB first.
    // Partial trailing groups still code four coefficients; the extras are
    // dropped but their sign bits remain in the sign sub-packet.
    int leftover[kMaxBands];
    {
        const size_t start = r.byte_pos();
        for (int b = packet.first_band; b < packet.end_band; ++b) {
            const Band& band = t.bands[b];
            const int line = RAWLAB_HE_LINE(band);
            Precinct::BandData& data = cur.bands[b];
            const uint8_t* gclis = &data.gcli[size_t(line) * band.gcli_width];
            uint16_t* coeff = &data.coeff[size_t(line) * band.width];
            const int gtli = data.gtli;
            int extra = 0;
            for (int g = 0; g < band.gcli_width; ++g) {
                uint32_t acc[4] = {0, 0, 0, 0};
                if (gclis[g] > gtli) {
                    const int planes = int(gclis[g]) - gtli;
                    for (int p = 0; p < planes; ++p) {
                        unsigned v;
                        if (!r.nibble(v)) return invalid("truncated coefficient data");
                        acc[0] = (acc[0] << 1) | ((v >> 3) & 1);
                        acc[1] = (acc[1] << 1) | ((v >> 2) & 1);
                        acc[2] = (acc[2] << 1) | ((v >> 1) & 1);
                        acc[3] = (acc[3] << 1) | (v & 1);
                    }
                }
                const int base = 4 * g;
                const int n = std::min(4, band.width - base);
                for (int j = 0; j < 4; ++j) {
                    const uint16_t mag = uint16_t((acc[j] << gtli) & 0x7fff);
                    if (j < n)
                        coeff[base + j] = mag;
                    else if (mag)
                        ++extra;
                }
            }
            leftover[b - packet.first_band] = extra;
        }
        r.align();
        if (!finish_subpacket(r, start, data_len)) return invalid("data sub-packet overrun");
    }

    // Sign sub-packet: one bit per non-zero coefficient (Fs = 1).
    {
        const size_t start = r.byte_pos();
        for (int b = packet.first_band; b < packet.end_band; ++b) {
            const Band& band = t.bands[b];
            uint16_t* coeff = &cur.bands[b].coeff[size_t(RAWLAB_HE_LINE(band)) * band.width];
            for (int i = 0; i < band.width; ++i) {
                if (!coeff[i]) continue;
                unsigned v;
                if (!r.bit(v)) return invalid("truncated sign data");
                if (v) coeff[i] |= kSignBit;
            }
            if (!r.skip_bits(size_t(leftover[b - packet.first_band])))
                return invalid("truncated sign data");
        }
        r.align();
        if (!finish_subpacket(r, start, sign_len)) return invalid("sign sub-packet overrun");
    }
#undef RAWLAB_HE_LINE
    return ok();
}

} // namespace detail

// Decode one precinct at `offset`; `top` is the precinct above in the same
// slice, or null at a slice start. Returns the offset of the next precinct.
inline Error decode_precinct(const uint8_t* buf, size_t len, size_t offset, const Header& h,
                             const Topology& t, Precinct& cur, const Precinct* top, size_t& next) {
    using namespace detail;
    const int nbands = int(t.bands.size());
    const size_t header_len = 5 + (2 * size_t(nbands) + 7) / 8;
    if (offset + header_len > len) return invalid("truncated precinct header");
    const uint8_t* p = buf + offset;
    const size_t payload = (size_t(p[0]) << 16) | (size_t(p[1]) << 8) | p[2];
    const int q = p[3], rq = p[4];
    if (payload > len - offset - header_len) return invalid("precinct overruns the codestream");
    uint8_t modes[kMaxBands];
    for (int b = 0; b < nbands; ++b) {
        const size_t bit = 40 + 2 * size_t(b);
        const unsigned hi = (p[bit >> 3] >> (7 - (bit & 7))) & 1u;
        const unsigned lo = (p[(bit + 1) >> 3] >> (7 - ((bit + 1) & 7))) & 1u;
        modes[b] = uint8_t((hi << 1) | lo);
    }
    for (int b = 0; b < nbands; ++b) cur.bands[b].gtli = t.bands[b].gtli(q, rq);
    const size_t end = offset + header_len + payload;
    // The reader is clamped to this precinct.
    BitReader r(buf, end, offset + header_len);
    for (size_t i = 0; i < t.packets.size(); ++i) {
        Error e = decode_packet(r, h, t, modes, cur, t.packets[i], top);
        if (e.status != kOk) return e;
    }
    next = end;
    return ok();
}

// Exact transfer from the coded domain to 14-bit sensor values (RdWing,
// LibRaw PR #826): a quadratic with a knee at C and Q16 gain A above it.
inline uint16_t transfer(int32_t v, const Header& h) {
    int64_t i = int64_t(v) + 32768;
    i = std::min<int64_t>(std::max<int64_t>(i, 0), 65535);
    const int64_t d = i - h.knee;
    int64_t q;
    if (d < 0)
        q = -d * d;
    else {
        const int64_t t = (d * int64_t(h.gain) + 32768) >> 16;
        q = t * t;
    }
    // Floor division for negative q as well.
    const int64_t num = q + 163840;
    const int64_t shifted = num >= 0 ? (num >> 18) : -((-num + (1 << 18) - 1) >> 18);
    const int64_t raw = int64_t(h.black) + shifted;
    return uint16_t(std::min<int64_t>(std::max<int64_t>(raw, 0), 16383));
}

// Receives component rows as the wavelet completes them and emits Bayer row
// pairs. Codestream components are (Y, Cr, delta, Cb); Y lands on the green
// of the even CFA row, delta on the green of the odd row. Neighbours outside
// the frame reflect, which in quad coordinates is a clamp.
class StarTetrix {
public:
    static const int kRing = 8;

    bool init(int w, int h, const Header& hdr, uint16_t* out, size_t out_pitch) {
        w_ = w;
        h_ = h;
        out_ = out;
        pitch_ = out_pitch;
        for (int c = 0; c < 4; ++c) rows_ready_[c] = 0;
        emitted_ = 0;
        processed_ = 0;
        try {
            for (int c = 0; c < 4; ++c) in_[c].assign(size_t(kRing) * w, 0);
            yp_.assign(size_t(kRing) * w, 0);
            g1_.assign(size_t(kRing) * w, 0);
            g2_.assign(size_t(kRing) * w, 0);
            lut_.resize(65536);
        } catch (const std::bad_alloc&) {
            return false;
        }
        for (int i = 0; i < 65536; ++i) lut_[i] = transfer(int32_t(i) - 32768, hdr);
        return true;
    }

    // Storage for row `y` of component `c`, valid until the row is committed.
    int32_t* row(int c, int y) { return &in_[c][size_t(y % kRing) * w_]; }

    void commit(int c, int y) {
        rows_ready_[c] = y + 1;
        const int ready = std::min(std::min(rows_ready_[0], rows_ready_[1]),
                                   std::min(rows_ready_[2], rows_ready_[3]));
        while (processed_ < ready) step(processed_++);
        if (processed_ == h_ && emitted_ < h_) finish();
    }

    bool complete() const { return emitted_ == h_; }

private:
    int32_t* at(std::vector<int32_t>& v, int y) { return &v[size_t(y % kRing) * w_]; }
    int up(int y) const { return y > 0 ? y - 1 : 0; }
    int down(int y) const { return y + 1 < h_ ? y + 1 : h_ - 1; }

    // Input row k has arrived for every component.
    void step(int k) {
        const int w = w_;
        const int32_t *Y = row(0, k), *Cr = row(1, k), *D = row(2, k), *Cb = row(3, k);
        const int32_t *Dup = row(2, up(k)), *Cbup = row(3, up(k));
        int32_t *yp = at(yp_, k), *g1 = at(g1_, k);
        // Inverse average step: Y sheds the delta diagonal average.
        for (int c = 0; c < w; ++c) {
            const int cr = c + 1 < w ? c + 1 : w - 1;
            yp[c] = Y[c] - ((Dup[c] + Dup[cr] + D[c] + D[cr]) >> 3);
        }
        // Inverse Y step for the even-row green (e1 = e2 = 0).
        for (int c = 0; c < w; ++c) {
            const int cr = c + 1 < w ? c + 1 : w - 1;
            g1[c] = yp[c] - ((Cr[c] + Cr[cr] + Cbup[c] + Cb[c]) >> 3);
        }
        if (k > 0) odd_green_and_emit(k - 1, k);
    }

    // Row `y` of delta/odd green needs Y' of row y + 1 (`below`, clamped).
    void odd_green_and_emit(int y, int below) {
        const int w = w_;
        const int32_t *D = row(2, y), *Cr = row(1, y), *Cb = row(3, y);
        const int32_t* Crdown = row(1, below);
        const int32_t *yp = at(yp_, y), *ypdown = at(yp_, below);
        int32_t* g2 = at(g2_, y);
        for (int c = 0; c < w; ++c) {
            const int cl = c > 0 ? c - 1 : 0;
            // Inverse delta step, then the inverse Y step for the odd-row green.
            const int32_t dp = D[c] + ((yp[cl] + yp[c] + ypdown[cl] + ypdown[c]) >> 2);
            g2[c] = dp - ((Cb[cl] + Cb[c] + Cr[c] + Crdown[c]) >> 3);
        }
        emit(y, below);
    }

    // Inverse CbCr step and transfer for Bayer rows 2y and 2y + 1.
    void emit(int y, int below) {
        const int w = w_;
        const int32_t *Cr = row(1, y), *Cb = row(3, y);
        const int32_t *g1 = at(g1_, y), *g1down = at(g1_, below);
        const int32_t *g2 = at(g2_, y), *g2up = at(g2_, up(y));
        uint16_t* top = out_ + size_t(2 * y) * pitch_;
        uint16_t* bottom = top + pitch_;
        const uint16_t* lut = &lut_[0];
        for (int c = 0; c < w; ++c) {
            const int cl = c > 0 ? c - 1 : 0;
            const int cr = c + 1 < w ? c + 1 : w - 1;
            const int32_t red = Cr[c] + ((g1[cl] + g1[c] + g2up[c] + g2[c]) >> 2);
            const int32_t blue = Cb[c] + ((g2[c] + g2[cr] + g1[c] + g1down[c]) >> 2);
            top[2 * c] = lut[clamp16(red)];
            top[2 * c + 1] = lut[clamp16(g1[c])];
            bottom[2 * c] = lut[clamp16(g2[c])];
            bottom[2 * c + 1] = lut[clamp16(blue)];
        }
        emitted_ = y + 1;
    }

    void finish() {
        // The last row's lower neighbours reflect onto itself.
        odd_green_and_emit(h_ - 1, h_ - 1);
    }

    static int clamp16(int32_t v) {
        const int32_t i = v + 32768;
        return i < 0 ? 0 : (i > 65535 ? 65535 : int(i));
    }

    int w_, h_;
    uint16_t* out_;
    size_t pitch_;
    int rows_ready_[4];
    int processed_, emitted_;
    std::vector<int32_t> in_[4], yp_, g1_, g2_;
    std::vector<uint16_t> lut_;
};

// Streaming inverse 5/3 DWT. Each precinct completes plane lines 2p - 1 and
// 2p (and 2p + 1 for the last one); the suppressed component is copied.
// `Sink` provides row(c, y) storage and commit(c, y), like StarTetrix.
template <class Sink> class InverseWavelet {
public:
    bool init(const Header& h, const Topology& t, Sink* sink) {
        w_ = h.plane_width();
        precincts_ = h.precinct_count();
        next_ = 0;
        fq_ = h.fq;
        sink_ = sink;
        t_ = &t;
        try {
            for (int c = 0; c < 4; ++c) {
                comps_[c].first_band = t.component_first_band[c];
                comps_[c].nlx = h.components[c].nlx;
                comps_[c].suppressed = h.components[c].suppressed();
                comps_[c].hf_prev.assign(w_, 0);
                comps_[c].even_prev.assign(w_, 0);
            }
            lo_.assign(w_, 0);
            swap_.assign(w_, 0);
            hi_.assign(w_, 0);
            hf_.assign(w_, 0);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }

    Error push(const Precinct& p) {
        using namespace detail;
        if (next_ >= precincts_) return invalid("too many precincts");
        const int pi = next_++;
        const int w = w_;
        const bool last = pi == precincts_ - 1;
        for (int c = 0; c < 4; ++c) {
            Comp& comp = comps_[c];
            if (comp.suppressed) {
                const Band& band = t_->bands[comp.first_band];
                for (int line = 0; line < 2; ++line) {
                    const int y = 2 * pi + line;
                    extract(p.bands[comp.first_band], band.width, line, sink_->row(c, y));
                    sink_->commit(c, y);
                }
                continue;
            }
            // Horizontal cascade up to the vertical low-pass line, then rebuild the
            // vertical high-pass line from the last two bands.
            const int first = comp.first_band;
            const int nlx = comp.nlx;
            int lo_len = t_->bands[first].width;
            extract(p.bands[first], lo_len, 0, &lo_[0]);
            for (int b = 1; b <= nlx; ++b) {
                const int wb = t_->bands[first + b].width;
                extract(p.bands[first + b], wb, 0, &hi_[0]);
                idwt_horizontal(&lo_[0], lo_len, &hi_[0], wb, &swap_[0]);
                lo_.swap(swap_);
                lo_len += wb;
            }
            const int wl = t_->bands[first + nlx + 1].width;
            const int wh = t_->bands[first + nlx + 2].width;
            extract(p.bands[first + nlx + 1], wl, 0, &swap_[0]);
            extract(p.bands[first + nlx + 2], wh, 0, &hi_[0]);
            idwt_horizontal(&swap_[0], wl, &hi_[0], wh, &hf_[0]);

            // Vertical lifting; plane line 2p - 2 is in even_prev.
            const int32_t *lf = &lo_[0], *hf = &hf_[0], *hfp = &comp.hf_prev[0];
            int32_t* even = sink_->row(c, 2 * pi);
            if (pi == 0)
                for (int x = 0; x < w; ++x) even[x] = lf[x] - ((hf[x] + 1) >> 1);
            else {
                for (int x = 0; x < w; ++x) even[x] = lf[x] - ((hfp[x] + hf[x] + 2) >> 2);
                int32_t* odd = sink_->row(c, 2 * pi - 1);
                const int32_t* above = &comp.even_prev[0];
                for (int x = 0; x < w; ++x) odd[x] = hfp[x] + ((above[x] + even[x]) >> 1);
                sink_->commit(c, 2 * pi - 1);
            }
            std::copy(even, even + w, comp.even_prev.begin());
            sink_->commit(c, 2 * pi);
            if (last) {
                // Bottom boundary: the final odd line extends off the last even one.
                int32_t* odd = sink_->row(c, 2 * pi + 1);
                for (int x = 0; x < w; ++x) odd[x] = hf[x] + comp.even_prev[x];
                sink_->commit(c, 2 * pi + 1);
            }
            comp.hf_prev.swap(hf_);
        }
        return ok();
    }

    bool complete() const { return next_ == precincts_; }

    // One level of inverse horizontal 5/3 lifting with symmetric extension.
    static void idwt_horizontal(const int32_t* lf, int nl, const int32_t* hf, int nh,
                                int32_t* out) {
        const int len = nl + nh;
        if (len == 1) {
            out[0] = lf[0];
            return;
        }
        out[0] = lf[0] - ((hf[0] + 1) >> 1);
        for (int k = 0; k < (len - 2) / 2; ++k) {
            out[2 * k + 2] = lf[k + 1] - ((hf[k] + hf[k + 1] + 2) >> 2);
            out[2 * k + 1] = hf[k] + ((out[2 * k] + out[2 * k + 2]) >> 1);
        }
        if (len & 1) {
            out[len - 1] = lf[nl - 1] - ((hf[nh - 1] + 1) >> 1);
            out[len - 2] = hf[nh - 1] + ((out[len - 3] + out[len - 1]) >> 1);
        } else
            out[len - 1] = hf[nh - 1] + out[len - 2];
    }

private:
    // Uniform inverse quantisation (Qpih = 1) and the Fq shift.
    void extract(const Precinct::BandData& d, int width, int line, int32_t* out) const {
        const int gtli = d.gtli;
        const uint16_t* coeff = &d.coeff[size_t(line) * width];
        const uint8_t* gcli = &d.gcli[size_t(line) * ((width + 3) / 4)];
        for (int i = 0; i < width; ++i) {
            uint32_t mag = coeff[i] & 0x7fffu;
            const int g = gcli[i >> 2];
            if (gtli > 0 && g > gtli && mag) {
                const int scale = g - gtli + 1;
                uint32_t v = mag;
                mag = 0;
                while (v) {
                    mag += v;
                    v >>= scale;
                }
            }
            const int32_t s = (coeff[i] & detail::kSignBit) ? -int32_t(mag) : int32_t(mag);
            out[i] = s * (1 << fq_);
        }
    }

    struct Comp {
        int first_band, nlx;
        bool suppressed;
        std::vector<int32_t> hf_prev, even_prev;
    };
    int w_, precincts_, next_, fq_;
    const Topology* t_;
    Sink* sink_;
    Comp comps_[4];
    std::vector<int32_t> lo_, swap_, hi_, hf_;
};

// Decodes one codestream slice by slice, so a caller can poll for
// cancellation between slices. Output rows are written as soon as they are
// final; on error the output is incomplete and must be discarded.
class Decoder {
public:
    Decoder() : stream_(0), len_(0), offset_(0), remaining_(0), cur_(0), next_slice_(0) {}

    // Parse and validate the headers and allocate the working set (a few
    // component rows; the frame itself is never buffered).
    Error open(const uint8_t* stream, size_t len, int width, int height, uint16_t* out,
               size_t pitch) {
        using namespace detail;
        Error e = parse_header(stream, len, h_);
        if (e.status != kOk) return e;
        if (h_.grid_width != width || h_.grid_height != height)
            return invalid("codestream size does not match the raw image");
        if (h_.plane_height() != h_.precinct_count() * h_.precinct_height())
            return unsupported("ragged last precinct");
        if (!out || pitch < size_t(width)) return invalid("bad output buffer");
        try {
            e = build_topology(h_, t_);
            if (e.status != kOk) return e;
            prec_[0].allocate(t_);
            prec_[1].allocate(t_);
        } catch (const std::bad_alloc&) {
            return no_memory();
        }
        if (!tetrix_.init(h_.plane_width(), h_.plane_height(), h_, out, pitch) ||
            !idwt_.init(h_, t_, &tetrix_))
            return no_memory();
        stream_ = stream;
        len_ = h_.lcod;
        offset_ = h_.first_slice;
        remaining_ = h_.precinct_count();
        cur_ = 0;
        next_slice_ = 0;
        return ok();
    }

    const Header& header() const { return h_; }
    int slice_count() const { return h_.slice_count(); }

    // Slices must be decoded in order: the wavelet carries state across them.
    Error decode_next_slice() {
        using namespace detail;
        if (!stream_ || next_slice_ >= h_.slice_count()) return invalid("no slice left");
        const int slice = next_slice_++;
        if (offset_ + 6 > len_ || be16(stream_ + offset_) != marker::SLH ||
            be16(stream_ + offset_ + 2) != 4 || int(be16(stream_ + offset_ + 4)) != slice)
            return invalid("missing or misnumbered slice header");
        offset_ += 6;
        // Vertical GCLI prediction restarts at every slice.
        const Precinct* top = 0;
        const int n = std::min(remaining_, h_.slice_height);
        for (int i = 0; i < n; ++i) {
            size_t next = 0;
            Error e = decode_precinct(stream_, len_, offset_, h_, t_, prec_[cur_], top, next);
            if (e.status != kOk) return e;
            e = idwt_.push(prec_[cur_]);
            if (e.status != kOk) return e;
            top = &prec_[cur_];
            cur_ ^= 1;
            offset_ = next;
            --remaining_;
        }
        return ok();
    }

    Error finish() const {
        using namespace detail;
        if (!stream_ || next_slice_ != h_.slice_count()) return invalid("slices left undecoded");
        if (offset_ + 2 != len_ || be16(stream_ + offset_) != marker::EOC)
            return invalid("missing EOC after the last slice");
        if (!idwt_.complete() || !tetrix_.complete()) return invalid("incomplete frame");
        return ok();
    }

private:
    static Error no_memory() {
        Error e = {kNoMemory, "out of memory"};
        return e;
    }

    Header h_;
    Topology t_;
    Precinct prec_[2];
    StarTetrix tetrix_;
    InverseWavelet<StarTetrix> idwt_;
    const uint8_t* stream_;
    size_t len_, offset_;
    int remaining_, cur_, next_slice_;
};

// Decode a complete HE/HE* codestream into a width x height RGGB mosaic of
// 14-bit values, `pitch` elements per output row.
inline Error decode(const uint8_t* stream, size_t len, int width, int height, uint16_t* out,
                    size_t pitch) {
    Decoder d;
    Error e = d.open(stream, len, width, height, out, pitch);
    for (int s = 0; e.status == kOk && s < d.slice_count(); ++s) e = d.decode_next_slice();
    return e.status == kOk ? d.finish() : e;
}

} // namespace rawlab_nikon_he
