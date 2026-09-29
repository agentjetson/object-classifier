# syntax=docker/dockerfile:1.7
# object-classifier — primary RF-DETR detect + track → ObjectEnvelope → core ingest
# Base: agentjetsonai/cv-base (Ubuntu 26.04 + OpenCV 5.0.0 + ORT 1.30.0 + rf-detr @ /opt/*)

FROM agentjetsonai/cv-base AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

# OpenCV / ORT / rf-detr come from the base — do not install distro opencv or download ORT
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config curl ca-certificates \
        libabsl-dev \
        libprotobuf-dev protobuf-compiler \
        libgrpc++-dev protobuf-compiler-grpc \
        libeigen3-dev \
        libyaml-cpp-dev \
        libspdlog-dev \
        libssl-dev \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Base already exports:
#   OpenCV_DIR, ONNXRUNTIME_ROOT, ONNXRUNTIME_ROOT_DIR, rfdetr_onnx_DIR
RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target object_classifier

# ── runtime ──────────────────────────────────────────────
FROM agentjetsonai/cv-base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libprotobuf32t64 \
        libgrpc++1.51t64 \
        libssl3t64 \
        ca-certificates \
        libabsl20260107 \
        libyaml-cpp0.8 \
        libspdlog1.15 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/object_classifier /app/object_classifier

ENV LD_LIBRARY_PATH=/opt/opencv/lib:/opt/onnxruntime/lib \
    INGEST_ADDR=ingest:50052 \
    ORT_DEVICE=cpu \
    EDGE_FORCE_CPU=1 \
    DET_CONF=0.35

WORKDIR /app

# Models + optional sample video must be mounted at runtime, e.g.:
#   -v $(pwd)/models:/app/models:ro
#   -v /path/to/sample.mp4:/app/sample.mp4:ro
#
# Default CMD runs the recommended single-box path (in-process capture).
# Override the source / model via command or env as needed.
CMD ["/app/object_classifier", "--in-process", "/app/sample.mp4", "/app/models/rf-detr-nano.onnx"]