#include "Trigger.h"

#include <algorithm>
#include <cmath>

namespace slp {

void EdgeTrigger::configure(TriggerSlope slope, double level, double hysteresis, double holdoffSamples) {
    slope_ = slope;
    level_ = level;
    hyst_ = std::fabs(hysteresis);
    holdoff_ = std::max(0.0, holdoffSamples);
}

void EdgeTrigger::reset() {
    armed_ = false;
    havePrev_ = false;
    lastTrig_ = -std::numeric_limits<double>::infinity();
}

void EdgeTrigger::process(const float* x, size_t n, int64_t start, std::vector<double>& events) {
    const double lvl = level_;
    const double armLo = level_ - hyst_, armHi = level_ + hyst_;
    for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        if (!std::isfinite(v)) { armed_ = false; havePrev_ = false; continue; }
        const double idx = static_cast<double>(start + static_cast<int64_t>(i));
        if (slope_ == TriggerSlope::Rising) {
            if (!armed_) {
                if (v < armLo || (hyst_ == 0.0 && v < lvl)) armed_ = true;
            } else if (v >= lvl && havePrev_) {
                const double d = v - prev_;
                const double t = (idx - 1.0) + (d > 0 ? (lvl - prev_) / d : 1.0);
                if (t - lastTrig_ >= holdoff_) { events.push_back(t); lastTrig_ = t; }
                armed_ = false;
            }
        } else {
            if (!armed_) {
                if (v > armHi || (hyst_ == 0.0 && v > lvl)) armed_ = true;
            } else if (v <= lvl && havePrev_) {
                const double d = prev_ - v;
                const double t = (idx - 1.0) + (d > 0 ? (prev_ - lvl) / d : 1.0);
                if (t - lastTrig_ >= holdoff_) { events.push_back(t); lastTrig_ = t; }
                armed_ = false;
            }
        }
        prev_ = static_cast<float>(v);
        havePrev_ = true;
    }
}

} // namespace slp
