// SIGNAL LAB PRO - instrument settings, enums and shared helpers.
// Pure C++20 (no Qt) so that the whole signal chain is unit-testable.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace slp {

constexpr int kNumChannels = 4;   // analog channels CH1..CH4
constexpr int kNumGenerators = 4; // built-in function generator outputs GEN1..GEN4
constexpr int kMathTrace = 4;     // trace index of the MATH channel
constexpr int kNumTraces = 5;     // CH1..CH4 + MATH
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Waveform { Sine, Square, Triangle, Sawtooth, Pulse, DC, Noise, Custom };
enum class Coupling { DC, AC, GND };
enum class TriggerMode { Auto, Normal, Single };
enum class TriggerSlope { Rising, Falling };
enum class SourceKind { Generator, File, Audio, Serial };
enum class WindowType { Rectangular, Hann, Hamming, BlackmanHarris, FlatTop };
enum class MathOp { Add, Subtract, Multiply, Derivative, Integral, LowPass, HighPass, BandPass, MovingAverage };
enum class CursorMode { Off, Manual, Track };
enum class Persistence { Off, Short, Long, Infinite };
enum class Interpolation { Linear, SinX };

// One partial of the user-defined (Custom) waveform. Frequency is a multiple of the
// generator's base frequency so that harmonic series stay locked to the fundamental.
struct HarmonicComponent {
    Waveform shape = Waveform::Sine; // Sine/Square/Triangle/Sawtooth
    double multiple = 1.0;           // f = multiple * base frequency
    double vpp = 1.0;                // V peak-to-peak of this partial
    double phaseDeg = 0.0;
    bool operator==(const HarmonicComponent&) const = default;
};

struct GeneratorSettings {
    bool output = true;
    Waveform shape = Waveform::Sine;
    double frequency = 1000.0; // Hz
    double vpp = 2.0;          // V peak-to-peak (Noise: Vpp = 6 sigma)
    double offset = 0.0;       // V DC offset
    double phaseDeg = 0.0;     // degrees
    double duty = 50.0;        // % (Square, Pulse)
    double edgeTime = 0.0;     // s, 10-90 % edge time (Square, Pulse); 0 = ideal edge
    double noiseRms = 0.0;     // V rms of additive white Gaussian noise
    std::vector<HarmonicComponent> components; // Custom waveform partials
    bool operator==(const GeneratorSettings&) const = default;
};

struct ChannelSettings {
    bool enabled = true;
    double voltsPerDiv = 0.5;  // units/div
    double position = 0.0;     // vertical offset of the 0-level, in divisions from centre
    Coupling coupling = Coupling::DC;
    bool invert = false;
    int input = 0;             // generator index / file column / audio channel
    uint32_t color = 0xFFFFFF; // 0xRRGGBB
    double lineWidth = 1.6;    // px (logical)
    bool operator==(const ChannelSettings&) const = default;
};

struct TimebaseSettings {
    double secPerDiv = 200e-6; // s/div (10 horizontal divisions)
    double delay = 0.0;        // s, horizontal offset of the trigger reference
    double triggerPos = 50.0;  // % of screen width where t = 0 (trigger) sits
    bool autoRoll = true;      // roll mode for slow sweeps (>= 50 ms/div) in AUTO mode
    bool operator==(const TimebaseSettings&) const = default;
};

struct TriggerSettings {
    TriggerMode mode = TriggerMode::Auto;
    TriggerSlope slope = TriggerSlope::Rising;
    int source = 0;              // CH index
    double level = 0.0;          // in source channel units
    double holdoff = 0.0;        // s
    double hysteresisDiv = 0.08; // noise rejection band (divisions of the source channel)
    bool operator==(const TriggerSettings&) const = default;
};

struct AcquisitionSettings {
    SourceKind source = SourceKind::Generator;
    double sampleRate = 1.0e6; // virtual ADC / generator sample rate (Sa/s)
    int refreshRate = 60;      // display/frame rate limit (frames/s)
    bool operator==(const AcquisitionSettings&) const = default;
};

struct MathSettings {
    bool enabled = false;
    MathOp op = MathOp::LowPass;
    int srcA = 0;
    int srcB = 1;
    int order = 4;             // Butterworth order 1..8
    double cutoff = 2000.0;    // Hz  (LPF/HPF cut-off, BPF lower edge)
    double cutoffHigh = 8000.0;// Hz  (BPF upper edge)
    int maLength = 16;         // moving average length (samples)
    double unitsPerDiv = 0.5;
    double position = 0.0;     // divisions
    uint32_t color = 0xFF8A3D;
    bool operator==(const MathSettings&) const = default;
};

struct SpectrumSettings {
    bool enabled = true;
    int source = 0;            // trace index 0..4 (4 = MATH)
    int fftSize = 8192;
    WindowType window = WindowType::Hann;
    bool logScale = true;      // dB (dBV / dBFS) vs linear (Vrms / FSrms)
    double refLevel = 10.0;    // dB at top of the display
    double dbPerDiv = 10.0;
    double fStart = 0.0;       // Hz, displayed span
    double fStop = 20000.0;    // Hz, 0 = Nyquist
    int averages = 1;          // exponential power averaging
    bool spectrogram = true;
    bool operator==(const SpectrumSettings&) const = default;
};

struct DisplaySettings {
    Persistence persistence = Persistence::Off;
    Interpolation interpolation = Interpolation::SinX;
    bool glow = true;
    double gridIntensity = 0.55;   // 0..1
    CursorMode cursorMode = CursorMode::Off;
    int cursorSource = 0;          // trace index
    double cursorX1 = -200e-6, cursorX2 = 200e-6; // s relative to trigger
    double cursorY1 = 0.5, cursorY2 = -0.5;       // source units
    int selectedTrace = 0;
    bool showSpectrum = true;
    bool showMeasurements = true;
    bool showPanel = true;
    bool operator==(const DisplaySettings&) const = default;
};

struct ScopeSettings {
    std::array<ChannelSettings, kNumChannels> ch;
    std::array<GeneratorSettings, kNumGenerators> gen;
    TimebaseSettings timebase;
    TriggerSettings trigger;
    AcquisitionSettings acq;
    MathSettings math;
    SpectrumSettings spectrum;
    DisplaySettings display;
    std::string filePath; // last imported CSV/WAV file

    static ScopeSettings defaults();
    bool operator==(const ScopeSettings&) const = default;
};

// ---- enum <-> string (used by the JSON setup files and the UI) -----------------
const char* toString(Waveform);
const char* toString(Coupling);
const char* toString(TriggerMode);
const char* toString(TriggerSlope);
const char* toString(SourceKind);
const char* toString(WindowType);
const char* toString(MathOp);
const char* toString(CursorMode);
const char* toString(Persistence);
const char* toString(Interpolation);

template <class E> E enumFromString(std::string_view s, E fallback);

// ---- numeric helpers -------------------------------------------------------------
double step125(double v, int steps);   // move along the 1-2-5 sequence
double ceil125(double v);              // smallest 1-2-5 value >= v
double round125(double v);             // nearest 1-2-5 value (log scale)
uint32_t defaultChannelColor(int ch);

// Engineering formatting: formatSI(0.0012, "V") -> "1.200 mV". NaN -> "N/A".
std::string formatSI(double v, std::string_view unit, int significant = 4);
// Unit-aware: units without SI semantics (FS, %, dB) are printed without prefixes.
std::string formatValue(double v, std::string_view unit, int significant = 4);
// Parses "1.5k", "200 mV", "10u", "2 MHz", "1e-3" ... (case-sensitive m / M).
bool parseSI(std::string_view s, double& out);

} // namespace slp
