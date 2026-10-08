# object-classifier

**Primary detect + track → `ObjectEnvelope` for AgentJetson.**

Edge CV consumer that runs [agentjetson/rf-detr](https://github.com/agentjetson/rf-detr) (RF-DETR via ONNX Runtime — TensorRT → CUDA → CPU), tracks objects with a Kalman + ByteTrack-lite tracker, and publishes stable `detection.v1.ObjectEnvelope` messages to [agentjetson/core](https://github.com/agentjetson/core) ingest (`IngestObject` / `cv.object.*`).

This README is the local trail marker. The architecture map lives in the README of https://github.com/agentjetson/core (bring-up order §6).

---

## Role in the stack

```text
Cameras / files / RTSP
        │
        ▼
camera-connector (or --in-process capture)
        │  FrameQueue / FrameEnvelope
        ▼
object-classifier  ◄── agentjetson/rf-detr (ONNX)
        │  ObjectEnvelope  (cv.object.<class>)
        ▼
core ingest → NATS JetStream → aggregator / alpr-consumer / …
```

- **ObjectEnvelope remains the cornerstone for detection.** Upstream capture and scene routing can change; downstream specialists must not care how the envelope was produced.
- Scene gating (SigLIP / DINOv3 / MoViNet) is owned by [scene-router](https://github.com/agentjetson/scene-router) / [temporal-classifier](https://github.com/agentjetson/temporal-classifier). This repo stays a pure detect+track consumer.

---

## Prerequisites

| Dependency | Notes |
|------------|--------|
| CMake ≥ 3.20, C++20 | |
| OpenCV ≥ 4.5 | 4.x or 5.x |
| ONNX Runtime | set `ONNXRUNTIME_ROOT` (or `ONNXRUNTIME_ROOT_DIR`) |
| protobuf + gRPC | for ingest / FrameService protos |
| spdlog, Threads | fetched / system |

```bash
export ONNXRUNTIME_ROOT=/path/to/onnxruntime   # or ONNXRUNTIME_ROOT_DIR
```

CMake will `find_package(rfdetr_onnx)` and, if missing, fetch [agentjetson/rf-detr](https://github.com/agentjetson/rf-detr) via FetchContent.

---

## Models

Primary weights: **`models/rf-detr-nano.onnx`** (COCO-80). Larger variants are optional.

### Download script

```bash
# Nano only (recommended for first demo)
./scripts/download_models.sh

# Other variants
./scripts/download_models.sh --variant small
./scripts/download_models.sh --variant base
./scripts/download_models.sh --all

# Custom output directory
MODEL_DIR=/opt/models ./scripts/download_models.sh
```

Source: pre-converted ONNX from [PierreMarieCurie/rf-detr-onnx](https://huggingface.co/PierreMarieCurie/rf-detr-onnx) (RF-DETR 1.4.1 / Roboflow COCO). License: Apache-2.0 for nano/small/base/medium.

| Variant | Local path after download | Typical use |
|---------|---------------------------|-------------|
| nano (default) | `models/rf-detr-nano.onnx` | Edge / Jetson first demo |
| small | `models/rf-detr-small.onnx` | Higher accuracy |
| base | `models/rf-detr-base.onnx` | Server / high-end GPU |

Optional: set `LABELS_PATH` to a one-class-per-line file to override the built-in COCO-80 names.

---

## Build

```bash
git clone https://github.com/agentjetson/object-classifier.git
cd object-classifier
./scripts/download_models.sh

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Binary: `build/object_classifier`.

---

## Run (aligned with System Overview §6)

### Recommended single-box path (capture + classify in one process)

Vendors the camera-connector `FrameQueue` path — no separate camera-connector process required.

```bash
# Video file
INGEST_ADDR=<ingest-host>:50052 \
ORT_DEVICE=gpu \
./build/object_classifier --in-process video.mp4 models/rf-detr-nano.onnx

# Webcam
INGEST_ADDR=<ingest-host>:50052 \
./build/object_classifier --in-process 0 models/rf-detr-nano.onnx

# RTSP
INGEST_ADDR=<ingest-host>:50052 \
./build/object_classifier --in-process rtsp://user:pass@cam/stream models/rf-detr-nano.onnx
```

### Standalone source mode

Opens `cv::VideoCapture` inside the classifier (no shared queue).

```bash
INGEST_ADDR=<ingest-host>:50052 \
./build/object_classifier --source video.mp4 models/rf-detr-nano.onnx
```

### Network path (gRPC FrameService)

Subscribe to a remote [camera-connector](https://github.com/agentjetson/camera-connector) `FrameService` stream.

```bash
INGEST_ADDR=<ingest-host>:50052 \
FRAME_GRPC_ADDR=<camera-host>:50060 \
./build/object_classifier --network-path localhost:50060 models/rf-detr-nano.onnx
```

### Environment

| Variable | Default | Meaning |
|----------|---------|---------|
| `INGEST_ADDR` | `localhost:50052` | core ingest gRPC |
| `ORT_DEVICE` | `gpu` | `gpu` or `cpu` (rf-detr provider selection) |
| `EDGE_FORCE_CPU` | `0` | if `1`, force CPU regardless of `ORT_DEVICE` |
| `DET_CONF` | `0.35` | detector confidence threshold |
| `LABELS_PATH` | (empty) | override COCO class names |
| `FRAME_GRPC_ADDR` | `localhost:50060` | FrameService address (`--network-path`) |
| `FRAME_CAMERA_ID` | (empty) | optional Subscribe filter |
| `FRAME_ENCODING` | `jpeg` | preferred encoding (`jpeg` / `raw_bgr`) |

Default emit classes: `person`, `car`, `truck`, `bus`, `motorcycle` (see `ConsumerPipeline::Config`).

---

## Docker

```bash
./scripts/download_models.sh
# place a sample under data/sample.mp4 if desired

docker build -f docker/Dockerfile -t object-classifier .

docker run --rm --network host \
  -e INGEST_ADDR=localhost:50052 \
  -e ORT_DEVICE=cpu \
  -v "$(pwd)/models:/app/models:ro" \
  -v /path/to/sample.mp4:/app/data/sample.mp4:ro \
  object-classifier
```

Or compose (against core on the host):

```bash
docker compose up --build
```

---

## First-demo path (detection-only)

From the System Overview **path A**:

```text
1. core
   docker compose up -d nats nats-publisher ingest aggregator consumer

2. alpr-consumer (optional specialist)
   NATS_URL=nats://localhost:4222 ORT_DEVICE=gpu ./build/alpr_consumer

3. object-classifier (this repo)
   INGEST_ADDR=localhost:50052 ORT_DEVICE=gpu \
   ./build/object_classifier --in-process sample.mp4 models/rf-detr-nano.onnx

4. Watch
   - NATS subjects cv.object.>
   - alpr publishes cv.result.alpr (when gated / vehicle classes)
   - aggregator emits cv.alert
   - consumer logs alerts
```

camera-connector and crop-preparator are **not** required when using `--in-process`.

---

## Architecture (this binary)

| Component | Path | Role |
|-----------|------|------|
| `ConsumerPipeline` | `src/cv/` | detect → track → emit `ObjectEvent` |
| `Detector` | `src/cv/` | thin wrapper over `rfdetr::RFDETRModel` |
| `Tracker` | `src/cv/` | Kalman constant-velocity + ByteTrack two-stage association (OpenCV only, no ReID) |
| Capture (vendored) | `src/capture/` | in-process `FrameQueue` for `--in-process` |
| CLI | `src/classifier/main.cpp` | `--source` / `--in-process` / `--network-path` → ingest |

### Tracker notes

- High-confidence detections (`≥ high_thresh`) spawn tracks; low-confidence boxes only recover lost tracks.
- Class-aware matching (never associates across classes).
- Public API: `update(vector<Detection>, frame)` returns dets with `track_id` set. **Callers must use the return value** (by-value API).

---

## Stable contract

Output: `detection.v1.ObjectEnvelope` via `IngestService.IngestObject`.

Relevant fields: `frame_id`, `timestamp`, `source`, `class_name` / `class_id`, `confidence`, `track_id`, `box` (xyxy), optional `crop_jpeg`, frame size, capture latency.

Downstream subjects (via core NATS publisher): `cv.object.<class>`.

---

## Repository layout

```text
object-classifier/
├── CMakeLists.txt
├── README.md
├── docker/
│   └── Dockerfile
├── docker-compose.yml
├── scripts/
│   └── download_models.sh    # RF-DETR ONNX weights
├── models/                   # gitignored *.onnx; use download script
├── proto/                    # vendored capture / detection / ingest protos
└── src/
    ├── classifier/main.cpp
    ├── capture/              # in-process FrameQueue + Capture
    ├── common/
    └── cv/                   # detector, tracker, postprocess, pipeline
```

---

## Design principles (local)

1. **Capture is not classification** — camera-connector owns sources; this binary owns detection + tracking.
2. **ObjectEnvelope is the stable detection contract** — do not break field semantics for specialists.
3. **One detection engine** — primary detect uses agentjetson/rf-detr; swap weights, not frameworks.
4. **Edge-first** — heavy inference stays near the camera; core is correlation + durable bus.

---

*Keep this README in sync with the System Overview when the architecture evolves.*
