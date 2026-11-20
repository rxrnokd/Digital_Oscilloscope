#include "SignalGenerator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slp {

namespace {
constexpr double kTwoPi = 2.0 * std::numbers::pi;

inline double frac(double x) { return x - std::floor(x); }

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
} // namespace

// ------------------------------------------------------------------------------- Rng
void Rng::seedWith(uint64_t seed) {
    for (auto& v : s_) v = splitmix(seed);
    haveSpare_ = false;
}

uint64_t Rng::next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double Rng::uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

double Rng::gauss() {
    if (haveSpare_) { haveSpare_ = false; return spare_; }
    double u1 = uniform();
    while (u1 <= 1e-300) u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(kTwoPi * u2);
    haveSpare_ = true;
    return r * std::cos(kTwoPi * u2);
}

// ------------------------------------------------------------------- SignalGenerator
SignalGenerator::SignalGenerator(uint64_t seed) : rng_(seed * 0x2545F4914F6CDD1Dull + 17) {}

void SignalGenerator::setSettings(const GeneratorSettings& s) {
    s_ = s;
    syncComponentState();
}

void SignalGenerator::syncComponentState() {
    if (compPhase_.size() != s_.components.size()) compPhase_.resize(s_.components.size(), phase_);
}

void SignalGenerator::resetPhase() {
    phase_ = 0.0;
    std::fill(compPhase_.begin(), compPhase_.end(), 0.0);
}

void SignalGenerator::advance(double seconds) {
    const double f = std::max(0.0, s_.frequency);
    phase_ = frac(phase_ + f * seconds);
    for (size_t k = 0; k < compPhase_.size(); ++k)
        compPhase_[k] = frac(compPhase_[k] + f * s_.components[k].multiple * seconds);
}

double SignalGenerator::shape(Waveform w, double p, double duty, double r) {
    switch (w) {
    case Waveform::Sine: return std::sin(kTwoPi * p);
    case Waveform::Triangle:
        if (p < 0.25) return 4.0 * p;
        if (p < 0.75) return 2.0 - 4.0 * p;
        return 4.0 * p - 4.0;
    case Waveform::Sawtooth: return p < 0.5 ? 2.0 * p : 2.0 * p - 2.0;
    case Waveform::Square:
    case Waveform::Pulse: {
        // Level in [0,1]. Rising transition centred on p = 0, falling centred on p = duty.
        double level;
        if (r <= 0.0) {
            level = p < duty ? 1.0 : 0.0;
        } else {
            const double h = 0.5 * r;
            if (p < h) level = 0.5 + p / r;
            else if (p < duty - h) level = 1.0;
            else if (p < duty + h) level = 1.0 - (p - (duty - h)) / r;
            else if (p < 1.0 - h) level = 0.0;
            else level = (p - (1.0 - h)) / r;
        }
        return 2.0 * level - 1.0;
    }
    case Waveform::DC: return 0.0;
    case Waveform::Noise:
    case Waveform::Custom: return 0.0; // handled in generate()
    }
    return 0.0;
}

void SignalGenerator::generate(float* out, size_t n, double dt) {
    if (n == 0) return;
    syncComponentState();
    const double f = std::max(0.0, s_.frequency);
    const double inc = f * dt; // cycles per sample
    const double amp = 0.5 * s_.vpp;
    const double off = s_.offset;
    const double ph0 = s_.phaseDeg / 360.0;

    if (!s_.output) { // output disabled: 0 V, but keep the phase running
        std::fill(out, out + n, 0.0f);
        advance(static_cast<double>(n) * dt);
        return;
    }

    switch (s_.shape) {
    case Waveform::Sine: {
        // Rotating phasor in double precision, re-seeded every 1024 samples with an exact
        // sin/cos so that the error never accumulates (~1e-13).
        const double cd = std::cos(kTwoPi * inc), sd = std::sin(kTwoPi * inc);
        size_t i = 0;
        double ph = phase_;
        while (i < n) {
            const size_t m = std::min<size_t>(n - i, 1024);
            const double a = kTwoPi * frac(ph + ph0);
            double c = std::cos(a), s = std::sin(a);
            for (size_t j = 0; j < m; ++j) {
                out[i + j] = static_cast<float>(off + amp * s);
                const double c2 = c * cd - s * sd;
                s = s * cd + c * sd;
                c = c2;
            }
            ph = frac(ph + inc * static_cast<double>(m));
            i += m;
        }
        phase_ = ph;
        break;
    }
    case Waveform::DC:
        std::fill(out, out + n, static_cast<float>(off));
        phase_ = frac(phase_ + inc * static_cast<double>(n));
        break;
    case Waveform::Noise: {
        const double sigma = s_.vpp / 6.0;
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<float>(off + sigma * rng_.gauss());
        phase_ = frac(phase_ + inc * static_cast<double>(n));
        break;
    }
    case Waveform::Custom: {
        std::fill(out, out + n, static_cast<float>(off));
        for (size_t k = 0; k < s_.components.size(); ++k) {
            const auto& c = s_.components[k];
            const double ik = inc * c.multiple;
            const double a = 0.5 * c.vpp;
            // global phase is a time shift: partial k is shifted by multiple * phase
            const double pk0 = c.phaseDeg / 360.0 + ph0 * c.multiple;
            double p = frac(compPhase_[k] + pk0);
            const Waveform w = c.shape == Waveform::Custom || c.shape == Waveform::Noise ||
                                       c.shape == Waveform::DC
                                   ? Waveform::Sine
                                   : c.shape;
            for (size_t i = 0; i < n; ++i) {
                out[i] += static_cast<float>(a * shape(w, p, 0.5, 0.0));
                p += ik;
                if (p >= 1.0) p -= std::floor(p);
            }
            compPhase_[k] = frac(compPhase_[k] + ik * static_cast<double>(n));
        }
        phase_ = frac(phase_ + inc * static_cast<double>(n));
        break;
    }
    default: { // Square, Pulse, Triangle, Sawtooth
        const double duty = std::clamp(s_.duty / 100.0, 0.001, 0.999);
        double rampFrac = 0.0;
        if ((s_.shape == Waveform::Square || s_.shape == Waveform::Pulse) && s_.edgeTime > 0.0) {
            rampFrac = (s_.edgeTime / 0.8) * f; // full linear ramp, as a fraction of the period
            rampFrac = std::min(rampFrac, std::min(duty, 1.0 - duty));
        }
        double p = frac(phase_ + ph0);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<float>(off + amp * shape(s_.shape, p, duty, rampFrac));
            p += inc;
            if (p >= 1.0) p -= std::floor(p);
        }
        phase_ = frac(phase_ + inc * static_cast<double>(n));
        break;
    }
    }

    if (s_.noiseRms > 0.0) {
        const double sigma = s_.noiseRms;
        for (size_t i = 0; i < n; ++i) out[i] += static_cast<float>(sigma * rng_.gauss());
    }
}

} // namespace slp
