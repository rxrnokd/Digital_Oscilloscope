#include "ScopeSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace slp {

// ---------------------------------------------------------------------------------
// Defaults: a pleasant first screen (CH1 sine, CH2 square) that also documents the
// parameter conventions.
// ---------------------------------------------------------------------------------
ScopeSettings ScopeSettings::defaults() {
    ScopeSettings s;
    for (int c = 0; c < kNumChannels; ++c) {
        s.ch[c].color = defaultChannelColor(c);
        s.ch[c].input = c;
        s.ch[c].enabled = c < 2;
    }
    s.ch[0].position = 1.8;
    s.ch[1].position = -2.2;
    s.ch[2].position = 0.0;
    s.ch[3].position = 0.0;

    s.gen[0].shape = Waveform::Sine;
    s.gen[0].frequency = 1000.0;
    s.gen[0].vpp = 2.0;

    s.gen[1].shape = Waveform::Square;
    s.gen[1].frequency = 1000.0;
    s.gen[1].vpp = 1.6;
    s.gen[1].edgeTime = 5e-6;

    s.gen[2].shape = Waveform::Triangle;
    s.gen[2].frequency = 500.0;
    s.gen[2].vpp = 3.0;

    s.gen[3].shape = Waveform::Custom;
    s.gen[3].frequency = 1000.0;
    s.gen[3].vpp = 2.0;
    s.gen[3].noiseRms = 0.02;
    s.gen[3].components = {{Waveform::Sine, 1.0, 2.0, 0.0},
                           {Waveform::Sine, 3.0, 0.4, 0.0},
                           {Waveform::Sine, 5.0, 0.2, 0.0}};
    for (auto& g : s.gen)
        if (g.components.empty()) g.components = {{Waveform::Sine, 1.0, 1.0, 0.0}};
    s.math.srcA = 1;
    return s;
}

uint32_t defaultChannelColor(int ch) {
    static const uint32_t colors[kNumChannels] = {0xFFD60A, 0x22D3EE, 0xF050D8, 0x3EE06A};
    return colors[std::clamp(ch, 0, kNumChannels - 1)];
}

// ---------------------------------------------------------------------------------
// enum <-> string
// ---------------------------------------------------------------------------------
namespace {
const char* kWaveform[] = {"Sine", "Square", "Triangle", "Sawtooth", "Pulse", "DC", "Noise", "Custom"};
const char* kCoupling[] = {"DC", "AC", "GND"};
const char* kTrigMode[] = {"Auto", "Normal", "Single"};
const char* kSlope[] = {"Rising", "Falling"};
const char* kSource[] = {"Generator", "File", "Audio", "Serial"};
const char* kWindow[] = {"Rectangular", "Hann", "Hamming", "BlackmanHarris", "FlatTop"};
const char* kMathOp[] = {"Add", "Subtract", "Multiply", "Derivative", "Integral",
                         "LowPass", "HighPass", "BandPass", "MovingAverage"};
const char* kCursor[] = {"Off", "Manual", "Track"};
const char* kPersist[] = {"Off", "Short", "Long", "Infinite"};
const char* kInterp[] = {"Linear", "SinX"};

template <class E, size_t N>
E lookup(std::string_view s, const char* (&names)[N], E fallback) {
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<E>(i);
    return fallback;
}
template <class E, size_t N>
const char* name(E e, const char* (&names)[N]) {
    auto i = static_cast<size_t>(e);
    return i < N ? names[i] : "?";
}
} // namespace

const char* toString(Waveform e) { return name(e, kWaveform); }
const char* toString(Coupling e) { return name(e, kCoupling); }
const char* toString(TriggerMode e) { return name(e, kTrigMode); }
const char* toString(TriggerSlope e) { return name(e, kSlope); }
const char* toString(SourceKind e) { return name(e, kSource); }
const char* toString(WindowType e) { return name(e, kWindow); }
const char* toString(MathOp e) { return name(e, kMathOp); }
const char* toString(CursorMode e) { return name(e, kCursor); }
const char* toString(Persistence e) { return name(e, kPersist); }
const char* toString(Interpolation e) { return name(e, kInterp); }

template <> Waveform enumFromString(std::string_view s, Waveform f) { return lookup(s, kWaveform, f); }
template <> Coupling enumFromString(std::string_view s, Coupling f) { return lookup(s, kCoupling, f); }
template <> TriggerMode enumFromString(std::string_view s, TriggerMode f) { return lookup(s, kTrigMode, f); }
template <> TriggerSlope enumFromString(std::string_view s, TriggerSlope f) { return lookup(s, kSlope, f); }
template <> SourceKind enumFromString(std::string_view s, SourceKind f) { return lookup(s, kSource, f); }
template <> WindowType enumFromString(std::string_view s, WindowType f) { return lookup(s, kWindow, f); }
template <> MathOp enumFromString(std::string_view s, MathOp f) { return lookup(s, kMathOp, f); }
template <> CursorMode enumFromString(std::string_view s, CursorMode f) { return lookup(s, kCursor, f); }
template <> Persistence enumFromString(std::string_view s, Persistence f) { return lookup(s, kPersist, f); }
template <> Interpolation enumFromString(std::string_view s, Interpolation f) { return lookup(s, kInterp, f); }

// ---------------------------------------------------------------------------------
// 1-2-5 sequence
// ---------------------------------------------------------------------------------
namespace {
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int index125(double v) {
    if (!(v > 0)) return -36; // 1e-12
    const double l = std::log10(v);
    int e = static_cast<int>(std::floor(l));
    const double lm = l - e; // log10 of mantissa in [0,1)
    int k;
    if (lm < 0.150515) k = 0;      // < sqrt(2)
    else if (lm < 0.5) k = 1;      // < sqrt(10)
    else if (lm < 0.849485) k = 2; // < sqrt(50)
    else { k = 0; ++e; }
    return 3 * e + k;
}

double value125(int idx) {
    static const double m[3] = {1.0, 2.0, 5.0};
    const int e = floorDiv(idx, 3);
    const int k = idx - 3 * e;
    return m[k] * std::pow(10.0, e);
}
} // namespace

double step125(double v, int steps) { return value125(index125(v) + steps); }
double round125(double v) { return value125(index125(v)); }
double ceil125(double v) {
    int idx = index125(v);
    while (value125(idx) < v * (1.0 - 1e-9)) ++idx;
    while (value125(idx - 1) >= v * (1.0 - 1e-9)) --idx;
    return value125(idx);
}

// ---------------------------------------------------------------------------------
// Formatting / parsing
// ---------------------------------------------------------------------------------
std::string formatSI(double v, std::string_view unit, int significant) {
    if (!std::isfinite(v)) return "N/A";
    static const char* prefixes[] = {"p", "n", "\xC2\xB5", "m", "", "k", "M", "G"};
    const double a = std::fabs(v);
    int e3 = 0;
    if (a >= 1e-15) e3 = std::clamp(static_cast<int>(std::floor(std::log10(a) / 3.0)), -4, 3);
    char buf[64];
    for (int pass = 0; pass < 3; ++pass) {
        const double scaled = v / std::pow(1000.0, e3);
        const double as = std::fabs(scaled);
        const int intDigits = as >= 100 ? 3 : as >= 10 ? 2 : 1;
        const int dec = std::max(0, significant - intDigits);
        const double p = std::pow(10.0, dec);
        const double rounded = std::round(as * p) / p;
        if (rounded >= 1000.0 && e3 < 3) { ++e3; continue; }
        std::snprintf(buf, sizeof buf, "%.*f", dec, rounded == 0.0 ? 0.0 : scaled);
        break;
    }
    std::string out(buf);
    out += ' ';
    out += prefixes[e3 + 4];
    out += unit;
    return out;
}

std::string formatValue(double v, std::string_view unit, int significant) {
    if (!std::isfinite(v)) return "N/A";
    char buf[64];
    if (unit == "FS") {
        std::snprintf(buf, sizeof buf, "%.4f FS", v == 0.0 ? 0.0 : v);
        return buf;
    }
    if (unit == "%") {
        std::snprintf(buf, sizeof buf, "%.2f %%", v);
        return buf;
    }
    if (unit.size() >= 2 && unit.substr(0, 2) == "dB") {
        std::snprintf(buf, sizeof buf, "%.2f %.*s", v, static_cast<int>(unit.size()), unit.data());
        return buf;
    }
    return formatSI(v, unit, significant);
}

bool parseSI(std::string_view s, double& out) {
    size_t i = 0;
    auto skipWs = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
    skipWs();
    // locale independent number parser: [+-]digits[.digits][e[+-]digits]
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    double mant = 0.0;
    int digits = 0, fracDigits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { mant = mant * 10 + (s[i] - '0'); ++i; ++digits; }
    if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            mant = mant * 10 + (s[i] - '0'); ++i; ++digits; ++fracDigits;
        }
    }
    if (digits == 0) return false;
    int exp10 = -fracDigits;
    if (i + 1 < s.size() && (s[i] == 'e' || s[i] == 'E') &&
        (std::isdigit(static_cast<unsigned char>(s[i + 1])) ||
         ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 2]))))) {
        ++i;
        bool eneg = false;
        if (s[i] == '-' || s[i] == '+') { eneg = s[i] == '-'; ++i; }
        int e = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { e = e * 10 + (s[i] - '0'); ++i; if (e > 400) e = 400; }
        exp10 += eneg ? -e : e;
    }
    double v = mant * std::pow(10.0, exp10);
    skipWs();
    if (i < s.size()) {
        double mult = 1.0;
        bool prefix = true;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case 'p': mult = 1e-12; break;
        case 'n': mult = 1e-9; break;
        case 'u': mult = 1e-6; break;
        case 'm': mult = 1e-3; break;
        case 'k': case 'K': mult = 1e3; break;
        case 'M': mult = 1e6; break;
        case 'G': mult = 1e9; break;
        default: prefix = false;
        }
        if (!prefix && c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xB5) {
            mult = 1e-6; prefix = true; ++i; // UTF-8 micro sign
        }
        // A lone 'm' followed by nothing is milli; "ms", "mV" also milli.
        if (prefix) v *= mult;
    }
    out = neg ? -v : v;
    return std::isfinite(out);
}

} // namespace slp
