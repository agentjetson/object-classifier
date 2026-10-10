// object-classifier – primary detection + tracking consumer
//
// Publish path (ObjectEnvelope):
//   PUBLISH_MODE=ingest  (default)  → IngestService.IngestObject gRPC
//   PUBLISH_MODE=nats               → JetStream cv.object.<class> (bypass ingest)
//   PUBLISH_MODE=both               → ingest + nats

#include "consumer_pipeline.hpp"

#include <aj/edge/env.hpp>
#include <aj/edge/nats_publisher.hpp>
#include <aj/edge/otel.hpp>
#include <aj/edge/signal.hpp>

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
#include "capture/capture.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

detection::v1::ObjectEnvelope to_proto(
    const edge_cv::ConsumerPipeline::ObjectEvent& ev,
    const std::string& source, int jpeg_quality) {
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
    if (cv::imencode(".jpg", ev.crop_bgr, buf, params))
      out.set_crop_jpeg(buf.data(), buf.size());
  }
  auto now = edge_cv::Clock::now();
  out.set_capture_latency_ms(edge_cv::DurationMs(now - ev.capture_ts).count());
  return out;
}

enum class PublishMode { Ingest, Nats, Both };

PublishMode parse_publish_mode(const std::string& s) {
  if (s == "nats" || s == "jetstream" || s == "js") return PublishMode::Nats;
  if (s == "both" || s == "all") return PublishMode::Both;
  return PublishMode::Ingest;
}

struct Emitter {
  PublishMode mode{PublishMode::Ingest};
  std::unique_ptr<ingest::v1::IngestService::Stub> ingest_stub;
  std::unique_ptr<aj::edge::NatsPublisher> nats;
  int jpeg_quality{80};

  void emit(const edge_cv::ConsumerPipeline::ObjectEvent& ev,
            const std::string& src) {
    auto env = to_proto(ev, src, jpeg_quality);
    const bool want_ingest = mode == PublishMode::Ingest || mode == PublishMode::Both;
    const bool want_nats = mode == PublishMode::Nats || mode == PublishMode::Both;

    if (want_ingest && ingest_stub) {
      grpc::ClientContext ctx;
      ingest::v1::IngestObjectRequest req;
      *req.mutable_object() = env;
      ingest::v1::IngestObjectResponse resp;
      auto status = ingest_stub->IngestObject(&ctx, req, &resp);
      if (!status.ok())
        spdlog::warn("IngestObject failed: {}", status.error_message());
    }

    if (want_nats && nats) {
      std::string bytes;
      if (!env.SerializeToString(&bytes)) {
        spdlog::warn("ObjectEnvelope SerializeToString failed");
      } else {
        const auto subj = aj::edge::NatsPublisher::object_subject(env.class_name());
        if (!nats->publish(subj, bytes))
          spdlog::warn("NATS publish {} failed", subj);
      }
    }

    if (ev.frame_id % 30 == 0) {
      spdlog::info("emitted {} track={} conf={:.2f} mode={}",
                   ev.detection.class_name, ev.detection.track_id,
                   ev.detection.confidence,
                   mode == PublishMode::Nats ? "nats"
                   : mode == PublishMode::Both ? "both" : "ingest");
    }
  }
};

cv::Mat decode_frame(const capture::v1::FrameEnvelope& env) {
  const std::string& enc = env.encoding();
  const auto& payload = env.payload();
  if (payload.empty()) return {};
  if (enc == "jpeg" || enc == "jpg") {
    std::vector<uchar> buf(payload.begin(), payload.end());
    return cv::imdecode(buf, cv::IMREAD_COLOR);
  }
  if (enc == "raw_bgr") {
    const int w = env.width(), h = env.height();
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (w <= 0 || h <= 0 || payload.size() < expected) return {};
    cv::Mat view(h, w, CV_8UC3, const_cast<char*>(payload.data()));
    return view.clone();
  }
  return {};
}

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --source|--in-process|--network-path ... <model.onnx> [ingest_addr]\n"
      << "Publish: PUBLISH_MODE=ingest|nats|both  NATS_URL=nats://127.0.0.1:4222\n";
}

}  // namespace

int main(int argc, char** argv) {
  aj::edge::otel::init(
      aj::edge::getenv_or("OTEL_SERVICE_NAME", "object-classifier"), "0.3.0");
  aj::edge::install_stop_handlers();

  if (argc < 4) { print_usage(argv[0]); aj::edge::otel::shutdown(); return 1; }

  const std::string mode = argv[1];
  const std::string arg2 = argv[2];
  const std::string model = argv[3];
  const std::string ingest_addr =
      argc > 4 ? argv[4] : aj::edge::getenv_or("INGEST_ADDR", "localhost:50052");

  if (mode != "--source" && mode != "--in-process" && mode != "--network-path") {
    print_usage(argv[0]); aj::edge::otel::shutdown(); return 1;
  }

  Emitter emitter;
  emitter.mode = parse_publish_mode(aj::edge::getenv_or("PUBLISH_MODE", "ingest"));
  emitter.jpeg_quality = 80;

  if (emitter.mode == PublishMode::Ingest || emitter.mode == PublishMode::Both) {
    auto channel = grpc::CreateChannel(ingest_addr, grpc::InsecureChannelCredentials());
    emitter.ingest_stub = ingest::v1::IngestService::NewStub(channel);
  }

  if (emitter.mode == PublishMode::Nats || emitter.mode == PublishMode::Both) {
    aj::edge::NatsPublisher::Config ncfg;
    ncfg.url = aj::edge::getenv_or("NATS_URL", "nats://127.0.0.1:4222");
    ncfg.client_name = aj::edge::getenv_or("NATS_CLIENT_NAME", "object-classifier");
    ncfg.stream = aj::edge::getenv_or("NATS_STREAM", "CV_EVENTS");
    emitter.nats = std::make_unique<aj::edge::NatsPublisher>(std::move(ncfg));
    if (!emitter.nats->connect()) {
      spdlog::error("NATS connect failed");
      if (emitter.mode == PublishMode::Nats) {
        aj::edge::otel::shutdown(); return 1;
      }
      emitter.nats.reset();
      emitter.mode = PublishMode::Ingest;
    }
  }

  edge_cv::ConsumerPipeline::Config cfg;
  cfg.detector.model_path = model;
  const bool force_cpu = aj::edge::getenv_or("EDGE_FORCE_CPU", "0") == "1";
  cfg.detector.device = force_cpu ? "cpu" : aj::edge::getenv_or("ORT_DEVICE", "gpu");
  cfg.detector.conf_thresh = std::stof(aj::edge::getenv_or("DET_CONF", "0.35"));
  if (const char* labels = std::getenv("LABELS_PATH"); labels && *labels)
    cfg.detector.labels_path = labels;
  edge_cv::ConsumerPipeline pipeline(cfg);
  const bool produce_crops = true;
  emitter.jpeg_quality = cfg.crop_jpeg_quality;

  auto emit = [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev,
                  const std::string& src) { emitter.emit(ev, src); };

  if (mode == "--network-path") {
    const std::string frame_addr =
        arg2.empty() ? aj::edge::getenv_or("FRAME_GRPC_ADDR", "localhost:50060") : arg2;
    const std::string camera_id = aj::edge::getenv_or("FRAME_CAMERA_ID", "");
    const std::string preferred_enc = aj::edge::getenv_or("FRAME_ENCODING", "jpeg");
    spdlog::info("object-classifier [network-path] frame={} model={} publish={}",
                 frame_addr, model, aj::edge::getenv_or("PUBLISH_MODE", "ingest"));
    while (aj::edge::running()) {
      auto frame_channel = grpc::CreateChannel(frame_addr, grpc::InsecureChannelCredentials());
      auto frame_stub = capture::v1::FrameService::NewStub(frame_channel);
      grpc::ClientContext ctx;
      std::thread cancel_watch([&ctx] {
        while (aj::edge::running()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ctx.TryCancel();
      });
      capture::v1::SubscribeRequest req;
      if (!camera_id.empty()) req.set_camera_id(camera_id);
      req.set_preferred_encoding(preferred_enc);
      std::unique_ptr<grpc::ClientReader<capture::v1::FrameEnvelope>> reader(
          frame_stub->Subscribe(&ctx, req));
      capture::v1::FrameEnvelope env;
      while (aj::edge::running() && reader->Read(&env)) {
        cv::Mat frame = decode_frame(env);
        if (frame.empty()) continue;
        edge_cv::FrameMeta meta;
        meta.frame_id = env.frame_id();
        meta.capture_ts = edge_cv::Clock::now();
        meta.width = env.width() > 0 ? env.width() : frame.cols;
        meta.height = env.height() > 0 ? env.height() : frame.rows;
        meta.source = env.source().empty() ? frame_addr : env.source();
        for (const auto& [k, v] : env.labels()) meta.labels[k] = v;
        pipeline.process_frame(frame, meta, produce_crops,
            [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) { emit(ev, meta.source); });
      }
      reader->Finish();
      cancel_watch.join();
      if (!aj::edge::running()) break;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    aj::edge::otel::shutdown();
    return 0;
  }

  if (mode == "--in-process") {
    camera::Capture::Config cap_cfg;
    cap_cfg.source = arg2;
    auto queue = std::make_shared<camera::FrameQueue>(2);
    camera::Capture capture(cap_cfg, queue);
    capture.start();
    while (aj::edge::running()) {
      auto item = queue->pop(std::chrono::milliseconds(500));
      if (!item) { if (!capture.is_running()) break; continue; }
      auto& [frame, cam_meta] = *item;
      edge_cv::FrameMeta meta;
      meta.frame_id = cam_meta.frame_id;
      meta.capture_ts = cam_meta.capture_ts;
      meta.width = cam_meta.width;
      meta.height = cam_meta.height;
      meta.source = cam_meta.source;
      meta.labels = cam_meta.labels;
      pipeline.process_frame(frame, meta, produce_crops,
          [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) {
            emit(ev, meta.source.empty() ? arg2 : meta.source);
          });
    }
    capture.stop();
    aj::edge::otel::shutdown();
    return 0;
  }

  cv::VideoCapture cap;
  if (arg2.size() == 1 && std::isdigit(static_cast<unsigned char>(arg2[0])))
    cap.open(std::stoi(arg2), cv::CAP_V4L2);
  else
    cap.open(arg2);
  if (!cap.isOpened()) {
    spdlog::error("failed to open source '{}'", arg2);
    aj::edge::otel::shutdown();
    return 1;
  }
  cv::Mat frame;
  int64_t frame_id = 0;
  while (aj::edge::running()) {
    if (!cap.read(frame) || frame.empty()) break;
    ++frame_id;
    edge_cv::FrameMeta meta;
    meta.frame_id = frame_id;
    meta.capture_ts = edge_cv::Clock::now();
    meta.width = frame.cols;
    meta.height = frame.rows;
    meta.source = arg2;
    pipeline.process_frame(frame, meta, produce_crops,
        [&](const edge_cv::ConsumerPipeline::ObjectEvent& ev) { emit(ev, arg2); });
  }
  aj::edge::otel::shutdown();
  return 0;
}
