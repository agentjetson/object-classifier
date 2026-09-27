# Improved Multi-Object Tracker for agentjetson/object-classifier

Drop-in replacement for the existing lightweight IoU tracker.

## What changed

| Feature | Old tracker | New tracker |
|---------|-------------|-------------|
| Motion model | None (last box only) | Constant-velocity Kalman filter on `(cx, cy, aspect, height)` |
| Association | Greedy IoU, single pass | ByteTrack-style **two-stage** (high-conf → low-conf recovery) |
| Cost metric | Raw IoU | `1 − IoU` with class gate (never cross classes) |
| Track lifecycle | Simple age / hits | Tentative → Confirmed → Lost (same semantics as DeepSORT / SORT) |
| Prediction through occlusion | No | Yes – Kalman predicts the box so IDs survive short misses |
| Dependencies | None | OpenCV only (already required by the project) |
| Public API | `update(vector<Detection>)` | **Identical** – zero changes needed in `ConsumerPipeline` |

## Solid ideas taken from the reference trackers

**Smorodov/Multitarget-tracker**
- Kalman filter for trajectory smoothing & prediction of missed detections
- IoU (Jaccard) distance as the primary association metric
- Explicit track age / hits / confirmed state

**shaoshengsong/DeepSORT**
- ByteTrack two-stage association (high-score first, then low-score recovery of lost tracks)
- Constant-velocity Kalman state `[cx, cy, a, h, vx, vy, va, vh]`
- Track states: Tentative / Confirmed / Lost
- Class-aware matching (association never crosses object classes)
- Cascade-style preference for recently updated tracks

Appearance ReID is intentionally omitted so the component stays lightweight on Jetson (no second ONNX model, no extra GPU memory). It can be added later behind a feature flag if needed.

## Integration

1. Replace the two files in the object-classifier tree:

```
src/cv/include/tracker.hpp   ←  this include/tracker.hpp
src/cv/src/tracker.cpp       ←  this src/tracker.cpp
```

2. `geo.hpp` and `types.hpp` stay unchanged.

3. `ConsumerPipeline` already does:

```cpp
tracker_ = std::make_unique<Tracker>();
...
tracker_->update(dets);   // now uses Kalman + ByteTrack logic
```

No other source changes are required.

4. Optional tuning via `Tracker::Config` (pass to the constructor):

```cpp
Tracker::Config cfg;
cfg.high_thresh   = 0.5f;   // ByteTrack high gate
cfg.low_thresh    = 0.1f;   // recovery gate
cfg.match_thresh  = 0.3f;   // min IoU
cfg.max_age       = 30;     // frames to keep a lost track
cfg.min_hits      = 3;      // hits before confirmed
tracker_ = std::make_unique<Tracker>(cfg);
```

## Behaviour notes for AgentJetson

- Only **high-confidence** detections (`≥ high_thresh`) can spawn new tracks. Low-confidence boxes are used solely to re-acquire lost tracks – this dramatically reduces ID switches under occlusion.
- Predicted boxes are used for matching, so a track that is temporarily missed still “owns” its spatial region and is more likely to be re-associated correctly.
- `Detection::track_id` is set for every matched detection (including newly created tracks). Downstream consumers that filter on `track_id > 0` continue to work.

## Files

```
object-classifier-tracker/
├── include/tracker.hpp   # public header (same namespace edge_cv)
├── src/tracker.cpp       # implementation
└── README.md             # this file
```
