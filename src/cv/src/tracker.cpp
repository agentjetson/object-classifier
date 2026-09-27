#include "tracker.hpp"

#include <opencv2/core.hpp>
#include <iostream>
#include <numeric>

namespace edge_cv {

// ============================================================================
// KalmanBoxTracker
// ============================================================================

cv::Mat Tracker::KalmanBoxTracker::to_xyah(const BoundingBox& b) {
    // [center_x, center_y, aspect_ratio, height]
    float w = std::max(1.f, b.width());
    float h = std::max(1.f, b.height());
    cv::Mat x(4, 1, CV_32F);
    x.at<float>(0) = b.x1 + w * 0.5f;
    x.at<float>(1) = b.y1 + h * 0.5f;
    x.at<float>(2) = w / h;
    x.at<float>(3) = h;
    return x;
}

BoundingBox Tracker::KalmanBoxTracker::from_xyah(const cv::Mat& xyah) {
    float cx = xyah.at<float>(0);
    float cy = xyah.at<float>(1);
    float a  = std::max(1e-3f, xyah.at<float>(2));
    float h  = std::max(1.f,   xyah.at<float>(3));
    float w  = a * h;
    BoundingBox b;
    b.x1 = cx - w * 0.5f;
    b.y1 = cy - h * 0.5f;
    b.x2 = cx + w * 0.5f;
    b.y2 = cy + h * 0.5f;
    return b;
}

Tracker::KalmanBoxTracker::KalmanBoxTracker(const Detection& det, int track_id,
                                            const Config& cfg)
    : id(track_id), last(det), age(0), time_since_update(0), hits(1), hit_streak(1)
{
    // Constant-velocity Kalman on state [cx, cy, a, h, vx, vy, va, vh]
    // Measurement is [cx, cy, a, h]
    kf = cv::KalmanFilter(8, 4, 0, CV_32F);

    // Transition matrix F (constant velocity)
    kf.transitionMatrix = cv::Mat::eye(8, 8, CV_32F);
    for (int i = 0; i < 4; ++i)
        kf.transitionMatrix.at<float>(i, i + 4) = 1.f;

    // Measurement matrix H
    kf.measurementMatrix = cv::Mat::zeros(4, 8, CV_32F);
    for (int i = 0; i < 4; ++i)
        kf.measurementMatrix.at<float>(i, i) = 1.f;

    // Process noise Q
    cv::setIdentity(kf.processNoiseCov, cv::Scalar(cfg.process_noise));
    // Higher uncertainty on velocities
    for (int i = 4; i < 8; ++i)
        kf.processNoiseCov.at<float>(i, i) = cfg.process_noise * 10.f;

    // Measurement noise R
    cv::setIdentity(kf.measurementNoiseCov, cv::Scalar(cfg.measurement_noise));

    // Initial error covariance
    cv::setIdentity(kf.errorCovPost, cv::Scalar(1.f));
    for (int i = 4; i < 8; ++i)
        kf.errorCovPost.at<float>(i, i) = 10.f;  // velocity uncertainty

    // Initialise state from first detection
    cv::Mat xyah = to_xyah(det.box);
    for (int i = 0; i < 4; ++i)
        kf.statePost.at<float>(i) = xyah.at<float>(i);
    // velocities start at 0
    for (int i = 4; i < 8; ++i)
        kf.statePost.at<float>(i) = 0.f;

    predicted = det.box;
    state = TrackState::Tentative;
}

void Tracker::KalmanBoxTracker::predict() {
    // Clamp aspect & height to stay positive after prediction
    if (kf.statePost.at<float>(2) <= 0.f) kf.statePost.at<float>(2) = 1e-3f;
    if (kf.statePost.at<float>(3) <= 0.f) kf.statePost.at<float>(3) = 1.f;

    cv::Mat prediction = kf.predict();
    predicted = from_xyah(prediction);

    age++;
    if (time_since_update > 0)
        hit_streak = 0;
    time_since_update++;
}

void Tracker::KalmanBoxTracker::update(const Detection& det) {
    last = det;
    time_since_update = 0;
    hits++;
    hit_streak++;

    cv::Mat measurement = to_xyah(det.box);
    kf.correct(measurement);
    predicted = from_xyah(kf.statePost);
}

BoundingBox Tracker::KalmanBoxTracker::get_state() const {
    return from_xyah(kf.statePost);
}

// ============================================================================
// Tracker public methods
// ============================================================================

int Tracker::num_confirmed() const {
    int n = 0;
    for (const auto& t : tracks_)
        if (t.state == TrackState::Confirmed) ++n;
    return n;
}

void Tracker::reset() {
    tracks_.clear();
    next_id_ = 1;
}

// ============================================================================
// Association
// ============================================================================

std::vector<std::pair<int,int>> Tracker::greedy_match(
    const std::vector<std::vector<float>>& cost,
    float max_cost)
{
    const int n_rows = static_cast<int>(cost.size());
    if (n_rows == 0) return {};
    const int n_cols = static_cast<int>(cost[0].size());
    if (n_cols == 0) return {};

    // Flatten into (cost, row, col) and sort ascending
    struct Entry { float c; int r, col; };
    std::vector<Entry> entries;
    entries.reserve(n_rows * n_cols);
    for (int r = 0; r < n_rows; ++r)
        for (int c = 0; c < n_cols; ++c)
            if (cost[r][c] < max_cost)
                entries.push_back({cost[r][c], r, c});

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.c < b.c; });

    std::vector<bool> row_used(n_rows, false);
    std::vector<bool> col_used(n_cols, false);
    std::vector<std::pair<int,int>> matches;
    matches.reserve(std::min(n_rows, n_cols));

    for (const auto& e : entries) {
        if (row_used[e.r] || col_used[e.col]) continue;
        row_used[e.r] = true;
        col_used[e.col] = true;
        matches.emplace_back(e.r, e.col);
    }
    return matches;
}

std::vector<std::vector<float>> Tracker::build_cost(
    const std::vector<Detection>& dets,
    const std::vector<size_t>& det_indices,
    const std::vector<size_t>& track_indices) const
{
    const size_t R = det_indices.size();
    const size_t C = track_indices.size();
    const float INF = 1e5f;

    std::vector<std::vector<float>> cost(R, std::vector<float>(C, INF));
    for (size_t i = 0; i < R; ++i) {
        const auto& d = dets[det_indices[i]];
        for (size_t j = 0; j < C; ++j) {
            const auto& t = tracks_[track_indices[j]];
            // Never associate across classes
            if (d.class_id != t.last.class_id) continue;
            float iou_val = iou(d.box, t.predicted);
            cost[i][j] = 1.f - iou_val;   // lower is better
        }
    }
    return cost;
}

// ============================================================================
// Main update – ByteTrack-style two-stage + Kalman prediction
// ============================================================================

std::vector<Detection> Tracker::update(std::vector<Detection> dets) {
    // ------------------------------------------------------------------
    // 0. Predict all existing tracks
    // ------------------------------------------------------------------
    for (auto& t : tracks_)
        t.predict();

    // ------------------------------------------------------------------
    // 1. Split detections into high / low confidence (ByteTrack idea)
    // ------------------------------------------------------------------
    std::vector<size_t> high_dets, low_dets;
    high_dets.reserve(dets.size());
    low_dets.reserve(dets.size());
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dets[i].confidence >= cfg_.high_thresh)
            high_dets.push_back(i);
        else if (dets[i].confidence >= cfg_.low_thresh)
            low_dets.push_back(i);
        // below low_thresh → ignore completely
    }

    // Indices of tracks that are still candidates for association
    std::vector<size_t> track_indices(tracks_.size());
    std::iota(track_indices.begin(), track_indices.end(), 0);

    std::vector<bool> det_matched(dets.size(), false);
    std::vector<bool> track_matched(tracks_.size(), false);

    // ------------------------------------------------------------------
    // 2. First association: high-score detections ↔ all tracks
    // ------------------------------------------------------------------
    {
        auto cost = build_cost(dets, high_dets, track_indices);
        auto matches = greedy_match(cost, 1.f - cfg_.match_thresh);

        for (const auto& [di_local, ti_local] : matches) {
            size_t di = high_dets[di_local];
            size_t ti = track_indices[ti_local];
            tracks_[ti].update(dets[di]);
            dets[di].track_id = tracks_[ti].id;
            det_matched[di] = true;
            track_matched[ti] = true;
        }
    }

    // ------------------------------------------------------------------
    // 3. Second association: low-score detections ↔ remaining *lost* tracks
    //    (recovery of briefly occluded objects – classic ByteTrack)
    // ------------------------------------------------------------------
    {
        std::vector<size_t> remaining_tracks;
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            if (track_matched[ti]) continue;
            // Prefer recovering tracks that were recently seen
            if (tracks_[ti].time_since_update <= 1) continue; // already tried
            remaining_tracks.push_back(ti);
        }

        if (!low_dets.empty() && !remaining_tracks.empty()) {
            auto cost = build_cost(dets, low_dets, remaining_tracks);
            auto matches = greedy_match(cost, 1.f - cfg_.second_match_thresh);

            for (const auto& [di_local, ti_local] : matches) {
                size_t di = low_dets[di_local];
                size_t ti = remaining_tracks[ti_local];
                tracks_[ti].update(dets[di]);
                dets[di].track_id = tracks_[ti].id;
                det_matched[di] = true;
                track_matched[ti] = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // 4. Update track states & create new tracks for unmatched high dets
    // ------------------------------------------------------------------
    for (size_t ti = 0; ti < tracks_.size(); ++ti) {
        auto& t = tracks_[ti];
        if (track_matched[ti]) {
            if (t.hit_streak >= cfg_.min_hits)
                t.state = TrackState::Confirmed;
        } else {
            if (t.state == TrackState::Confirmed)
                t.state = TrackState::Lost;
        }
    }

    for (size_t di : high_dets) {
        if (det_matched[di]) continue;
        // New track from a high-confidence detection
        KalmanBoxTracker t(dets[di], next_id_++, cfg_);
        dets[di].track_id = t.id;
        tracks_.push_back(std::move(t));
    }

    // Unmatched low-score detections are deliberately *not* used to
    // spawn new tracks (ByteTrack design) – they only recover existing ones.

    // ------------------------------------------------------------------
    // 5. Remove dead tracks
    // ------------------------------------------------------------------
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
            [&](const KalmanBoxTracker& t) {
                return t.time_since_update > cfg_.max_age;
            }),
        tracks_.end());

    // ------------------------------------------------------------------
    // 6. For detections that are still unmatched (or tentative), leave
    //    track_id as assigned above. Confirmed tracks keep their id.
    // ------------------------------------------------------------------
    return dets;
}

} // namespace edge_cv
