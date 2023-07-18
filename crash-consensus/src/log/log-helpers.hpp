#pragma once

#include <array>

class LengthPredictor {
 public:
  LengthPredictor(uint64_t initial_length)
      : sum{W * initial_length}, next_idx{0} {
    std::fill(history.begin(), history.end(), initial_length);
  }

  uint64_t predict() {
    // Round up the average;
    return (sum + W) / W;
  }

  void adjust(uint64_t len) {
    // std::cout << "Adjusting predictor to " << len << std::endl;
    rolling_sum(len);
  }

 private:
  void rolling_sum(uint64_t new_sample) {
    sum -= history[next_idx];
    sum += new_sample;
    history[next_idx] = new_sample;
    next_idx = (next_idx + 1) & mask;
  }

 private:
  uint64_t sum;
  size_t next_idx;
  static constexpr int W = 4;  // Must be power of 2
  static constexpr int mask = W - 1;
  std::array<uint64_t, W> history;
};