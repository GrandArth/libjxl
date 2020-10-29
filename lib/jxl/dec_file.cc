// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/dec_file.h"

#include <stddef.h>

#include <utility>
#include <vector>

#include "jxl/decode.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/color_management.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/dec_frame.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/icc_codec.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/multiframe.h"

namespace jxl {
namespace {

Status DecodePreview(const DecompressParams& dparams,
                     BitReader* JXL_RESTRICT reader,
                     const Span<const uint8_t> file, AuxOut* aux_out,
                     ThreadPool* pool, CodecInOut* JXL_RESTRICT io) {
  // No preview present in file.
  if (!io->metadata.m2.have_preview) {
    if (dparams.preview == Override::kOn) {
      return JXL_FAILURE("preview == kOn but no preview present");
    }
    return true;
  }

  // Have preview; prepare to skip or read it.
  JXL_RETURN_IF_ERROR(reader->JumpToByteBoundary());
  FrameDimensions frame_dim;
  frame_dim.Set(io->preview.xsize(), io->preview.ysize(),
                /*group_size_shift=*/1, /*max_hshift=*/0, /*max_vshift=*/0);

  const AnimationHeader* animation = nullptr;
  if (dparams.preview == Override::kOff) {
    JXL_RETURN_IF_ERROR(
        SkipFrame(file, animation, &io->metadata, &frame_dim, reader));
    return true;
  }

  // Else: default or kOn => decode preview.
  Multiframe multiframe;
  JXL_RETURN_IF_ERROR(DecodeFrame(dparams, file, animation, &frame_dim,
                                  &multiframe, pool, reader, aux_out,
                                  &io->preview_frame, io));
  io->dec_pixels += frame_dim.xsize_upsampled * frame_dim.ysize_upsampled;
  return true;
}

Status DecodeHeaders(BitReader* reader, size_t* JXL_RESTRICT xsize,
                     size_t* JXL_RESTRICT ysize, CodecInOut* io) {
  SizeHeader size;
  JXL_RETURN_IF_ERROR(ReadSizeHeader(reader, &size));
  *xsize = size.xsize();
  *ysize = size.ysize();

  JXL_RETURN_IF_ERROR(ReadImageMetadata(reader, &io->metadata));

  if (io->metadata.m2.have_preview) {
    JXL_RETURN_IF_ERROR(ReadPreviewHeader(reader, &io->preview));
  }

  if (io->metadata.m2.have_animation) {
    JXL_RETURN_IF_ERROR(ReadAnimationHeader(reader, &io->animation));
  }

  return true;
}

}  // namespace

// To avoid the complexity of file I/O and buffering, we assume the bitstream
// is loaded (or for large images/sequences: mapped into) memory.
Status DecodeFile(const DecompressParams& dparams,
                  const Span<const uint8_t> file, CodecInOut* JXL_RESTRICT io,
                  AuxOut* aux_out, ThreadPool* pool) {
  PROFILER_ZONE("DecodeFile uninstrumented");

  // Marker
  JxlSignature signature = JxlSignatureCheck(file.data(), file.size());
  if (signature == JXL_SIG_NOT_ENOUGH_BYTES || signature == JXL_SIG_INVALID) {
    return JXL_FAILURE("File does not start with known JPEG XL signature");
  }

  std::unique_ptr<brunsli::JPEGData> jpeg_data = nullptr;
  if (dparams.keep_dct) {
    if (io->Main().jpeg_data == nullptr) {
      return JXL_FAILURE("Caller must set jpeg_data");
    }
    jpeg_data = std::move(io->Main().jpeg_data);
  }

  Status ret = true;
  {
    BitReader reader(file);
    BitReaderScopedCloser reader_closer(&reader, &ret);
    (void)reader.ReadFixedBits<16>();  // skip marker

    FrameDimensions main_frame_dim;
    {
      size_t xsize, ysize;
      JXL_RETURN_IF_ERROR(DecodeHeaders(&reader, &xsize, &ysize, io));
      JXL_RETURN_IF_ERROR(io->VerifyDimensions(xsize, ysize));
      main_frame_dim.Set(xsize, ysize, /*group_size_shift=*/1,
                         /*max_hshift=*/0,
                         /*max_vshift=*/0);
    }

    if (io->metadata.color_encoding.WantICC()) {
      PaddedBytes icc;
      JXL_RETURN_IF_ERROR(ReadICC(&reader, &icc));
      JXL_RETURN_IF_ERROR(io->metadata.color_encoding.SetICC(std::move(icc)));
    }

    JXL_RETURN_IF_ERROR(
        DecodePreview(dparams, &reader, file, aux_out, pool, io));

    // Only necessary if no ICC and no preview.
    JXL_RETURN_IF_ERROR(reader.JumpToByteBoundary());
    if (io->metadata.m2.have_animation && dparams.keep_dct) {
      return JXL_FAILURE("Cannot decode to JPEG an animation");
    }

    if (io->metadata.bit_depth.floating_point_sample &&
        !io->metadata.xyb_encoded) {
      io->dec_target = DecodeTarget::kLosslessFloat;
    }

    Multiframe multiframe;

    io->frames.clear();
    io->animation_frames.clear();
    if (io->metadata.m2.have_animation) {
      do {
        io->animation_frames.emplace_back(&io->metadata);
        io->frames.emplace_back(&io->metadata);
        FrameDimensions frame_dim;
        // Skip frames that are not displayed.
        do {
          frame_dim = main_frame_dim;
          JXL_RETURN_IF_ERROR(DecodeFrame(dparams, file, &io->animation,
                                          &frame_dim, &multiframe, pool,
                                          &reader, aux_out, &io->frames.back(),
                                          io, &io->animation_frames.back()));
        } while (!multiframe.IsDisplayed());
        io->dec_pixels += frame_dim.xsize_upsampled * frame_dim.ysize_upsampled;
      } while (!io->animation_frames.back().is_last);
    } else {
      io->frames.emplace_back(&io->metadata);
      io->frames.back().jpeg_data = std::move(jpeg_data);
      FrameDimensions frame_dim;
      // Skip frames that are not displayed.
      do {
        frame_dim = main_frame_dim;
        JXL_RETURN_IF_ERROR(
            DecodeFrame(dparams, file, /*animation_or_null=*/nullptr,
                        &frame_dim, &multiframe, pool, &reader, aux_out,
                        &io->frames.back(), io, /*animation=*/nullptr));
      } while (!multiframe.IsDisplayed());
      io->dec_pixels += frame_dim.xsize_upsampled * frame_dim.ysize_upsampled;
    }

    if (dparams.check_decompressed_size && dparams.max_downsampling == 1) {
      if (reader.TotalBitsConsumed() != file.size() * kBitsPerByte) {
        return JXL_FAILURE("DecodeFile reader position not at EOF.");
      }
    }

    io->CheckMetadata();
    // reader is closed here.
  }
  return ret;
}

}  // namespace jxl