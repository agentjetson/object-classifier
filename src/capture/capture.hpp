#pragma once

#include "capture/camera_types.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace camera {

class FrameQueue {
public:
  explicit FrameQueue(size_t capacity = 2);

  void push(cv::Mat frame, FrameMeta meta);
  std::optional<std::pair<cv::Mat, FrameMeta>> pop(std::chrono::milliseconds timeout);
  void stop();
  bool stopped() const;

private:
  size_t cap_;
  std::queue<std::pair<cv::Mat, FrameMeta>> q_;
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic<bool> stopped_{false};
};

class Capture {
public:
  struct Config {
    std::string source;
    int         target_width{0};
    int         target_height{0};
    int         queue_capacity{2};
    bool        loop_file{true};
  };

  Capture(Config cfg, std::shared_ptr<FrameQueue> queue);
  ~Capture();

  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

  void start();
  void stop();
  bool is_running() const { return running_.load(); }

  int width()  const { return width_; }
  int height() const { return height_; }

private:
  void capture_loop();

  Config cfg_;
  std::shared_ptr<FrameQueue> queue_;
  cv::VideoCapture cap_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  int width_{0};
  int height_{0};
  int64_t frame_counter_{0};
};

using FrameSink = std::function<void(const cv::Mat&, const FrameMeta&)>;

}  // namespace camera
