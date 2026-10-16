# Nikon High Efficiency NEF Decoder

`nikon_he_decoder.h` decodes the raw strip of Nikon NEF files recorded with
High Efficiency (HE) or High Efficiency* (HE*) compression. Both modes store
the CFA as a single ISO/IEC 21122 (JPEG XS) codestream written by intoPIX
TicoRAW and differ only in their per-precinct quantisation, so one decoder
covers both. LibRaw 0.22.2 identifies these files but refuses to unpack them.

`lutools/cmake/patch-libraw.cmake` copies the header into the LibRaw source
tree and replaces the unsupported `nikon_he_load_raw()` stub, so every client
that builds the patched LibRaw (macOS, Windows, Android and iOS) gains the
decoder. Unpatched system or Homebrew LibRaw does not.

## Provenance

No Nikon SDK material is used.

- Codestream, packet and entropy layout: a C++ port of
  [dnglab PR #835](https://github.com/dnglab/dnglab/pull/835) by Nicolai
  Buchwitz (`rawler/src/decompressors/jpegxs`, LGPL-2.1).
- Inverse colour transform: ISO/IEC 21122-1 Annex F star-tetrix as implemented
  by [SVT-JPEG-XS](https://github.com/OpenVisualCloud/SVT-JPEG-XS), with the
  component placement and boundary handling of
  [LibRaw PR #826](https://github.com/LibRaw/LibRaw/pull/826) by Dmitri
  Sotnikov, including the bottom-row correction published there by RdWing.
- Transfer curve and its parameters in the picture header extension: RdWing,
  LibRaw PR #826, reported to match Adobe DNG Converter 17.5.1 on Z9, Z8,
  Z6III, Zf and Z5II files.

dnglab's own output stage (chroma gains, a quad-confined transform and a fitted
square-root curve) is not used. On the raw.pixls.us Z9 HE file its ratio to the
same-scene lossless file rises from about 0.9 in highlights to about 1.4 in the
deepest shadows; this decoder stays at 0.85 across the tonal range in all four
CFA channels, the remainder being the exposure change between the two shots.

## License

`nikon_he_decoder.h` is licensed under the GNU Lesser General Public License
version 2.1 (`LGPL-2.1-only`), the license of the dnglab code it ports. It is
compiled into LibRaw, which RawLab uses under LibRaw's LGPL option; see
`LibRaw-LICENSE.LGPL` in each client's license directory. The patched LibRaw
source shipped with a release contains this file.

## Scope and Verification

The decoder accepts only the verified profile: TicoRAW capability marker,
four 14-bit components with component 2 coded without a wavelet, 5/3 wavelet
with one vertical level, uniform quantiser, separate sign sub-packets and the
13-byte picture header extension. Other JPEG XS streams are rejected as
unsupported rather than decoded approximately.

Verified on the CC0 [raw.pixls.us](https://raw.pixls.us/) samples listed in
`lutools/tests/nikon-he-raw-samples.json` (Z8 and Z9 HE and HE*, Zf HE and
HE*): every sub-packet length matches, decoding ends exactly at EOC, the
streaming implementation matches a whole-frame ISO reference bit for bit, and
same-scene lossless files show a flat brightness ratio in all four CFA
channels. No other Nikon model or crop mode has been decoded locally, and the
output has not been compared with Adobe DNG Converter in this repository.
