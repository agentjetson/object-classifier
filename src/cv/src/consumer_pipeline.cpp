#include "consumer_pipeline.hpp"
#include "postprocess.hpp"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace edge_cv {

ConsumerPipeline::ConsumerPipeline(Config cfg) : cfg_(std::move(cfg)) {
  detector_ = std::make_unique<Detector>(cfg_.detector);
  tracker_  = std::make_unique<Tracker>();
}

bool ConsumerPipeline::should_emit(const Detection& d) const {
  if (d.confidence < cfg_.min_confidence) return false;
  for (const auto& c : cfg_.emit_classes) {
    if (d.class_name == c) return true;
  }
  return false;
}

void ConsumerPipeline::process_frame(const cv::Mat& frame, const FrameMeta& meta,
                                     bool produce_crops, EmitCallback emit_cb) {
  if (frame.empty()) return;

  auto dets = detector_->infer(frame);
  // Must assign the return value: update takes dets by value, so without
  // this the track_ids never reach the emit loop (existing bug).
  dets = tracker_->update(std::move(dets), frame);

  for (auto& d : dets) {
    if (!should_emit(d)) continue;

    ObjectEvent ev;
    ev.frame_id     = meta.frame_id;
    ev.capture_ts   = meta.capture_ts;
    ev.frame_width  = meta.width;
    ev.frame_height = meta.height;
    ev.detection    = d;

    if (produce_crops) {
      int x1 = std::max(0, static_cast<int>(d.box.x1) - cfg_.crop_pad_px);
      int y1 = std::max(0, static_cast<int>(d.box.y1) - cfg_.crop_pad_px);
      int x2 = std::min(frame.cols, static_cast<int>(d.box.x2) + cfg_.crop_pad_px);
      int y2 = std::min(frame.rows, static_cast<int>(d.box.y2) + cfg_.crop_pad_px);
      if (x2 > x1 && y2 > y1) {
        ev.crop_bgr = frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)).clone();
      }
    }

    emit_cb(ev);
  }
}

}  // namespace edge_cv
