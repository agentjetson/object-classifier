# syntax=docker/dockerfile:1.7
# object-classifier — primary RF-DETR detect + track → ObjectEnvelope → core ingest
# Aligned with agentjetson/core (Ubuntu 25.04 / OpenCV 4.10)

FROM ubuntu:25.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git pkg-config curl ca-certificates \
        libabsl-dev \
        libprotobuf-dev protobuf-compiler \
        libgrpc++-dev protobuf-compiler-grpc \
        libopencv-dev \
        libeigen3-dev \
        libyaml-cpp-dev \
        libspdlog-dev \
        libssl-dev \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ONNX Runtime (CPU). For GPU images swap the URL / add CUDA base later.
ARG ORT_VERSION=1.19.2
RUN mkdir -p /opt/onnxruntime && \
    curl -fsSL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" \
    | tar -xz -C /opt/onnxruntime --strip-components=1

WORKDIR /src
COPY . .

ENV ONNXRUNTIME_ROOT=/opt/onnxruntime \
    ONNXRUNTIME_ROOT_DIR=/opt/onnxruntime

# CMake will FetchContent rf-detr / motcpp / nats / spdlog as needed
RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target object_classifier

# ── runtime ──────────────────────────────────────────────
FROM ubuntu:25.04

ENV DEBIAN_FRONTEND=noninteractive

# Same OpenCV 4.10 ABI as core (plucky packages use the "410" suffix)
RUN apt-get update && apt-get install -y --no-install-recommends \
        libprotobuf32t64 libgrpc++1.51t64 libssl3 ca-certificates \
        libabsl20230802 \
        libopencv-core410 \
        libopencv-imgproc410 \
        libopencv-imgcodecs410 \
        libopencv-videoio410 \
        libopencv-calib3d410 \
        libopencv-features2d410 \
        libyaml-cpp0.8 \
        libspdlog1.12 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/onnxruntime /opt/onnxruntime
COPY --from=builder /src/build/object_classifier /app/object_classifier

ENV LD_LIBRARY_PATH=/opt/onnxruntime/lib \
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