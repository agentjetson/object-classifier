#pragma once
// Primary detector backed by agentjetson/rf-detr (ONNX Runtime).
// Same public surface as the former YOLO Detector so ConsumerPipeline /
// Tracker stay unchanged.

#include "types.hpp"

#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace edge_cv {

class Detector {
public:
  struct Config {
    std::string model_path;
    float       conf_thresh  = 0.35f;
    int         max_boxes    = 300;
    // "gpu" | "cpu"  — matches agentjetson/rf-detr
    std::string device       = "gpu";
    // Optional path to a labels file (one class name per line). Empty = COCO-80.
    std::string labels_path;
  };

  explicit Detector(const Config& cfg);
  ~Detector();

  Detector(const Detector&) = delete;
  Detector& operator=(const Detector&) = delete;

  // Inference in original image coordinates (xyxy).
  std::vector<Detection> infer(const cv::Mat& bgr_frame);

  double last_infer_ms() const { return last_infer_ms_; }

  const std::vector<std::string>& class_names() const { return class_names_; }

private:
  Config cfg_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<std::string> class_names_;
  double last_infer_ms_{0.0};
};

}  // namespace edge_cv
