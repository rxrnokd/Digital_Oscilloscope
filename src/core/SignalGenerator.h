// SIGNAL LAB PRO - built-in function generator.
//
// Every generator is a phase accumulator (DDS style): changing the frequency keeps the
// waveform phase-continuous, exactly like a real arbitrary/function generator.
// The generator knows nothing about wall-clock time: the acquisition engine asks it for
// `n` samples spaced `dt` seconds apart and tells it to `advance()` over gaps (dead time
// between segmented acquisitions). Therefore the virtual time axis is always consistent
// with the sample rate.
//
// Conventions (all waveforms):  phase 0 deg = mid-level, rising.
//   Sine     : sin(2*pi*p)
//   Square   : high for p < duty, rising 50 % point at p = 0
//   Pulse    : as Square, default narrow duty (user set)
//   Triangle : 0 -> +1 (p=0.25) -> -1 (p=0.75) -> 0
//   Sawtooth : rising ramp, 0 at p = 0, reset (fall) at p = 0.5
//   DC       : output = offset
//   Noise    : Gaussian, sigma = Vpp / 6 (99.7 % of samples inside Vpp)
//   Custom   : offset + sum of partials (shape, multiple of f, Vpp, phase)
// Edge time (Square/Pulse) is the 10 %-90 % transition time; the transition is linear.
#pragma once

#include "ScopeSettings.h"

#include <cstdint>
#include <vector>

namespace slp {

// xoshiro256** PRNG + Box-Muller Gaussian. Fast and statistically good.
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { seedWith(seed); }
    void seedWith(uint64_t seed);
    uint64_t next();
    double uniform();  // [0, 1)
    double gauss();    // N(0, 1)

private:
    uint64_t s_[4] = {};
    bool haveSpare_ = false;
    double spare_ = 0.0;
};

class SignalGenerator {
public:
    explicit SignalGenerator(uint64_t seed = 1);

    void setSettings(const GeneratorSettings& s); // keeps the phase (phase continuous)
    const GeneratorSettings& settings() const { return s_; }

    void resetPhase();              // "Sync phase": restart all accumulators at 0
    void advance(double seconds);   // move the phase over a gap without producing output
    void generate(float* out, size_t n, double dt);

    // Normalised waveform value in [-1, 1] for phase p in [0, 1).
    // duty in (0,1), rampFrac = full linear transition time as a fraction of the period.
    static double shape(Waveform w, double p, double duty, double rampFrac);

    double phase() const { return phase_; }

private:
    void syncComponentState();

    GeneratorSettings s_;
    double phase_ = 0.0;             // cycles, [0,1)
    std::vector<double> compPhase_;  // per-partial accumulators (Custom)
    Rng rng_;
};

} // namespace slp
