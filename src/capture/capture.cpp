#include "capture/capture.hpp"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

namespace camera {

// ── FrameQueue ──────────────────────────────────────────────────────────────

FrameQueue::FrameQueue(size_t capacity) : cap_(capacity) {}

void FrameQueue::push(cv::Mat frame, FrameMeta meta) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (stopped_) return;
  while (q_.size() >= cap_) {
    q_.pop();  // drop oldest
  }
  q_.emplace(std::move(frame), std::move(meta));
  cv_.notify_one();
}

std::optional<std::pair<cv::Mat, FrameMeta>> FrameQueue::pop(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mtx_);
  if (!cv_.wait_for(lock, timeout, [this] { return !q_.empty() || stopped_; })) {
    return std::nullopt;
  }
  if (q_.empty()) return std::nullopt;
  auto item = std::move(q_.front());
  q_.pop();
  return item;
}

void FrameQueue::stop() {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    stopped_ = true;
  }
  cv_.notify_all();
}

bool FrameQueue::stopped() const { return stopped_.load(); }

// ── Capture ─────────────────────────────────────────────────────────────────

Capture::Capture(Config cfg, std::shared_ptr<FrameQueue> queue)
    : cfg_(std::move(cfg)), queue_(std::move(queue)) {}

Capture::~Capture() { stop(); }

void Capture::start() {
  if (running_) return;
  stop_requested_ = false;
  thread_ = std::thread(&Capture::capture_loop, this);
}

void Capture::stop() {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
  if (queue_) queue_->stop();
}

void Capture::capture_loop() {
  // Open source
  if (cfg_.source.size() == 1 && std::isdigit(cfg_.source[0])) {
    cap_.open(std::stoi(cfg_.source), cv::CAP_V4L2);
  } else {
    cap_.open(cfg_.source);
  }
  if (!cap_.isOpened()) {
    spdlog::error("camera-connector: failed to open source '{}'", cfg_.source);
    return;
  }

  // Optional resize
  if (cfg_.target_width > 0 && cfg_.target_height > 0) {
    cap_.set(cv::CAP_PROP_FRAME_WIDTH, cfg_.target_width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.target_height);
  }

  width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
  height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
  spdlog::info("camera-connector: opened '{}' ({}x{})", cfg_.source, width_, height_);

  running_ = true;
  cv::Mat frame;
  while (!stop_requested_) {
    if (!cap_.read(frame) || frame.empty()) {
      if (cfg_.loop_file && !cfg_.source.empty() &&
          cfg_.source.find("rtsp") == std::string::npos &&
          cfg_.source.find("/dev/") == std::string::npos) {
        cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
        continue;
      }
      spdlog::warn("camera-connector: end of stream or read failure");
      break;
    }

    FrameMeta meta;
    meta.frame_id   = ++frame_counter_;
    meta.capture_ts = Clock::now();
    meta.width      = frame.cols;
    meta.height     = frame.rows;
    meta.source     = cfg_.source;

    queue_->push(frame.clone(), std::move(meta));
  }
  running_ = false;
  spdlog::info("camera-connector: capture loop exited");
}

}  // namespace camera
