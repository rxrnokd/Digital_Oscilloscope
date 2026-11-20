// SIGNAL LAB PRO - edge trigger detector.
//
// The trigger is evaluated on the actual acquired samples of the source channel:
//   Rising : the detector arms when the signal goes below (level - hysteresis) and fires
//            on the first sample >= level. Falling is the mirror image.
// The hysteresis band (noise rejection) prevents noise around the level from producing
// multiple triggers per edge. The exact trigger instant is linearly interpolated between
// the two samples that straddle the level (sub-sample trigger interpolation), which is
// what gives a real DSO its jitter-free display. Holdoff suppresses re-triggering for a
// given time after each trigger event.
#pragma once

#include "ScopeSettings.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace slp {

class EdgeTrigger {
public:
    void configure(TriggerSlope slope, double level, double hysteresis, double holdoffSamples);
    void reset(); // forget arm state and previous sample (call on gaps / restarts)

    // Scans a contiguous block whose first sample has absolute index `start`.
    // Appends fractional absolute indices of trigger instants to `events`.
    void process(const float* x, size_t n, int64_t start, std::vector<double>& events);

    bool armed() const { return armed_; }

private:
    TriggerSlope slope_ = TriggerSlope::Rising;
    double level_ = 0.0, hyst_ = 0.0, holdoff_ = 0.0;
    bool armed_ = false;
    bool havePrev_ = false;
    float prev_ = 0.0f;
    double lastTrig_ = -std::numeric_limits<double>::infinity();
};

} // namespace slp
