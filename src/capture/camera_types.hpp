#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <map>

namespace camera {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using DurationMs = std::chrono::duration<double, std::milli>;

struct FrameMeta {
  int64_t     frame_id{0};
  TimePoint   capture_ts;
  int         width{0};
  int         height{0};
  std::string source;
  std::map<std::string, std::string> labels;
};

}  // namespace camera
