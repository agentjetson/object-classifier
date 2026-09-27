#pragma once

#include "types.hpp"
#include "detector.hpp"
#include "tracker.hpp"

#include <opencv2/core.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace edge_cv {

// Clean consumer: takes frames in, emits ObjectEvents (no capture responsibility)
class ConsumerPipeline {
public:
  struct Config {
    Detector::Config detector;
    float            min_confidence{0.35f};
    int              crop_jpeg_quality{80};
    int              crop_pad_px{8};
    std::vector<std::string> emit_classes{
        "person", "car", "truck", "bus", "motorcycle"};
  };

  struct ObjectEvent {
    int64_t     frame_id{0};
    TimePoint   capture_ts;
    int         frame_width{0};
    int         frame_height{0};
    Detection   detection;
    cv::Mat     crop_bgr;   // optional, filled when requested
  };

  using EmitCallback = std::function<void(const ObjectEvent&)>;

  explicit ConsumerPipeline(Config cfg);
  ~ConsumerPipeline() = default;

  // Process one frame; calls emit_cb for every emitted object
  void process_frame(const cv::Mat& frame, const FrameMeta& meta,
                     bool produce_crops, EmitCallback emit_cb);

  bool should_emit(const Detection& d) const;

private:
  Config cfg_;
  std::unique_ptr<Detector> detector_;
  std::unique_ptr<Tracker>  tracker_;
};

}  // namespace edge_cv
