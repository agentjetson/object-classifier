# syntax=docker/dockerfile:1.7
# object-classifier — RF-DETR detect + track → ObjectEnvelope
# Base: agentjetsonai/gecko (OpenCV 5 + ORT + rf-detr + protobuf/gRPC)

FROM agentjetsonai/gecko AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
        libeigen3-dev \
        libyaml-cpp-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target object_classifier

FROM agentjetsonai/gecko

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libyaml-cpp0.8 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/object_classifier /app/object_classifier

ENV LD_LIBRARY_PATH=/opt/opencv/lib:/opt/onnxruntime/lib \
    INGEST_ADDR=ingest:50052 \
    ORT_DEVICE=cpu \
    EDGE_FORCE_CPU=1 \
    DET_CONF=0.35

WORKDIR /app
CMD ["/app/object_classifier", "--in-process", "/app/sample.mp4", "/app/models/rf-detr-nano.onnx"]