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

#ifndef LIB_JXL_PASSES_STATE_H_
#define LIB_JXL_PASSES_STATE_H_

#include "lib/jxl/ac_context.h"
#include "lib/jxl/ac_strategy.h"
#include "lib/jxl/chroma_from_luma.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dot_dictionary.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/image.h"
#include "lib/jxl/image_bundle.h"
#include "lib/jxl/loop_filter.h"
#include "lib/jxl/multiframe.h"
#include "lib/jxl/noise.h"
#include "lib/jxl/patch_dictionary.h"
#include "lib/jxl/quant_weights.h"
#include "lib/jxl/quantizer.h"
#include "lib/jxl/splines.h"

// Structures that hold the (en/de)coder state for a JPEG XL kVarDCT
// (en/de)coder.

namespace jxl {

struct ImageFeatures {
  LoopFilter loop_filter;
  NoiseParams noise_params;
  PatchDictionary patches;
  Splines splines;
};

// State common to both encoder and decoder.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct PassesSharedState {
  // TODO(lode): must be able to set metadata from a CodecInOut to the
  // frame_header instead?
  PassesSharedState() : frame_header(&metadata) {}

  // Headers and metadata.
  ImageMetadata metadata;
  FrameHeader frame_header;

  FrameDimensions frame_dim;

  // Control fields and parameters.
  AcStrategyImage ac_strategy;

  // Dequant matrices + quantizer.
  DequantMatrices matrices;
  Quantizer quantizer{&matrices};
  ImageI raw_quant_field;

  // Per-block side information for EPF detail preservation.
  ImageB epf_sharpness;

  ColorCorrelationMap cmap;

  OpsinParams opsin_params;

  ImageFeatures image_features;

  // Memory area for storing coefficient orders.
  std::vector<coeff_order_t> coeff_orders =
      std::vector<coeff_order_t>(kMaxNumPasses * kCoeffOrderSize);

  // Decoder-side DC and quantized DC.
  ImageB quant_dc;
  Image3F dc_storage;
  const Image3F* JXL_RESTRICT dc = &dc_storage;

  BlockCtxMap block_ctx_map;

  Multiframe* JXL_RESTRICT multiframe = nullptr;

  // Number of pre-clustered set of histograms (with the same ctx map), per
  // pass. Encoded as num_histograms_ - 1.
  size_t num_histograms = 0;

  bool IsGrayscale() const { return metadata.color_encoding.IsGray(); }

  Rect GroupRect(size_t group_index) const {
    const size_t gx = group_index % frame_dim.xsize_groups;
    const size_t gy = group_index / frame_dim.xsize_groups;
    const Rect rect(gx * frame_dim.group_dim, gy * frame_dim.group_dim,
                    frame_dim.group_dim, frame_dim.group_dim, frame_dim.xsize,
                    frame_dim.ysize);
    return rect;
  }

  Rect PaddedGroupRect(size_t group_index) const {
    const size_t gx = group_index % frame_dim.xsize_groups;
    const size_t gy = group_index / frame_dim.xsize_groups;
    const Rect rect(gx * frame_dim.group_dim, gy * frame_dim.group_dim,
                    frame_dim.group_dim, frame_dim.group_dim,
                    frame_dim.xsize_padded, frame_dim.ysize_padded);
    return rect;
  }

  Rect BlockGroupRect(size_t group_index) const {
    const size_t gx = group_index % frame_dim.xsize_groups;
    const size_t gy = group_index / frame_dim.xsize_groups;
    const Rect rect(gx * (frame_dim.group_dim >> 3),
                    gy * (frame_dim.group_dim >> 3), frame_dim.group_dim >> 3,
                    frame_dim.group_dim >> 3, frame_dim.xsize_blocks,
                    frame_dim.ysize_blocks);
    return rect;
  }

  Rect DCGroupRect(size_t group_index) const {
    const size_t gx = group_index % frame_dim.xsize_dc_groups;
    const size_t gy = group_index / frame_dim.xsize_dc_groups;
    const Rect rect(gx * frame_dim.group_dim, gy * frame_dim.group_dim,
                    frame_dim.group_dim, frame_dim.group_dim,
                    frame_dim.xsize_blocks, frame_dim.ysize_blocks);
    return rect;
  }
};

// Initialized the state information that is shared between encoder and decoder.
Status InitializePassesSharedState(const FrameHeader& frame_header,
                                   const LoopFilter& loop_filter,
                                   const ImageMetadata& image_metadata,
                                   const FrameDimensions& frame_dim,
                                   Multiframe* JXL_RESTRICT multiframe,
                                   PassesSharedState* JXL_RESTRICT shared,
                                   bool encoder = false);

}  // namespace jxl

#endif  // LIB_JXL_PASSES_STATE_H_