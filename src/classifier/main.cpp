// object-classifier – primary detection + tracking consumer
//
// Modes:
//   1. --source <path|device>       Standalone test (opens VideoCapture itself)
//   2. --in-process <path|device>   Co-located with camera-connector FrameQueue
//                                   (capture + classify in one process)
//   3. --network-path <frame_addr>  Subscribe to camera-connector FrameService
//                                   (gRPC stream of FrameEnvelope)
//
// Stable output: detection.v1.ObjectEnvelope via IngestService.IngestObject

#include "consumer_pipeline.hpp"
#include "common/env.hpp"
#include "common/otel.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/time_util.h>
#include <spdlog/spdlog.h>

#include "ingest/v1/ingest_service.grpc.pb.h"
#include "detection/v1/detection.pb.h"
#include "capture/v1/frame.pb.h"
#include "capture/v1/frame_service.grpc.pb.h"

// In-process capture (vendored from camera-connector)
#include "capture/capture.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// Decode FrameEnvelope payload → BGR Mat. Returns empty on failure.
cv::Mat decode_frame(const capture::v1::FrameEnvelope& env) {
  const std::string& enc = env.encoding();
  const auto& payload = env.payload();
  if (payload.empty()) {
    spdlog::warn("frame {}: empty payload (ref-only not implemented yet)",
                 env.frame_id());
    return {};
  }

  if (enc == "jpeg" || enc == "jpg") {
    std::vector<uchar> buf(payload.begin(), payload.end());
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
    return img;
  }

  if (enc == "raw_bgr") {
    const int w = env.width();
    const int h = env.height();
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (w <= 0 || h <= 0 || payload.size() < expected) {
      spdlog::warn("frame {}: raw_bgr size mismatch ({} vs {}x{}x3)",
                   env.frame_id(), payload.size(), w, h);
      return {};
    }
    // Clone so the Mat owns its data after the envelope is freed.
    cv::Mat view(h, w, CV_8UC3,
                 const_cast<char*>(payload.data()));
    return view.clone();
  }

  spdlog::warn("frame {}: unsupported encoding '{}'", env.frame_id(), enc);
  return {};
}

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --source <0|video.mp4|rtsp://...> <model.onnx> [ingest_addr]\n"
      << "  " << argv0 << " --in-process <0|video.mp4|rtsp://...> <model.onnx> [ingest_addr]\n"
      << "  " << argv0 << " --network-path <frame_grpc_addr> <model.onnx> [ingest_addr]\n"
      << "\n"
      << "  --source         Standalone test: open VideoCapture inside classifier\n"
      << "  --in-process     Co-located: camera-connector FrameQueue → ConsumerPipeline\n"
      << "  --network-path   Subscribe to camera-connector FrameService (gRPC stream)\n"
      << "  frame_grpc_addr  default localhost:50060 (or FRAME_GRPC_ADDR env)\n"
      << "  ingest_addr      default localhost:50052 (or INGEST_ADDR env)\n"
      << "\n"
      << "Environment:\n"
      << "  FRAME_GRPC_ADDR, INGEST_ADDR, ORT_DEVICE, EDGE_FORCE_CPU, DET_CONF, LABELS_PATH\n"
      << "  FRAME_CAMERA_ID  optional filter passed to SubscribeRequest.camera_id\n"
      << "  FRAME_ENCODING   preferred_encoding for Subscribe (jpeg|raw_bgr)\n";
}

}  // namespace

int main(int argc, char** argv) {
  edge::otel::init("object-classifier", "0.2.0");
  spdlog::set_level(spdlog::level::info);

  if (argc < 4) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string mode = argv[1];
  const std::string arg2 = argv[2];   // source OR frame_grpc_addr
  const std::string model = argv[3];
  const std::string ingest_addr =
      argc > 4 ? argv[4] : edge::getenv_or("INGEST_ADDR", "localhost:50052");

  if (mode != "--source" && mode != "--in-process" && mode != "--network-path") {
    print_usage(argv[0]);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto channel = grpc::CreateChannel(ingest_addr, grpc::InsecureChannelCredentials());
  auto stub = ingest::v1::IngestService::NewStub(channel);

  edge_cv::ConsumerPipeline::Config cfg;
  cfg.detector.model_path = model;
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

  // ── Mode: --network-path (gRPC FrameService client) ─────────────────────
  if (mode == "--network-path") {
    const std::string frame_addr =
        arg2.empty() ? edge::getenv_or("FRAME_GRPC_ADDR", "localhost:50060")
                     : arg2;
    const std::string camera_id = edge::getenv_or("FRAME_CAMERA_ID", "");
    const std::string preferred_enc =
        edge::getenv_or("FRAME_ENCODING", "jpeg");

    spdlog::info(
        "object-classifier [network-path] frame_addr={} model={} ingest={}",
        frame_addr, model, ingest_addr);

    // Reconnect loop so a camera-connector restart does not kill the classifier.
    while (g_running) {
      auto frame_channel =
          grpc::CreateChannel(frame_addr, grpc::InsecureChannelCredentials());
      auto frame_stub = capture::v1::FrameService::NewStub(frame_channel);

      grpc::ClientContext ctx;
      // Cancel the stream when we get SIGINT/SIGTERM.
      std::thread cancel_watch([&ctx] {
        while (g_running) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        ctx.TryCancel();
      });

      capture::v1::SubscribeRequest req;
      if (!camera_id.empty()) req.set_camera_id(camera_id);
      req.set_preferred_encoding(preferred_enc);

      std::unique_ptr<grpc::ClientReader<capture::v1::FrameEnvelope>> reader(
          frame_stub->Subscribe(&ctx, req));

      capture::v1::FrameEnvelope env;
      int frames = 0;
      while (g_running && reader->Read(&env)) {
        cv::Mat frame = decode_frame(env);
        if (frame.empty()) continue;

        edge_cv::FrameMeta meta;
        meta.frame_id = env.frame_id();
        meta.capture_ts = edge_cv::Clock::now();  // approximate; wall clock in proto
        meta.width = env.width() > 0 ? env.width() : frame.cols;
        meta.height = env.height() > 0 ? env.height() : frame.rows;
        meta.source = env.source().empty() ? frame_addr : env.source();
        for (const auto& [k, v] : env.labels()) {
          meta.labels[k] = v;
        }

        pipeline.process_frame(frame, meta, produce_crops,
            [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) {
              emit(ev, meta.source);
            });

        ++frames;
        if (frames % 30 == 0) {
          spdlog::info("network-path processed {} frames (last id={})",
                       frames, env.frame_id());
        }
      }

      auto status = reader->Finish();
      cancel_watch.join();

      if (!g_running) break;
      spdlog::warn("FrameService stream ended: {} — reconnecting in 2s",
                   status.error_message());
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    spdlog::info("object-classifier [network-path] stopped");
    return 0;
  }

  // ── Mode: --in-process (shared FrameQueue) ──────────────────────────────
  if (mode == "--in-process") {
    camera::Capture::Config cap_cfg;
    cap_cfg.source = arg2;
    auto queue = std::make_shared<camera::FrameQueue>(2);
    camera::Capture capture(cap_cfg, queue);
    capture.start();

    spdlog::info("object-classifier [in-process] source={} model={} ingest={}",
                 arg2, model, ingest_addr);

    // Drain until capture has stopped *and* the queue is empty.
    // start() sets running_ early so we do not race-exit before the first frame.
    while (g_running) {
      auto item = queue->pop(std::chrono::milliseconds(500));
      if (!item) {
        if (!capture.is_running()) break;
        continue;
      }

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
            emit(ev, meta.source.empty() ? arg2 : meta.source);
          });
    }

    capture.stop();
    spdlog::info("object-classifier [in-process] stopped");
    return 0;
  }

  // ── Mode: --source (standalone VideoCapture) ────────────────────────────
  cv::VideoCapture cap;
  if (arg2.size() == 1 && std::isdigit(static_cast<unsigned char>(arg2[0]))) {
    cap.open(std::stoi(arg2), cv::CAP_V4L2);
  } else {
    cap.open(arg2);
  }
  if (!cap.isOpened()) {
    spdlog::error("failed to open source '{}'", arg2);
    return 1;
  }

  spdlog::info("object-classifier [source] source={} model={} ingest={}",
               arg2, model, ingest_addr);

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
    meta.source     = arg2;

    pipeline.process_frame(frame, meta, produce_crops,
        [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) {
          emit(ev, arg2);
        });
  }

  spdlog::info("object-classifier [source] stopped");
  return 0;
}
