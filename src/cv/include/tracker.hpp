#pragma once

#include "types.hpp"
#include "geo.hpp"

#include <opencv2/core.hpp>
#include <opencv2/video/tracking.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

namespace edge_cv {

/**
 * Multi-object tracker for AgentJetson object-classifier.
 *
 * Solid ideas taken from:
 *   - Smorodov/Multitarget-tracker  → Kalman prediction, IoU cost, track lifecycle
 *   - shaoshengsong/DeepSORT        → ByteTrack two-stage association, cascade,
 *                                     constant-velocity Kalman on (cx,cy,a,h)
 *
 * Design goals for Jetson edge:
 *   - No ReID / no extra ONNX model (appearance is optional later)
 *   - Pure OpenCV + STL, same public API as the previous lightweight tracker
 *   - Class-aware association (never cross person ↔ car etc.)
 *   - Predicts through short occlusions so IDs stay stable
 */
class Tracker {
public:
    struct Config {
        // Association
        float high_thresh     = 0.5f;   // ByteTrack high-score gate
        float low_thresh      = 0.1f;   // ByteTrack low-score recovery gate
        float match_thresh    = 0.3f;   // min IoU to accept a match
        float second_match_thresh = 0.5f; // stricter IoU for low-score stage

        // Track lifecycle (frames)
        int   max_age         = 30;     // keep lost tracks this many frames
        int   min_hits        = 3;      // consecutive hits before confirmed
        int   max_time_lost   = 30;     // same as max_age; alias for clarity

        // Kalman process / measurement noise (tuned for image coordinates)
        float process_noise   = 1e-2f;
        float measurement_noise = 1e-1f;
    };

    Tracker() : cfg_(Config{}) {}
    explicit Tracker(Config cfg) : cfg_(std::move(cfg)) {}

    /**
     * Update tracks with new detections.
     * Modifies dets in-place (sets track_id).
     * Returns the same vector with stable track_ids filled.
     *
     * Only confirmed tracks receive a positive track_id that is
     * re-used across frames. Tentative tracks get a temporary id
     * that becomes permanent once confirmed.
     */
    std::vector<Detection> update(std::vector<Detection> dets, const cv::Mat& frame);

    int num_tracks() const { return static_cast<int>(tracks_.size()); }
    int num_confirmed() const;

    void reset();

private:
    // -----------------------------------------------------------------------
    // Internal track representation
    // -----------------------------------------------------------------------
    enum class TrackState { Tentative, Confirmed, Lost };

    struct KalmanBoxTracker {
        int              id{-1};
        TrackState       state{TrackState::Tentative};
        Detection        last;           // last associated detection
        int              age{0};         // frames since creation
        int              time_since_update{0};
        int              hits{0};
        int              hit_streak{0};  // consecutive associations
        cv::KalmanFilter kf;

        // Predicted box in (x1,y1,x2,y2) for matching / emission
        BoundingBox      predicted;

        KalmanBoxTracker() = default;
        explicit KalmanBoxTracker(const Detection& det, int track_id, const Config& cfg);

        void predict();
        void update(const Detection& det);
        BoundingBox get_state() const;

        static cv::Mat to_xyah(const BoundingBox& b);
        static BoundingBox from_xyah(const cv::Mat& xyah);
    };

    // -----------------------------------------------------------------------
    // Association helpers
    // -----------------------------------------------------------------------
    static float iou(const BoundingBox& a, const BoundingBox& b) {
        return geom::intersect(a, b);
    }

    // Greedy matching on cost matrix (1 - IoU). Fast enough for typical
    // edge scenes (<< 50 objects). Returns pairs (det_idx, track_idx).
    static std::vector<std::pair<int,int>> greedy_match(
        const std::vector<std::vector<float>>& cost,
        float max_cost);

    // Build IoU cost matrix between detections and tracks (same class only).
    // cost[i][j] = 1 - IoU; large value if class mismatch.
    std::vector<std::vector<float>> build_cost(
        const std::vector<Detection>& dets,
        const std::vector<size_t>& det_indices,
        const std::vector<size_t>& track_indices) const;

    // -----------------------------------------------------------------------
    Config cfg_;
    int next_id_{1};
    std::vector<KalmanBoxTracker> tracks_;
};

} // namespace edge_cv
