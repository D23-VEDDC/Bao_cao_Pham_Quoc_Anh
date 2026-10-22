// Host parity check for the VEDC fast 1x1 path. It exercises the same TFLM
// reference implementation and requantizer used by the firmware.
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "tensorflow/lite/kernels/internal/reference/integer_ops/conv.h"
#include "tensorflow/lite/micro/kernels/esp_nn/vedc_exact_pointwise.h"

template <int Depth>
bool check_shape(int side, std::mt19937& rng) {
  constexpr int kOutputs = 8;
  const int pixels = side * side;
  std::uniform_int_distribution<int> byte_dist(-128, 127);
  std::uniform_int_distribution<int> offset_dist(-127, 128);
  std::uniform_int_distribution<int> bias_dist(-50000, 50000);
  std::uniform_int_distribution<int> shift_dist(-9, 4);
  std::uniform_int_distribution<int32_t> mult_dist(1 << 25, INT32_MAX);
  std::vector<int8_t> input(pixels * Depth);
  std::array<int8_t, kOutputs * Depth> weights{};
  std::array<int32_t, kOutputs> bias{}, multipliers{}, shifts{};
  std::vector<int8_t> reference(pixels * kOutputs);
  std::vector<int8_t> optimized(pixels * kOutputs);

  for (int trial = 0; trial < 200; ++trial) {
    for (auto& value : input) value = static_cast<int8_t>(byte_dist(rng));
    for (auto& value : weights) value = static_cast<int8_t>(byte_dist(rng));
    for (int oc = 0; oc < kOutputs; ++oc) {
      bias[oc] = bias_dist(rng);
      multipliers[oc] = mult_dist(rng);
      shifts[oc] = shift_dist(rng);
    }
    tflite::ConvParams params{};
    params.input_offset = offset_dist(rng);
    params.output_offset = offset_dist(rng);
    params.stride_width = params.stride_height = 1;
    params.dilation_width_factor = params.dilation_height_factor = 1;
    params.padding_values.width = params.padding_values.height = 0;
    params.quantized_activation_min = (trial & 1) ? -40 : -128;
    params.quantized_activation_max = (trial & 1) ? 80 : 127;
    const int32_t* bias_ptr = (trial % 3 == 0) ? nullptr : bias.data();
    const int32_t input_dims[] = {1, side, side, Depth};
    const int32_t filter_dims[] = {kOutputs, 1, 1, Depth};
    const int32_t bias_dims[] = {kOutputs};
    const int32_t output_dims[] = {1, side, side, kOutputs};
    const tflite::RuntimeShape input_shape(4, input_dims);
    const tflite::RuntimeShape filter_shape(4, filter_dims);
    const tflite::RuntimeShape bias_shape(1, bias_dims);
    const tflite::RuntimeShape output_shape(4, output_dims);
    tflite::reference_integer_ops::ConvPerChannel(
        params, multipliers.data(), shifts.data(), input_shape, input.data(),
        filter_shape, weights.data(), bias_shape, bias_ptr, output_shape,
        reference.data());
    tflite::VedcExactPointwise8<Depth>(
        params, multipliers.data(), shifts.data(), input.data(),
        weights.data(), bias_ptr, pixels, optimized.data());
    if (reference != optimized) {
      for (size_t i = 0; i < reference.size(); ++i) {
        if (reference[i] != optimized[i]) {
          std::fprintf(stderr,
                       "Mismatch depth=%d side=%d trial=%d byte=%zu ref=%d fast=%d\n",
                       Depth, side, trial, i, reference[i], optimized[i]);
          break;
        }
      }
      return false;
    }
  }
  std::printf("PASS depth=%d side=%d trials=200\n", Depth, side);
  return true;
}

int main() {
  std::mt19937 rng(0x51A8D123u);
  return check_shape<16>(32, rng) && check_shape<48>(16, rng) ? 0 : 1;
}
