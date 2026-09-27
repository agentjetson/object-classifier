#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <map>

namespace edge_cv {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using DurationMs = std::chrono::duration<double, std::milli>;

struct BoundingBox {
    float x1{0.f};
    float y1{0.f};
    float x2{0.f};
    float y2{0.f};

    float width()  const { return x2 - x1; }
    float height() const { return y2 - y1; }
    float area()   const { return width() * height(); }
};

struct Detection {
    int         class_id{-1};
    std::string class_name;
    float       confidence{0.f};
    BoundingBox box;
    int         track_id{-1};
    std::string ocr_text;
    float       ocr_confidence{0.f};
};

struct TrackState {
    int         track_id{-1};
    int         age{0};
    int         frames_since_ocr{999};
    std::string last_ocr_text;
    float       last_ocr_conf{0.f};
    bool        plate_confirmed{false};
};

using TrackStateMap = std::unordered_map<int, TrackState>;

// Aligned with camera-connector::FrameMeta
struct FrameMeta {
    int64_t     frame_id{0};
    TimePoint   capture_ts;
    int         width{0};
    int         height{0};
    std::string source;
    std::map<std::string, std::string> labels;
};

struct Alert {
    int64_t                  frame_id{0};
    TimePoint                timestamp;
    std::vector<Detection>   detections;
    bool                     watchlist_hit{false};
    std::string              matched_label;
    double                   e2e_latency_ms{0.0};
};

struct WatchlistEntry {
    std::string label;
    float       min_confidence{0.5f};
};

} // namespace edge_cv
