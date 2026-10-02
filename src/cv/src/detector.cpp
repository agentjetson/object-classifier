#include "detector.hpp"

#include "rfdetr_model.hpp"  // agentjetson/rf-detr

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace edge_cv {

namespace {

// COCO 80 (same order as classic YOLO exports / RF-DETR COCO)
const std::vector<std::string> kCocoNames = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
    "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};

std::vector<std::string> load_labels(const std::string& path) {
  std::vector<std::string> names;
  if (path.empty()) return names;
  std::ifstream in(path);
  if (!in) return names;
  std::string line;
  while (std::getline(in, line)) {
    // trim
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    auto start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    names.push_back(line.substr(start));
  }
  return names;
}

}  // namespace

struct Detector::Impl {
  std::unique_ptr<rfdetr::RFDETRModel> model;
};

Detector::Detector(const Config& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>()) {
  auto custom = load_labels(cfg_.labels_path);
  class_names_ = custom.empty() ? kCocoNames : std::move(custom);

  const std::string device = cfg_.device.empty() ? "gpu" : cfg_.device;
  impl_->model = std::make_unique<rfdetr::RFDETRModel>(cfg_.model_path, device);
  impl_->model->warmup();
  fprintf(stderr, "[Detector] RF-DETR loaded: %s  device=%s  classes=%zu\n",
          cfg_.model_path.c_str(), device.c_str(), class_names_.size());
}

Detector::~Detector() = default;

std::vector<Detection> Detector::infer(const cv::Mat& bgr_frame) {
  if (bgr_frame.empty() || !impl_->model) return {};

  auto t0 = Clock::now();

  std::vector<rfdetr::Detection> raw;
  rfdetr::Timings timings;
  impl_->model->predict(bgr_frame, raw, timings, cfg_.conf_thresh, cfg_.max_boxes);

  std::vector<Detection> out;
  out.reserve(raw.size());
  for (const auto& d : raw) {
    Detection e;
    e.class_id   = d.label;
    e.confidence = d.score;
    if (d.label >= 0 && d.label < static_cast<int>(class_names_.size()))
      e.class_name = class_names_[d.label];
    else
      e.class_name = "unknown";

    // rfdetr unnormalizedBox is [x, y, w, h] in pixels
    const auto& b = d.unnormalizedBox;
    e.box.x1 = b.x;
    e.box.y1 = b.y;
    e.box.x2 = b.x + b.width;
    e.box.y2 = b.y + b.height;
    out.push_back(std::move(e));
  }

  auto t1 = Clock::now();
  last_infer_ms_ = DurationMs(t1 - t0).count();
  // Prefer library timing when available
  if (timings.total > 0.f) last_infer_ms_ = timings.total;
  return out;
}

}  // namespace edge_cv
