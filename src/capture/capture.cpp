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
  if (running_.load()) return;
  stop_requested_ = false;
  // Set running before the worker starts so consumers do not observe a false
  // "not running" window and exit immediately (race with is_running()).
  running_ = true;
  thread_ = std::thread(&Capture::capture_loop, this);
}

void Capture::stop() {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
  if (queue_) queue_->stop();
}

void Capture::capture_loop() {
  // Prefer FFmpeg for files; fall back only for cameras / RTSP
  const bool is_file =
      !cfg_.source.empty() &&
      cfg_.source.find("rtsp") == std::string::npos &&
      cfg_.source.find("/dev/") == std::string::npos &&
      !(cfg_.source.size() == 1 &&
        std::isdigit(static_cast<unsigned char>(cfg_.source[0])));

  if (is_file) {
    // Force the backend that can actually decode most .mp4 files
    if (!cap_.open(cfg_.source, cv::CAP_FFMPEG)) {
      spdlog::error("camera-connector: CAP_FFMPEG open failed for '{}'", cfg_.source);
      // last-ditch fallback
      cap_.open(cfg_.source);
    }
  } else if (cfg_.source.size() == 1 &&
             std::isdigit(static_cast<unsigned char>(cfg_.source[0]))) {
    cap_.open(std::stoi(cfg_.source), cv::CAP_V4L2);
  } else {
    cap_.open(cfg_.source);   // RTSP / other
  }

  if (!cap_.isOpened()) {
    spdlog::error("camera-connector: failed to open source '{}'", cfg_.source);
    running_ = false;
    return;
  }

  // Optional resize
  if (cfg_.target_width > 0 && cfg_.target_height > 0) {
    cap_.set(cv::CAP_PROP_FRAME_WIDTH,  cfg_.target_width);
    cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg_.target_height);
  }

  width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
  height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
  const double frame_count = cap_.get(cv::CAP_PROP_FRAME_COUNT);
  const int backend = static_cast<int>(cap_.get(cv::CAP_PROP_BACKEND));

  spdlog::info("camera-connector: opened '{}' ({}x{}, frames≈{}, backend={})",
               cfg_.source, width_, height_,
               frame_count > 0 ? static_cast<int>(frame_count) : -1,
               backend);

  // ---- critical diagnostic: try the first frame right now ----
  cv::Mat probe;
  if (!cap_.read(probe) || probe.empty()) {
    spdlog::error("camera-connector: first read() failed – "
                  "OpenCV cannot decode this file (missing codec / FFmpeg?)");
    running_ = false;
    return;
  }
  // put the probe frame into the queue so we don't lose it
  {
    FrameMeta meta;
    meta.frame_id   = ++frame_counter_;
    meta.capture_ts = Clock::now();
    meta.width      = probe.cols;
    meta.height     = probe.rows;
    meta.source     = cfg_.source;
    queue_->push(probe.clone(), std::move(meta));
  }

  cv::Mat frame;
  int consecutive_fail = 0;
  while (!stop_requested_) {
    if (!cap_.read(frame) || frame.empty()) {
      ++consecutive_fail;
      if (cfg_.loop_file && is_file && consecutive_fail < 3) {
        spdlog::info("camera-connector: looping file '{}'", cfg_.source);
        cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
        continue;
      }
      spdlog::warn("camera-connector: end of stream or read failure "
                   "(consecutive_fail={})", consecutive_fail);
      break;
    }
    consecutive_fail = 0;

    FrameMeta meta;
    meta.frame_id   = ++frame_counter_;
    meta.capture_ts = Clock::now();
    meta.width      = frame.cols;
    meta.height     = frame.rows;
    meta.source     = cfg_.source;

    queue_->push(frame.clone(), std::move(meta));
  }

  running_ = false;
  spdlog::info("camera-connector: capture loop exited (frames={})", frame_counter_);
}

}  // namespace camera
