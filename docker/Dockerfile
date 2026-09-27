# object-classifier — edge detection + tracking (RF-DETR + motcpp ByteTrack)
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config \
    libopencv-dev libeigen3-dev libyaml-cpp-dev \
    libprotobuf-dev protobuf-compiler \
    libgrpc++-dev protobuf-compiler-grpc \
    libspdlog-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    libopencv-core406t64 libopencv-imgproc406t64 libopencv-imgcodecs406t64 libopencv-videoio406t64 \
    libyaml-cpp0.8 libprotobuf32t64 libgrpc++1.51t64 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/object_classifier /app/object_classifier
# Models mounted at runtime
VOLUME ["/models", "/data"]

ENV INGEST_ADDR=ingest:50052
ENTRYPOINT ["/app/object_classifier"]
