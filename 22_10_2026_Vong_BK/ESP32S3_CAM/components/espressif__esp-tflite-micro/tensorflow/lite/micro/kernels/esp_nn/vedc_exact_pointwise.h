/*
 * VEDC 1x1 int8 convolution for the three model layers that cannot use the
 * current ESP-NN implementation. Keep the reference kernel's accumulation,
 * per-channel requantization and clamping exactly; only remove its general
 * convolution indexing and reuse each input value across eight outputs.
 */
#ifndef VEDC_EXACT_POINTWISE_H_
#define VEDC_EXACT_POINTWISE_H_

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "tensorflow/lite/kernels/internal/common.h"

namespace tflite {

template <int InputDepth>
inline void VedcExactPointwise8(const ConvParams& params,
                                const int32_t* output_multiplier,
                                const int32_t* output_shift,
                                const int8_t* input_data,
                                const int8_t* filter_data,
                                const int32_t* bias_data,
                                int pixels,
                                int8_t* output_data) {
  static_assert(InputDepth == 16 || InputDepth == 48,
                "Only the verified VEDC 1x1 channel counts are supported");

  // The small filter and quantization arrays fit on the worker's stack. Their
  // source tensors may live in flash/PSRAM, so copy them only once per Invoke.
  int8_t weights[8][InputDepth];
  int32_t multipliers[8];
  int32_t shifts[8];
  int32_t biases[8];
  for (int oc = 0; oc < 8; ++oc) {
    std::memcpy(weights[oc], filter_data + oc * InputDepth, InputDepth);
    multipliers[oc] = output_multiplier[oc];
    shifts[oc] = output_shift[oc];
    biases[oc] = bias_data ? bias_data[oc] : 0;
  }

  for (int pixel = 0; pixel < pixels; ++pixel) {
    const int8_t* input = input_data + pixel * InputDepth;
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int32_t a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    for (int ic = 0; ic < InputDepth; ++ic) {
      const int32_t value = static_cast<int32_t>(input[ic]) + params.input_offset;
      a0 += value * weights[0][ic];
      a1 += value * weights[1][ic];
      a2 += value * weights[2][ic];
      a3 += value * weights[3][ic];
      a4 += value * weights[4][ic];
      a5 += value * weights[5][ic];
      a6 += value * weights[6][ic];
      a7 += value * weights[7][ic];
    }
    int32_t sums[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    for (int oc = 0; oc < 8; ++oc) {
      int32_t acc = sums[oc] + biases[oc];
      acc = MultiplyByQuantizedMultiplier(acc, multipliers[oc], shifts[oc]);
      acc += params.output_offset;
      acc = std::max(acc, params.quantized_activation_min);
      acc = std::min(acc, params.quantized_activation_max);
      output_data[pixel * 8 + oc] = static_cast<int8_t>(acc);
    }
  }
}

}  // namespace tflite

#endif  // VEDC_EXACT_POINTWISE_H_
