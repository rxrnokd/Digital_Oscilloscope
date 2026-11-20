#include "Measurements.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(SLP_HAVE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace slp {

const char* measName(MeasId id) {
    static const char* names[] = {"Max", "Min", "Pk-Pk", "RMS", "Mean", "Freq", "Period", "+Duty",
                                  "Rise", "Fall", "Ampl", "Top", "Base", "AC RMS"};
    return names[static_cast<size_t>(id)];
}

std::string measUnit(MeasId id, const std::string& u) {
    switch (id) {
    case MeasId::Freq: return "Hz";
    case MeasId::Period:
    case MeasId::Rise:
    case MeasId::Fall: return "s";
    case MeasId::Duty: return "%";
    default: return u;
    }
}

namespace {

struct Crossings {
    std::vector<double> rises, falls; // fractional sample indices
};

Crossings findCrossings(const float* x, size_t n, double mid, double hyst) {
    Crossings c;
    enum { Unknown, Low, High } state = Unknown;
    long lastChange = 0;
    for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        if (state != High && v > mid + hyst) {
            if (state == Low) {
                long k = static_cast<long>(i) - 1;
                while (k > lastChange && x[k] >= mid) --k;
                const double d = static_cast<double>(x[k + 1]) - x[k];
                const double t = k + (d > 0 ? (mid - x[k]) / d : 0.5);
                c.rises.push_back(t);
            }
            state = High;
            lastChange = static_cast<long>(i);
        } else if (state != Low && v < mid - hyst) {
            if (state == High) {
                long k = static_cast<long>(i) - 1;
                while (k > lastChange && x[k] <= mid) --k;
                const double d = static_cast<double>(x[k]) - x[k + 1];
                const double t = k + (d > 0 ? (x[k] - mid) / d : 0.5);
                c.falls.push_back(t);
            }
            state = Low;
            lastChange = static_cast<long>(i);
        }
    }
    return c;
}

// Average period (in samples) from a list of same-direction crossings.
// Returns <= 0 if undetermined or unstable (coefficient of variation > 25 %).
double periodFrom(const std::vector<double>& t) {
    if (t.size() < 2) return -1.0;
    const double mean = (t.back() - t.front()) / static_cast<double>(t.size() - 1);
    if (!(mean > 0)) return -1.0;
    if (t.size() >= 3) {
        double var = 0.0;
        for (size_t i = 1; i < t.size(); ++i) {
            const double d = (t[i] - t[i - 1]) - mean;
            var += d * d;
        }
        var /= static_cast<double>(t.size() - 1);
        if (std::sqrt(var) / mean > 0.25) return -1.0;
    }
    return mean;
}

} // namespace

MeasurementSet measure(const float* x, size_t n, double dt) {
    MeasurementSet r;
    r.samples = n;
    if (n < 2 || !(dt > 0.0) || x == nullptr) return r;

    float mx, mn;
#if defined(SLP_HAVE_ACCELERATE)
    vDSP_maxv(x, 1, &mx, static_cast<vDSP_Length>(n));
    vDSP_minv(x, 1, &mn, static_cast<vDSP_Length>(n));
#else
    mx = mn = x[0];
    for (size_t i = 1; i < n; ++i) {
        mx = std::max(mx, x[i]);
        mn = std::min(mn, x[i]);
    }
#endif
    r[MeasId::Max] = {mx, true};
    r[MeasId::Min] = {mn, true};
    r[MeasId::PkPk] = {static_cast<double>(mx) - mn, true};

    const double range = static_cast<double>(mx) - mn;
    double top = mx, base = mn;

    // ---- Top / Base by histogram (two-level detection) ------------------------------
    if (range > 0.0) {
        constexpr int B = 200;
        constexpr int band = 20; // +-10 % of range
        std::vector<uint32_t> h(B, 0);
        const double scale = (B - 1e-9) / range;
        for (size_t i = 0; i < n; ++i) h[std::min(B - 1, static_cast<int>((x[i] - mn) * scale))]++;
        const int iTop = static_cast<int>(std::max_element(h.begin() + B / 2, h.end()) - h.begin());
        const int iBase = static_cast<int>(std::max_element(h.begin(), h.begin() + B / 2) - h.begin());
        if (iTop - iBase >= B / 2) {
            // classification uses the +-10 % band; the level itself is the mean of the
            // samples within +-2 % of the mode (excludes edge samples, unbiased for noise)
            constexpr int narrow = 4;
            double sT = 0, sB = 0;
            size_t cT = 0, cB = 0, nT = 0, nB = 0;
            for (size_t i = 0; i < n; ++i) {
                const int b = std::min(B - 1, static_cast<int>((x[i] - mn) * scale));
                const int dT = std::abs(b - iTop), dB = std::abs(b - iBase);
                if (dT <= band) { ++cT; if (dT <= narrow) { sT += x[i]; ++nT; } }
                else if (dB <= band) { ++cB; if (dB <= narrow) { sB += x[i]; ++nB; } }
            }
            if (nT > 0 && nB > 0 && static_cast<double>(cT + cB) >= 0.6 * static_cast<double>(n)) {
                top = sT / static_cast<double>(nT);
                base = sB / static_cast<double>(nB);
                r.twoLevel = true;
            }
        }
    }
    const double amp = top - base;
    r[MeasId::Top] = {top, true};
    r[MeasId::Base] = {base, true};
    r[MeasId::Amplitude] = {amp, true};

    // ---- crossings / timing ---------------------------------------------------------
    double periodS = -1.0; // samples
    Crossings c;
    const bool timingPossible = amp > 0.0 && amp > 1e-9 * std::max(std::fabs(top), std::fabs(base));
    if (timingPossible) {
        const double mid = 0.5 * (top + base);
        c = findCrossings(x, n, mid, 0.05 * amp);
        r.risingEdges = static_cast<int>(c.rises.size());
        r.fallingEdges = static_cast<int>(c.falls.size());
        periodS = periodFrom(c.rises);
        if (periodS <= 0) periodS = periodFrom(c.falls);
        if (periodS > 0) {
            r[MeasId::Period] = {periodS * dt, true};
            r[MeasId::Freq] = {1.0 / (periodS * dt), true};
            // positive width: rising crossing -> next falling crossing
            double wSum = 0;
            int wCount = 0;
            size_t fi = 0;
            for (size_t i = 0; i < c.rises.size(); ++i) {
                while (fi < c.falls.size() && c.falls[fi] <= c.rises[i]) ++fi;
                if (fi >= c.falls.size()) break;
                const double next = i + 1 < c.rises.size() ? c.rises[i + 1] : 1e300;
                if (c.falls[fi] < next) { wSum += c.falls[fi] - c.rises[i]; ++wCount; }
            }
            if (wCount > 0) r[MeasId::Duty] = {100.0 * (wSum / wCount) / periodS, true};
        }

        // Rise / fall 10-90 %
        const double lo = base + 0.1 * amp, hi = base + 0.9 * amp;
        auto edgeTimes = [&](const std::vector<double>& cross, bool rising) {
            double sum = 0;
            int count = 0, limited = 0;
            for (double t : cross) {
                const long k = static_cast<long>(std::floor(t));
                // search backwards for the start threshold, forwards for the end threshold
                long j = k;
                if (rising) { while (j >= 0 && x[j] >= lo) --j; }
                else { while (j >= 0 && x[j] <= hi) --j; }
                if (j < 0) continue;
                long m = k + 1;
                if (rising) { while (m < static_cast<long>(n) && x[m] < hi) ++m; }
                else { while (m < static_cast<long>(n) && x[m] > lo) ++m; }
                if (m >= static_cast<long>(n)) continue;
                double tStart, tEnd;
                if (rising) {
                    tStart = j + (lo - x[j]) / (static_cast<double>(x[j + 1]) - x[j]);
                    tEnd = (m - 1) + (hi - x[m - 1]) / (static_cast<double>(x[m]) - x[m - 1]);
                } else {
                    tStart = j + (x[j] - hi) / (static_cast<double>(x[j]) - x[j + 1]);
                    tEnd = (m - 1) + (x[m - 1] - lo) / (static_cast<double>(x[m - 1]) - x[m]);
                }
                if (m - 1 == j) { ++limited; continue; } // whole edge inside one sample step
                sum += (tEnd - tStart);
                ++count;
            }
            MeasValue v;
            if (count > 0) v = {sum / count * dt, true, false};
            else if (limited > 0) v = {dt, true, true};
            return v;
        };
        r[MeasId::Rise] = edgeTimes(c.rises, true);
        r[MeasId::Fall] = edgeTimes(c.falls, false);
    }

    // ---- Mean / RMS (integer number of cycles when possible) ------------------------
    size_t i0 = 0, i1 = n;
    if (periodS > 0 && c.rises.size() >= 2 && periodFrom(c.rises) > 0) {
        i0 = static_cast<size_t>(std::llround(c.rises.front()));
        i1 = static_cast<size_t>(std::llround(c.rises.back()));
        if (i1 <= i0 + 1 || i1 > n) { i0 = 0; i1 = n; }
    }
    double s = 0.0, s2 = 0.0;
    for (size_t i = i0; i < i1; ++i) {
        const double v = x[i];
        s += v;
        s2 += v * v;
    }
    const double cnt = static_cast<double>(i1 - i0);
    const double mean = s / cnt;
    const double ms = s2 / cnt;
    r[MeasId::Mean] = {mean, true};
    r[MeasId::Rms] = {std::sqrt(std::max(0.0, ms)), true};
    r[MeasId::AcRms] = {std::sqrt(std::max(0.0, ms - mean * mean)), true};
    return r;
}

} // namespace slp
