// SIGNAL LAB PRO - automatic measurements (IEEE 181 style pulse terminology).
//
// All measurements are computed on the samples of the displayed record (screen).
//   Max / Min / Pk-Pk : extreme sample values, Pk-Pk = Max - Min.
//   Top / Base        : histogram modes when the waveform is two-level (>= 60 % of the
//                       samples lie within +-10 % of the range around the two modes),
//                       otherwise Max / Min (sine, triangle, ...).
//   Amplitude         : Top - Base.
//   Mid reference     : (Top + Base) / 2, crossing hysteresis = 5 % of Amplitude.
//   Period            : mean distance between rising mid-level crossings (linear
//                       interpolation between samples); needs >= 2 rising crossings,
//                       i.e. at least one complete cycle on screen.
//   Frequency         : 1 / Period.
//   +Duty             : mean positive width (rising -> next falling mid crossing) / Period.
//   Rise / Fall       : 10 % -> 90 % (90 % -> 10 %) of Base..Top, averaged over all
//                       complete edges. If the edge is faster than one sample interval,
//                       the result is reported as an upper bound ("< dt").
//   Mean / RMS        : over an integer number of cycles when the period is known
//                       (removes the partial-cycle bias), else over the whole record.
//                       RMS is the true RMS including DC (AC+DC); AC RMS = std deviation.
// Values that cannot be determined from the data are flagged invalid -> UI shows "N/A".
#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace slp {

enum class MeasId { Max, Min, PkPk, Rms, Mean, Freq, Period, Duty, Rise, Fall, Amplitude, Top, Base, AcRms, Count };

struct MeasValue {
    double v = 0.0;
    bool valid = false;
    bool upperBound = false; // true: real value is <= v (limited by the sample interval)
};

struct MeasurementSet {
    std::array<MeasValue, static_cast<size_t>(MeasId::Count)> m{};
    int risingEdges = 0;
    int fallingEdges = 0;
    bool twoLevel = false;
    size_t samples = 0;

    const MeasValue& operator[](MeasId id) const { return m[static_cast<size_t>(id)]; }
    MeasValue& operator[](MeasId id) { return m[static_cast<size_t>(id)]; }
};

// x: samples (finite values only), n: count, dt: sample interval (s)
MeasurementSet measure(const float* x, size_t n, double dt);

const char* measName(MeasId id);
// Unit of a measurement given the channel unit (e.g. "V" -> Freq "Hz", Rise "s").
std::string measUnit(MeasId id, const std::string& channelUnit);

} // namespace slp
