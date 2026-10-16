# Keep the released 0.22.2 ABI while backporting narrowly scoped fixes.
if(NOT DEFINED LIBRAW_SOURCE_DIR)
    message(FATAL_ERROR "LIBRAW_SOURCE_DIR is required")
endif()

function(replace_libraw relative before after)
    set(path "${LIBRAW_SOURCE_DIR}/${relative}")
    file(READ "${path}" source)
    string(FIND "${source}" "${after}" patched)
    if(NOT patched EQUAL -1)
        return()
    endif()
    string(FIND "${source}" "${before}" original)
    if(original EQUAL -1)
        message(FATAL_ERROR "Unexpected LibRaw source: ${relative}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${path}" "${source}")
endfunction()

# SDK 的平台和类布局开关必须与调用方一致，不能把 iOS 强制当成 macOS。
replace_libraw(libraw/libraw_datastream.h [=[
#if defined LIBRAW_WIN32_CALLS
#define qWinOS 1
#define qMacOS 0
#elif defined(__APPLE__)
#define qWinOS 0
#define qMacOS 1
#else
/* define OS types for DNG here */
#endif
#define qDNGXMPDocOps 0
#define qDNGUseLibJPEG 1
#define qDNGXMPFiles 0
#define qDNGExperimental 1
#define qDNGThreadSafe 1
]=] [=[
#if !(defined(qMacOS) || defined(qWinOS) || defined(qiPhone) || defined(qAndroid) || defined(qLinux))
#if defined LIBRAW_WIN32_CALLS
#define qWinOS 1
#define qMacOS 0
#elif defined(__APPLE__)
#define qWinOS 0
#define qMacOS 1
#endif
#endif
#ifndef qDNGXMPDocOps
#define qDNGXMPDocOps 0
#endif
#ifndef qDNGUseLibJPEG
#define qDNGUseLibJPEG 1
#endif
#ifndef qDNGXMPFiles
#define qDNGXMPFiles 0
#endif
#ifndef qDNGExperimental
#define qDNGExperimental 1
#endif
#ifndef qDNGThreadSafe
#define qDNGThreadSafe 1
#endif
]=])

# JPEG XL 没有备用解码器，SDK 失败必须中止，不能回落到占位器或使用半解码数据。
file(READ "${LIBRAW_SOURCE_DIR}/src/decoders/unpack.cpp" unpack_source)
string(FIND "${unpack_source}" "if (dng_result == LIBRAW_UNSUFFICIENT_MEMORY)" has_memory_mapping)
if(has_memory_mapping EQUAL -1)
    replace_libraw(src/decoders/unpack.cpp
        "      try_dngsdk();"
        "      const int dng_result = try_dngsdk();\n      if (load_raw == &LibRaw::jxl_dng_load_raw_placeholder && dng_result != LIBRAW_SUCCESS)\n        throw LIBRAW_EXCEPTION_DECODE_RAW;")

    # 兼容已打过旧补丁的源码缓存，并保留 SDK 返回的内存不足类别。
    replace_libraw(src/decoders/unpack.cpp
        "      if (load_raw == &LibRaw::jxl_dng_load_raw_placeholder && dng_result != LIBRAW_SUCCESS)\n        throw LIBRAW_EXCEPTION_DECODE_RAW;"
        "      if (load_raw == &LibRaw::jxl_dng_load_raw_placeholder && dng_result != LIBRAW_SUCCESS)\n      {\n        if (dng_result == LIBRAW_UNSUFFICIENT_MEMORY)\n          throw LIBRAW_EXCEPTION_ALLOC;\n        throw LIBRAW_EXCEPTION_DECODE_RAW;\n      }")
endif()

# Stage2 的像素已经去掉 ActiveArea 外边缘；DefaultCrop 也要换成新图像坐标。
replace_libraw(src/integration/dngsdk_glue.cpp
    "\t\t\tS.raw_width = S.width = stage2->Bounds().W();"
    "\t\t\tif (stageBits & 2)\n\t\t\t{\n\t\t\t\tfor (auto &crop : S.raw_inset_crops)\n\t\t\t\t{\n\t\t\t\t\tif (crop.cleft == 0xffff || crop.ctop == 0xffff)\n\t\t\t\t\t\tcontinue;\n\t\t\t\t\tif (crop.cleft < S.left_margin || crop.ctop < S.top_margin)\n\t\t\t\t\t\tcrop.cleft = crop.ctop = 0xffff;\n\t\t\t\t\telse\n\t\t\t\t\t{\n\t\t\t\t\t\tcrop.cleft -= S.left_margin;\n\t\t\t\t\t\tcrop.ctop -= S.top_margin;\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t\tS.raw_width = S.width = stage2->Bounds().W();")
replace_libraw(src/integration/dngsdk_glue.cpp
    "#include \"dng_host.h\""
    "#include \"dng_host.h\"\n#include \"dng_exceptions.h\"\n#include <new>")
replace_libraw(src/integration/dngsdk_glue.cpp [=[
  catch (...)
  {
    imgdata.process_warnings |= LIBRAW_WARN_DNG_NOT_PROCESSED;
    return LIBRAW_UNSPECIFIED_ERROR;
  }
]=] [=[
  catch (const dng_exception &error)
  {
    imgdata.process_warnings |= LIBRAW_WARN_DNG_NOT_PROCESSED;
    return error.ErrorCode() == dng_error_memory ? LIBRAW_UNSUFFICIENT_MEMORY
                                               : LIBRAW_UNSPECIFIED_ERROR;
  }
  catch (const std::bad_alloc &)
  {
    imgdata.process_warnings |= LIBRAW_WARN_DNG_NOT_PROCESSED;
    return LIBRAW_UNSUFFICIENT_MEMORY;
  }
  catch (...)
  {
    imgdata.process_warnings |= LIBRAW_WARN_DNG_NOT_PROCESSED;
    return LIBRAW_UNSPECIFIED_ERROR;
  }
]=])

configure_file("${CMAKE_CURRENT_LIST_DIR}/../third_party/rawspeed-camera-calibration.inc"
    "${LIBRAW_SOURCE_DIR}/src/tables/rawlab_camera_calibration.inc" COPYONLY)
replace_libraw(src/tables/colordata.cpp
    "  } table[] = {"
    "  } table[] = {\n#include \"rawlab_camera_calibration.inc\"")

# Nikon models must reach the existing JPEG-XS marker check rather than the
# legacy lossless decoder. The named list is kept for older patched sources;
# the prefix check below now routes every Z body by its strip signature.
replace_libraw(src/metadata/tiff.cpp
    "|| !strcmp(model, \"NIKON Z6_3\")) &&"
    "|| !strcmp(model, \"NIKON Z6_3\") || !strcmp(model, \"NIKON Z5_2\") || !strcmp(model, \"NIKON Z50_2\")) &&")

# Nikon High Efficiency (HE/HE*) strips are one JPEG XS codestream. LibRaw only
# recognises them; RawLab's decoder (third_party/nikon-he, LGPL-2.1) replaces the
# unsupported stub. Strips without the SOC + CAP signature keep the lossless
# decoder, and the HE decoder validates the whole header before decoding.
replace_libraw(src/metadata/tiff.cpp
    "else if ((!strncmp(model, \"NIKON Z 9\", 9) ||"
    "else if ((!strncmp(model, \"NIKON Z\", 7) || !strncmp(model, \"NIKON Z 9\", 9) ||")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../third_party/nikon-he/nikon_he_decoder.h"
    "${LIBRAW_SOURCE_DIR}/src/decoders/rawlab_nikon_he_decoder.h" COPYONLY)
# The decoder must precede dcraw_defs.h, whose macros rename common identifiers.
replace_libraw(src/decoders/decoders_libraw_dcrdefs.cpp
    "#include \"../../internal/dcraw_defs.h\""
    "#include \"rawlab_nikon_he_decoder.h\"\n#include \"../../internal/dcraw_defs.h\"")
replace_libraw(src/decoders/decoders_libraw_dcrdefs.cpp [=[
void LibRaw::nikon_he_load_raw()
{
    if(dng_version)
    	throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT; // Never reached
    throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}
]=] [=[
void LibRaw::nikon_he_load_raw()
{
  // RawLab: decode the JPEG XS strip; rows are written straight into raw_image.
  const INT64 strip_bytes = INT64(data_size);
  if (strip_bytes < 64 || strip_bytes > INT64(LIBRAW_MAX_NONDNG_RAW_FILE_SIZE))
    throw LIBRAW_EXCEPTION_IO_CORRUPT;
  std::vector<unsigned char> codestream;
  try
  {
    codestream.resize(size_t(strip_bytes));
  }
  catch (const std::bad_alloc &)
  {
    throw LIBRAW_EXCEPTION_ALLOC;
  }
  fseek(ifp, data_offset, SEEK_SET);
  if (fread(codestream.data(), 1, codestream.size(), ifp) != int(codestream.size()))
    throw LIBRAW_EXCEPTION_IO_EOF;
  rawlab_nikon_he::Decoder decoder;
  rawlab_nikon_he::Error result = decoder.open(codestream.data(), codestream.size(),
                                               raw_width, raw_height, raw_image, raw_width);
  for (int slice = 0; result.status == rawlab_nikon_he::kOk && slice < decoder.slice_count();
       ++slice)
  {
    checkCancel();
    result = decoder.decode_next_slice();
  }
  if (result.status == rawlab_nikon_he::kOk)
    result = decoder.finish();
  if (result.status == rawlab_nikon_he::kNoMemory)
    throw LIBRAW_EXCEPTION_ALLOC;
  if (result.status == rawlab_nikon_he::kUnsupported)
    throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
  if (result.status != rawlab_nikon_he::kOk)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;
}
]=])
replace_libraw(src/utils/decoder_info.cpp
    "    d_info->decoder_name = \"nikon_he_load_raw()\";\n    d_info->decoder_flags = LIBRAW_DECODER_UNSUPPORTED_FORMAT;"
    "    d_info->decoder_name = \"nikon_he_load_raw()\";\n    d_info->decoder_flags = 0;")
foreach(nikon_he_model "Z 6 III" "Z 8" "Z 9" "Z f")
    replace_libraw(src/tables/cameralist.cpp
        "\"Nikon ${nikon_he_model} (HE/HE* formats are not supported yet)\","
        "\"Nikon ${nikon_he_model}\",")
endforeach()

# R6 III ColorData subversion 66 uses the existing v12 offsets, not v11.
# Layout and Adobe-derived matrix: https://github.com/LibRaw/LibRaw/issues/821
# This is a sample-verified local fix, not a claim of upstream camera support.
replace_libraw(src/metadata/canon.cpp [=[
      imCanon.ColorDataVer = 11;
      AsShot_Auto_MeasuredWB(0x0069);
]=] [=[
      imCanon.ColorDataVer = 11;
      AsShot_Auto_MeasuredWB(0x0069);
      if (len == 3778 && imCanon.ColorDataSubVer == 66)
      {
        imCanon.ColorDataVer = 12;
        fseek(ifp, save1 + ((0x006d+0x0001) << 1), SEEK_SET);
        Canon_WBpresets(2, 12);
        fseek(ifp, save1 + ((0x0069+0x00d7) << 1), SEEK_SET);
        Canon_WBCTpresets(0);
        offsetChannelBlackLevel2 = save1 + ((0x0069+0x0116) << 1);
        offsetChannelBlackLevel  = save1 + ((0x0069+0x0227) << 1);
        offsetWhiteLevels        = save1 + ((0x0069+0x022b) << 1);
        break;
      }
]=])
replace_libraw(src/tables/colordata.cpp
    "#include \"rawlab_camera_calibration.inc\""
    "#include \"rawlab_camera_calibration.inc\"\n    { LIBRAW_CAMERAMAKER_Canon, \"EOS R6 Mark III\", 0, 0,\n      { 8309,-1786,-1095,-4851,12736,2353,-956,1911,5534 } },")

# Upstream bcb267e0d53808a7db857c79a21a4f6d7f2388a8: X2D II uses the same crop.
replace_libraw(src/metadata/identify.cpp
    "else if ((imHassy.SensorCode == 20) && imHassy.uncropped)"
    "else if ((imHassy.SensorCode == 20 || imHassy.SensorCode == 22) && imHassy.uncropped)")
replace_libraw(src/tables/cameralist.cpp
    "\"Hasselblad X2D 100C\","
    "\"Hasselblad X2D 100C\",\n    \"Hasselblad X2D II 100C\",")

# X-Trans tiles read overlapping halos but write their results into image.
# Snapshot all source reads so adjacent workers cannot consume developed pixels.
set(xtrans src/demosaic/xtrans_demosaic.cpp)
replace_libraw(${xtrans}
    "#include \"../../internal/dcraw_defs.h\""
    "#include \"../../internal/dcraw_defs.h\"\n#include <memory>")
replace_libraw(${xtrans}
    "  size_t buffer_size = LIBRAW_AHD_TILE"
    "  using SourcePixel = ushort[4];\n  const size_t source_count = size_t(width) * height;\n  std::unique_ptr<SourcePixel[]> source(new SourcePixel[source_count]);\n  memcpy(source.get(), image, source_count * sizeof(SourcePixel));\n  const SourcePixel* source_pixels = source.get();\n\n  size_t buffer_size = LIBRAW_AHD_TILE")
replace_libraw(${xtrans}
    "firstprivate(buffers, allhex, passes, sgrow, sgcol, ndir)"
    "firstprivate(buffers, allhex, passes, sgrow, sgcol, ndir, source_pixels)")
replace_libraw(${xtrans}
    "memcpy(rgb[0][row - top][col - left], image[row * width + col], 6);"
    "memcpy(rgb[0][row - top][col - left], source_pixels[row * width + col], 6);")
replace_libraw(${xtrans}
    "ushort (*pix)[4] = image + row * width + col;"
    "const ushort (*pix)[4] = source_pixels + row * width + col;")
replace_libraw(${xtrans}
    "ushort(*pix)[4] = image + row * width + col;"
    "const ushort(*pix)[4] = source_pixels + row * width + col;")
