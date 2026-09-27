// object-classifier – primary detection + tracking consumer
//
// Modes:
//   1. --source <path|device>     Standalone test (opens VideoCapture itself)
//   2. --in-process <path|device> Co-located with camera-connector FrameQueue
//                                 (capture + classify in one process)
//
// Stable output: detection.v1.ObjectEnvelope via IngestService.IngestObject

#include "consumer_pipeline.hpp"
#include "common/env.hpp"
#include "common/otel.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/time_util.h>
#include <spdlog/spdlog.h>

#include "ingest/v1/ingest_service.grpc.pb.h"
#include "detection/v1/detection.pb.h"

// In-process capture (vendored from camera-connector)
#include "capture/capture.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

detection::v1::ObjectEnvelope to_proto(
    const edge_cv::ConsumerPipeline::ObjectEvent& ev,
    const std::string& source,
    int jpeg_quality) {
  detection::v1::ObjectEnvelope out;
  out.set_frame_id(ev.frame_id);
  *out.mutable_timestamp() = google::protobuf::util::TimeUtil::GetCurrentTime();
  out.set_source(source);
  out.set_class_name(ev.detection.class_name);
  out.set_class_id(ev.detection.class_id);
  out.set_confidence(ev.detection.confidence);
  out.set_track_id(ev.detection.track_id);
  out.set_frame_width(ev.frame_width);
  out.set_frame_height(ev.frame_height);

  auto* box = out.mutable_box();
  box->set_x1(ev.detection.box.x1);
  box->set_y1(ev.detection.box.y1);
  box->set_x2(ev.detection.box.x2);
  box->set_y2(ev.detection.box.y2);

  if (!ev.crop_bgr.empty()) {
    std::vector<uchar> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpeg_quality};
    if (cv::imencode(".jpg", ev.crop_bgr, buf, params)) {
      out.set_crop_jpeg(buf.data(), buf.size());
    }
  }

  auto now = edge_cv::Clock::now();
  out.set_capture_latency_ms(
      edge_cv::DurationMs(now - ev.capture_ts).count());
  return out;
}

void publish(ingest::v1::IngestService::Stub* stub,
             const detection::v1::ObjectEnvelope& env) {
  grpc::ClientContext ctx;
  ingest::v1::IngestObjectRequest req;
  *req.mutable_object() = env;
  ingest::v1::IngestObjectResponse resp;
  auto status = stub->IngestObject(&ctx, req, &resp);
  if (!status.ok()) {
    spdlog::warn("IngestObject failed: {}", status.error_message());
  }
}

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --source <0|video.mp4|rtsp://...> <rf-detr.onnx> [ingest_addr]\n"
      << "  " << argv0 << " --in-process <0|video.mp4|rtsp://...> <rf-detr.onnx> [ingest_addr]\n"
      << "\n"
      << "  --source      Standalone test: open VideoCapture inside classifier\n"
      << "  --in-process  Co-located: camera-connector FrameQueue → ConsumerPipeline\n"
      << "  ingest_addr   default localhost:50052 (or INGEST_ADDR env)\n";
}

}  // namespace

int main(int argc, char** argv) {
  edge::otel::init("object-classifier", "0.1.0");
  spdlog::set_level(spdlog::level::info);

  if (argc < 4) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string mode = argv[1];
  const std::string source = argv[2];
  const std::string model = argv[3];
  const std::string ingest_addr =
      argc > 4 ? argv[4] : edge::getenv_or("INGEST_ADDR", "localhost:50052");

  if (mode != "--source" && mode != "--in-process") {
    print_usage(argv[0]);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto channel = grpc::CreateChannel(ingest_addr, grpc::InsecureChannelCredentials());
  auto stub = ingest::v1::IngestService::NewStub(channel);

  edge_cv::ConsumerPipeline::Config cfg;
  cfg.detector.model_path = model;
  // agentjetson/rf-detr uses "gpu" | "cpu"
  const bool force_cpu = edge::getenv_or("EDGE_FORCE_CPU", "0") == "1";
  cfg.detector.device = force_cpu
      ? "cpu"
      : edge::getenv_or("ORT_DEVICE", "gpu");
  cfg.detector.conf_thresh = std::stof(edge::getenv_or("DET_CONF", "0.35"));
  if (const char* labels = std::getenv("LABELS_PATH"); labels && *labels)
    cfg.detector.labels_path = labels;
  edge_cv::ConsumerPipeline pipeline(cfg);

  const bool produce_crops = true;
  int jpeg_quality = cfg.crop_jpeg_quality;

  auto emit = [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev,
                  const std::string& src) {
    auto env = to_proto(ev, src, jpeg_quality);
    publish(stub.get(), env);
    if (ev.frame_id % 30 == 0) {
      spdlog::info("emitted {} track={} conf={:.2f}",
                   ev.detection.class_name, ev.detection.track_id,
                   ev.detection.confidence);
    }
  };

  // ── Mode: --in-process (shared FrameQueue) ──────────────────────────────
  if (mode == "--in-process") {
    camera::Capture::Config cap_cfg;
    cap_cfg.source = source;
    auto queue = std::make_shared<camera::FrameQueue>(2);
    camera::Capture capture(cap_cfg, queue);
    capture.start();

    spdlog::info("object-classifier [in-process] source={} model={} ingest={}",
                 source, model, ingest_addr);

    while (g_running && capture.is_running()) {
      auto item = queue->pop(std::chrono::milliseconds(500));
      if (!item) continue;

      auto& [frame, cam_meta] = *item;

      edge_cv::FrameMeta meta;
      meta.frame_id   = cam_meta.frame_id;
      meta.capture_ts = cam_meta.capture_ts;
      meta.width      = cam_meta.width;
      meta.height     = cam_meta.height;
      meta.source     = cam_meta.source;
      meta.labels     = cam_meta.labels;

      pipeline.process_frame(frame, meta, produce_crops,
          [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) {
            emit(ev, meta.source.empty() ? source : meta.source);
          });
    }

    capture.stop();
    spdlog::info("object-classifier [in-process] stopped");
    return 0;
  }

  // ── Mode: --source (standalone VideoCapture) ────────────────────────────
  cv::VideoCapture cap;
  if (source.size() == 1 && std::isdigit(static_cast<unsigned char>(source[0]))) {
    cap.open(std::stoi(source), cv::CAP_V4L2);
  } else {
    cap.open(source);
  }
  if (!cap.isOpened()) {
    spdlog::error("failed to open source '{}'", source);
    return 1;
  }

  spdlog::info("object-classifier [source] source={} model={} ingest={}",
               source, model, ingest_addr);

  cv::Mat frame;
  int64_t frame_id = 0;
  while (g_running) {
    if (!cap.read(frame) || frame.empty()) break;
    ++frame_id;

    edge_cv::FrameMeta meta;
    meta.frame_id   = frame_id;
    meta.capture_ts = edge_cv::Clock::now();
    meta.width      = frame.cols;
    meta.height     = frame.rows;
    meta.source     = source;

    pipeline.process_frame(frame, meta, produce_crops,
        [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) {
          emit(ev, source);
        });
  }

  spdlog::info("object-classifier [source] stopped");
  return 0;
}
