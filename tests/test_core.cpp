// SIGNAL LAB PRO - automated verification of the signal chain (no GUI required).
#include "TestFramework.h"

#include "Acquisition.h"
#include "Dsp.h"
#include "FileIO.h"
#include "Measurements.h"
#include "RingBuffer.h"
#include "ScopeSettings.h"
#include "SignalGenerator.h"
#include "Trigger.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <numeric>
#include <vector>

using namespace slp;
constexpr double kPi = std::numbers::pi;

namespace {

std::vector<float> gen(const GeneratorSettings& g, double fs, size_t n, uint64_t seed = 1) {
    SignalGenerator sg(seed);
    sg.setSettings(g);
    std::vector<float> v(n);
    sg.generate(v.data(), n, 1.0 / fs);
    return v;
}

GeneratorSettings sine(double f, double vpp, double off = 0.0, double ph = 0.0) {
    GeneratorSettings g;
    g.shape = Waveform::Sine;
    g.frequency = f;
    g.vpp = vpp;
    g.offset = off;
    g.phaseDeg = ph;
    return g;
}

double db(double p) { return 10.0 * std::log10(p); }

void runFor(AcquisitionEngine& e, double& t, double dur, double step = 0.005) {
    const double end = t + dur;
    for (; t < end; t += step) e.tick(t);
}

double valueAt(const TraceData& tr, double t) {
    const double k = (t - tr.t0) / tr.dt;
    const auto i = static_cast<long>(std::floor(k));
    if (i < 0 || i + 1 >= static_cast<long>(tr.y.size())) return kNaN;
    const double a = k - static_cast<double>(i);
    return tr.y[i] * (1 - a) + tr.y[i + 1] * a;
}

ScopeSettings baseSettings() {
    ScopeSettings s = ScopeSettings::defaults();
    for (auto& c : s.ch) { c.enabled = false; c.position = 0; c.coupling = Coupling::DC; }
    s.ch[0].enabled = true;
    s.gen[0] = sine(1000.0, 2.0);
    s.acq.sampleRate = 1e6;
    s.timebase.secPerDiv = 200e-6;
    s.timebase.delay = 0;
    s.timebase.triggerPos = 50;
    s.trigger = TriggerSettings{};
    s.spectrum.enabled = false;
    s.math.enabled = false;
    return s;
}

std::string tmpPath(const char* name) {
    return (std::filesystem::temp_directory_path() / (std::string("slp_test_") + name)).string();
}

} // namespace

// ============================================================ helpers / settings
TEST(format_and_parse) {
    CHECK(formatSI(0.0012, "V") == "1.200 mV");
    CHECK(formatSI(1000.0, "Hz") == "1.000 kHz");
    CHECK(formatSI(999.96, "Hz") == "1.000 kHz");
    CHECK(formatSI(-0.5, "V") == "-500.0 mV");
    CHECK(formatSI(kNaN, "V") == "N/A");
    double v = 0;
    CHECK(parseSI("2 MHz", v)); CHECK_NEAR(v, 2e6, 1e-6);
    CHECK(parseSI("200m", v)); CHECK_NEAR(v, 0.2, 1e-12);
    CHECK(parseSI("10u", v)); CHECK_NEAR(v, 1e-5, 1e-15);
    CHECK(parseSI("1.5k", v)); CHECK_NEAR(v, 1500, 1e-9);
    CHECK(parseSI("5 ms", v)); CHECK_NEAR(v, 5e-3, 1e-15);
    CHECK(parseSI("-3.3e-2 V", v)); CHECK_NEAR(v, -0.033, 1e-12);
    CHECK(!parseSI("abc", v));
    CHECK_NEAR(step125(1.0, 1), 2.0, 1e-12);
    CHECK_NEAR(step125(5.0, 1), 10.0, 1e-12);
    CHECK_NEAR(step125(0.5, -1), 0.2, 1e-12);
    CHECK_NEAR(step125(200e-6, 2), 1e-3, 1e-15);
    CHECK_NEAR(ceil125(0.3125), 0.5, 1e-12);
    CHECK_NEAR(ceil125(2e-4), 2e-4, 1e-15);
    CHECK_NEAR(round125(0.0031), 0.002, 1e-12);
}

TEST(ring_buffer) {
    SampleRing r(16);
    std::vector<float> a(40);
    std::iota(a.begin(), a.end(), 0.0f);
    r.append(a.data(), 10);
    CHECK(r.writeIndex() == 10 && r.validFrom() == 0);
    r.append(a.data() + 10, 10);
    CHECK(r.writeIndex() == 20 && r.validFrom() == 4);
    float out[8];
    r.copy(2, 8, out);
    CHECK(std::isnan(out[0]) && std::isnan(out[1]));
    CHECK(out[2] == 4.0f && out[7] == 9.0f);
    r.copy(16, 8, out);
    CHECK(out[3] == 19.0f && std::isnan(out[4]));
    r.copy(4, 4, out, 4); // strided
    CHECK(out[0] == 4.0f && out[1] == 8.0f && out[3] == 16.0f);
    r.append(a.data(), 40); // larger than capacity: keeps newest 16
    CHECK(r.writeIndex() == 60 && r.validFrom() == 44 && r.at(59) == 39.0f);
    r.restart(100);
    CHECK(!r.contains(59) && r.validFrom() == 100);

    SpscRing<int> q(8);
    int data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, got[10];
    CHECK(q.push(data, 5) == 5);
    CHECK(q.pop(got, 3) == 3 && got[2] == 2);
    CHECK(q.push(data, 10) == 6); // 8 - 2 queued
    CHECK(q.available() == 8);
    CHECK(q.pop(got, 10) == 8 && got[0] == 3 && got[2] == 0);
}

// ============================================================ signal generator
TEST(generator_sine_frequency_amplitude) {
    const double fs = 1e6;
    auto x = gen(sine(1000.0, 2.0, 0.5), fs, 100000);
    auto m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK(m[MeasId::Freq].valid);
    CHECK_NEAR(m[MeasId::Freq].v, 1000.0, 0.01);
    CHECK_NEAR(m[MeasId::Period].v, 1e-3, 1e-8);
    CHECK_NEAR(m[MeasId::PkPk].v, 2.0, 1e-3);
    CHECK_NEAR(m[MeasId::Max].v, 1.5, 1e-4);
    CHECK_NEAR(m[MeasId::Min].v, -0.5, 1e-4);
    CHECK_NEAR(m[MeasId::Mean].v, 0.5, 1e-4);
    CHECK_NEAR(m[MeasId::Rms].v, std::sqrt(0.25 + 0.5), 1e-4);
    CHECK_NEAR(m[MeasId::AcRms].v, std::sqrt(0.5), 1e-4);
    CHECK(!m.twoLevel);
}

TEST(generator_square_duty_levels) {
    GeneratorSettings g;
    g.shape = Waveform::Square;
    g.frequency = 1000;
    g.vpp = 2.0;
    g.duty = 25.0;
    const double fs = 1e6;
    auto x = gen(g, fs, 20000);
    auto m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK(m.twoLevel);
    CHECK_NEAR(m[MeasId::Top].v, 1.0, 1e-6);
    CHECK_NEAR(m[MeasId::Base].v, -1.0, 1e-6);
    CHECK_NEAR(m[MeasId::Duty].v, 25.0, 0.2);
    CHECK_NEAR(m[MeasId::Freq].v, 1000.0, 0.1);
    CHECK_NEAR(m[MeasId::Mean].v, -0.5, 2e-3);
    // ideal edge: rise time is reported as an upper bound (< 1 sample)
    CHECK(m[MeasId::Rise].valid && m[MeasId::Rise].upperBound);
    CHECK_NEAR(m[MeasId::Rise].v, 1.0 / fs, 1e-12);
}

TEST(generator_pulse_edge_time) {
    GeneratorSettings g;
    g.shape = Waveform::Pulse;
    g.frequency = 10e3;
    g.vpp = 3.3;
    g.offset = 1.65;
    g.duty = 30.0;
    g.edgeTime = 2e-6; // 10-90 %
    const double fs = 10e6;
    auto x = gen(g, fs, 50000);
    auto m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::Rise].v, 2e-6, 0.02 * 2e-6);
    CHECK_NEAR(m[MeasId::Fall].v, 2e-6, 0.02 * 2e-6);
    CHECK(!m[MeasId::Rise].upperBound);
    CHECK_NEAR(m[MeasId::Duty].v, 30.0, 0.3);
    CHECK_NEAR(m[MeasId::Top].v, 3.3, 1e-3);
    CHECK_NEAR(m[MeasId::Base].v, 0.0, 1e-3);
    CHECK_NEAR(m[MeasId::Freq].v, 10e3, 1.0);
}

TEST(generator_triangle_sawtooth_rms) {
    const double fs = 1e6;
    GeneratorSettings g;
    g.frequency = 1000;
    g.vpp = 2.0;
    g.shape = Waveform::Triangle;
    auto x = gen(g, fs, 100000);
    auto m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::Rms].v, 1.0 / std::sqrt(3.0), 2e-4);
    CHECK_NEAR(m[MeasId::Freq].v, 1000.0, 0.05);
    CHECK_NEAR(m[MeasId::Rise].v, 0.8 * 0.5e-3, 1e-6); // 10-90 % of a half-period ramp
    g.shape = Waveform::Sawtooth;
    x = gen(g, fs, 100000);
    m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::Rms].v, 1.0 / std::sqrt(3.0), 2e-4);
    CHECK_NEAR(m[MeasId::Freq].v, 1000.0, 0.05);
}

TEST(generator_dc_and_noise) {
    const double fs = 1e6;
    GeneratorSettings g;
    g.shape = Waveform::DC;
    g.offset = 1.25;
    auto x = gen(g, fs, 10000);
    auto m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::Mean].v, 1.25, 1e-6);
    CHECK_NEAR(m[MeasId::PkPk].v, 0.0, 1e-9);
    CHECK(!m[MeasId::Freq].valid && !m[MeasId::Period].valid && !m[MeasId::Rise].valid);

    g.shape = Waveform::Noise;
    g.offset = 0.0;
    g.vpp = 0.6; // sigma = 0.1
    x = gen(g, fs, 200000, 7);
    m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::AcRms].v, 0.1, 0.002);
    CHECK_NEAR(m[MeasId::Mean].v, 0.0, 0.002);
    CHECK(!m[MeasId::Freq].valid); // random crossings are rejected as unstable

    auto s = sine(1000, 2.0);
    s.noiseRms = 0.05;
    x = gen(s, fs, 200000, 3);
    m = measure(x.data(), x.size(), 1.0 / fs);
    CHECK_NEAR(m[MeasId::AcRms].v, std::sqrt(0.5 + 0.0025), 2e-3);
    CHECK_NEAR(m[MeasId::Freq].v, 1000.0, 0.5);
}

TEST(generator_phase_and_continuity) {
    const double fs = 1e6, f = 1000;
    auto a = gen(sine(f, 2.0, 0.0, 0.0), fs, 20000);
    auto b = gen(sine(f, 2.0, 0.0, 90.0), fs, 20000);
    EdgeTrigger ta, tb;
    ta.configure(TriggerSlope::Rising, 0.0, 0.1, 0);
    tb.configure(TriggerSlope::Rising, 0.0, 0.1, 0);
    std::vector<double> ea, eb;
    ta.process(a.data(), a.size(), 0, ea);
    tb.process(b.data(), b.size(), 0, eb);
    CHECK(ea.size() >= 10 && eb.size() >= 10);
    // B leads A by 90 deg = T/4 = 250 samples
    double lag = std::fmod(ea[5] - eb[5] + 1000.0, 1000.0);
    CHECK_NEAR(lag, 250.0, 0.01);

    // frequency change is phase continuous (no jump in the waveform)
    SignalGenerator g(1);
    auto s = sine(1000, 2.0);
    g.setSettings(s);
    std::vector<float> v(2000);
    g.generate(v.data(), 1000, 1.0 / fs);
    s.frequency = 1500;
    g.setSettings(s);
    g.generate(v.data() + 1000, 1000, 1.0 / fs);
    double maxStep = 0;
    for (size_t i = 1; i < v.size(); ++i) maxStep = std::max(maxStep, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
    CHECK(maxStep < 2 * kPi * 1500 / fs * 1.01);

    // advance() over a gap equals generating through it
    SignalGenerator g1(1), g2(1);
    g1.setSettings(sine(1234.5, 2.0));
    g2.setSettings(sine(1234.5, 2.0));
    std::vector<float> skip(77777), o1(100), o2(100);
    g1.generate(skip.data(), skip.size(), 1.0 / fs);
    g1.generate(o1.data(), 100, 1.0 / fs);
    g2.advance(77777.0 / fs);
    g2.generate(o2.data(), 100, 1.0 / fs);
    for (int i = 0; i < 100; ++i) CHECK_NEAR(o1[i], o2[i], 1e-5);
}

TEST(generator_custom_composite) {
    GeneratorSettings g;
    g.shape = Waveform::Custom;
    g.frequency = 1000;
    g.components = {{Waveform::Sine, 1.0, 2.0, 0.0}, {Waveform::Sine, 3.0, 0.5, 0.0}, {Waveform::Sine, 5.0, 0.2, 0.0}};
    const double fs = 102400; // 1 kHz falls exactly on bin 80 for N = 8192
    auto x = gen(g, fs, 8192);
    SpectrumAnalyzer sa;
    sa.configure(8192, WindowType::Hann);
    std::vector<float> p;
    sa.compute(x.data(), p);
    const double bin = fs / 8192;
    auto peaks = findPeaks(p, bin, WindowType::Hann, sa.enbwBins(), 5);
    CHECK(peaks.size() >= 3);
    if (peaks.size() >= 3) {
        CHECK_NEAR(peaks[0].freq, 1000, 0.05 * bin);
        CHECK_NEAR(peaks[1].freq, 3000, 0.05 * bin);
        CHECK_NEAR(peaks[2].freq, 5000, 0.05 * bin);
        CHECK_NEAR(db(peaks[0].power), db(0.5), 0.05);          // 1 V peak
        CHECK_NEAR(db(peaks[1].power), db(0.25 * 0.25 / 2), 0.05); // 0.25 V peak
        CHECK_NEAR(db(peaks[2].power), db(0.1 * 0.1 / 2), 0.05);
    }
}

// ============================================================ measurements
TEST(measurement_na_rules) {
    float one = 1.0f;
    auto m = measure(&one, 1, 1e-6);
    for (auto& v : m.m) CHECK(!v.valid);
    std::vector<float> flat(1000, 0.7f);
    m = measure(flat.data(), flat.size(), 1e-6);
    CHECK(m[MeasId::PkPk].valid && m[MeasId::PkPk].v == 0.0);
    CHECK(!m[MeasId::Freq].valid && !m[MeasId::Duty].valid && !m[MeasId::Rise].valid);
    // only half a period on screen: frequency cannot be determined
    auto half = gen(sine(1000, 2.0), 1e6, 600);
    m = measure(half.data(), half.size(), 1e-6);
    CHECK(!m[MeasId::Freq].valid && !m[MeasId::Period].valid);
    CHECK(m[MeasId::Max].valid && m[MeasId::Rms].valid);
}

// ============================================================ FFT
TEST(fft_matches_naive_dft) {
    for (size_t n : {size_t{64}, size_t{1024}}) {
        std::vector<float> x(n);
        Rng rng(42);
        for (auto& v : x) v = static_cast<float>(rng.gauss());
        RealFFT fft(n);
        std::vector<std::complex<float>> a, b;
        fft.forward(x.data(), a);
        fft.forwardPortable(x.data(), b);
        CHECK(a.size() == n / 2 + 1 && b.size() == n / 2 + 1);
        double maxErr = 0, maxMag = 0;
        for (size_t k = 0; k <= n / 2; ++k) {
            std::complex<double> ref = 0;
            for (size_t i = 0; i < n; ++i)
                ref += static_cast<double>(x[i]) * std::polar(1.0, -2.0 * kPi * double(k) * double(i) / double(n));
            maxMag = std::max(maxMag, std::abs(ref));
            maxErr = std::max(maxErr, std::abs(std::complex<double>(a[k]) - ref));
            maxErr = std::max(maxErr, std::abs(std::complex<double>(b[k]) - ref));
        }
        CHECK(maxErr / maxMag < 2e-5);
    }
    std::printf("  FFT backend: %s\n", RealFFT::backendName());
}

TEST(fft_peak_accuracy_and_normalisation) {
    const double fs = 100e3;
    const size_t n = 8192;
    const double bin = fs / n;
    struct W { WindowType w; double ampTolDb; };
    const W windows[] = {{WindowType::Hann, 0.02}, {WindowType::Hamming, 0.05}, {WindowType::BlackmanHarris, 0.02},
                         {WindowType::FlatTop, 0.02}};
    for (double f : {1000.0, 100 * bin, 12345.6}) {
        auto x = gen(sine(f, 2.0), fs, n); // 1 V peak -> 0.5 Vrms^2 -> -3.01 dBV
        for (const auto& w : windows) {
            SpectrumAnalyzer sa;
            sa.configure(n, w.w);
            std::vector<float> p;
            sa.compute(x.data(), p);
            CHECK(p.size() == n / 2 + 1);
            auto peaks = findPeaks(p, bin, w.w, sa.enbwBins(), 1);
            CHECK(!peaks.empty());
            if (peaks.empty()) continue;
            CHECK_NEAR(peaks[0].freq, f, 0.01 * bin);
            CHECK_NEAR(db(peaks[0].power), db(0.5), w.ampTolDb);
        }
    }
    // bin-centred tone with rectangular window reads exactly; DC bin normalisation
    GeneratorSettings g = sine(100 * bin, 2.0, 0.3);
    auto x = gen(g, fs, n);
    SpectrumAnalyzer sa;
    sa.configure(n, WindowType::Rectangular);
    std::vector<float> p;
    sa.compute(x.data(), p);
    CHECK_NEAR(p[100], 0.5, 1e-4);
    CHECK_NEAR(p[0], 0.09, 1e-4);
    CHECK_NEAR(sa.enbwBins(), 1.0, 1e-9);
    // Hann: coherent gain 0.5, ENBW 1.5 bins; DC still reads 0.09 V^2
    sa.configure(n, WindowType::Hann);
    sa.compute(x.data(), p);
    CHECK_NEAR(sa.coherentGain(), 0.5, 1e-6);
    CHECK_NEAR(sa.enbwBins(), 1.5, 1e-6);
    CHECK_NEAR(p[100], 0.5, 1e-4);
    CHECK_NEAR(p[0], 0.09, 1e-3);
    // frequency axis: fs/4 lands on bin N/4, Nyquist bin = N/2
    x = gen(sine(fs / 4, 2.0), fs, n);
    sa.compute(x.data(), p);
    auto pk = findPeaks(p, bin, WindowType::Hann, sa.enbwBins(), 1);
    CHECK(!pk.empty() && pk[0].bin == static_cast<int>(n / 4));
    CHECK_NEAR((p.size() - 1) * bin, fs / 2, 1e-9);
}

TEST(fft_harmonics_thd) {
    GeneratorSettings g;
    g.shape = Waveform::Custom;
    g.frequency = 1000;
    g.components = {{Waveform::Sine, 1, 2.0, 0}, {Waveform::Sine, 3, 0.2, 0}, {Waveform::Sine, 5, 0.1, 0}};
    const double fs = 100e3;
    const size_t n = 16384;
    auto x = gen(g, fs, n);
    SpectrumAnalyzer sa;
    sa.configure(n, WindowType::BlackmanHarris);
    std::vector<float> p;
    sa.compute(x.data(), p);
    auto h = analyzeHarmonics(p, fs / n, WindowType::BlackmanHarris, sa.enbwBins(), 10);
    CHECK(h.valid);
    CHECK_NEAR(h.fundamental, 1000.0, 1.0);
    CHECK_NEAR(h.thdRatio, std::sqrt(0.01 + 0.0025), 0.002);
    CHECK(h.harmonics.size() >= 5);
    if (h.harmonics.size() >= 5) {
        CHECK_NEAR(db(h.harmonics[2].power / h.harmonics[0].power), -20.0, 0.1); // H3 = -20 dBc
        CHECK(db(h.harmonics[1].power / h.harmonics[0].power) < -100.0);          // H2 absent
    }
}

// ============================================================ trigger
TEST(trigger_edge_detection_interpolated) {
    const double fs = 1e6, f = 1000;
    auto x = gen(sine(f, 2.0), fs, 10000);
    EdgeTrigger t;
    std::vector<double> ev;
    t.configure(TriggerSlope::Rising, 0.5, 0.05, 0);
    t.process(x.data(), x.size(), 0, ev);
    CHECK(ev.size() == 10);
    const double tr = std::asin(0.5) / (2 * kPi * f) * fs;
    for (size_t k = 0; k < ev.size(); ++k) CHECK_NEAR(ev[k], tr + 1000.0 * static_cast<double>(k), 0.01);
    ev.clear();
    t.configure(TriggerSlope::Falling, 0.5, 0.05, 0);
    t.reset();
    t.process(x.data(), x.size(), 0, ev);
    const double tf = (kPi - std::asin(0.5)) / (2 * kPi * f) * fs;
    CHECK(!ev.empty());
    for (size_t k = 0; k < ev.size(); ++k) CHECK_NEAR(ev[k], tf + 1000.0 * static_cast<double>(k), 0.01);
    // processing in two blocks gives identical results (state carried across blocks)
    std::vector<double> e1, e2;
    EdgeTrigger a, b;
    a.configure(TriggerSlope::Rising, 0.5, 0.05, 0);
    b.configure(TriggerSlope::Rising, 0.5, 0.05, 0);
    a.process(x.data(), x.size(), 0, e1);
    b.process(x.data(), 4321, 0, e2);
    b.process(x.data() + 4321, x.size() - 4321, 4321, e2);
    CHECK(e1 == e2);
}

TEST(trigger_hysteresis_and_holdoff) {
    const double fs = 1e6;
    auto s = sine(1000, 2.0);
    s.noiseRms = 0.05;
    auto x = gen(s, fs, 20000, 11);
    EdgeTrigger t;
    std::vector<double> ev;
    t.configure(TriggerSlope::Rising, 0.0, 0.0, 0); // no hysteresis: noise re-triggers
    t.process(x.data(), x.size(), 0, ev);
    CHECK(ev.size() > 20);
    ev.clear();
    t.configure(TriggerSlope::Rising, 0.0, 0.3, 0); // noise rejection
    t.reset();
    t.process(x.data(), x.size(), 0, ev);
    CHECK(ev.size() == 20);

    auto y = gen(sine(10e3, 2.0), fs, 3000);
    ev.clear();
    t.configure(TriggerSlope::Rising, 0.0, 0.1, 250e-6 * fs);
    t.reset();
    t.process(y.data(), y.size(), 0, ev);
    CHECK(ev.size() == 10); // every third period (300 us)
    for (size_t k = 1; k < ev.size(); ++k) CHECK_NEAR(ev[k] - ev[k - 1], 300.0, 0.01);
}

// ============================================================ acquisition engine
TEST(engine_triggered_frame_alignment) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.trigger.level = 0.3;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.3);
    auto f = e.latestFrame();
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(f->state == TrigState::Triggered);
    const auto& tr = f->tr[0];
    CHECK(tr.present);
    CHECK_NEAR(valueAt(tr, 0.0), 0.3, 2e-3);              // waveform crosses the level at t = 0
    CHECK(valueAt(tr, 2e-6) > valueAt(tr, -2e-6));        // rising edge
    CHECK_NEAR(f->tStart, -1e-3, 1e-12);
    CHECK_NEAR(f->tEnd, 1e-3, 1e-12);
    CHECK_NEAR(tr.dt, 1e-6, 1e-15);
    CHECK_NEAR(tr.meas[MeasId::Freq].v, 1000.0, 0.1);
    CHECK_NEAR(tr.meas[MeasId::PkPk].v, 2.0, 0.01);
    CHECK_NEAR(tr.meas[MeasId::Rms].v, std::sqrt(0.5), 2e-3);
    // trigger position 10 %, falling slope
    s.timebase.triggerPos = 10;
    s.trigger.slope = TriggerSlope::Falling;
    e.setSettings(s);
    runFor(e, t, 0.2);
    f = e.latestFrame();
    CHECK_NEAR(f->tStart, -0.2e-3, 1e-12);
    CHECK_NEAR(valueAt(f->tr[0], 0.0), 0.3, 2e-3);
    CHECK(valueAt(f->tr[0], 2e-6) < valueAt(f->tr[0], -2e-6));
    // pre-trigger history: 90 % of the screen before the trigger is filled with data
    s.timebase.triggerPos = 90;
    e.setSettings(s);
    runFor(e, t, 0.2);
    f = e.latestFrame();
    CHECK(std::isfinite(valueAt(f->tr[0], -1.79e-3)));
}

TEST(engine_trigger_modes) {
    auto s = baseSettings();
    s.gen[0].shape = Waveform::DC;
    s.gen[0].offset = 0.2;
    s.trigger.level = 0.5; // never crossed
    {   // NORMAL: no trigger -> no frame, WAIT
        AcquisitionEngine e(false);
        s.trigger.mode = TriggerMode::Normal;
        e.setSettings(s);
        double t = 0;
        runFor(e, t, 0.5);
        CHECK(e.latestFrame() == nullptr);
        CHECK(e.status().state == TrigState::Waiting);
        e.forceTrigger(); // FORCE produces one frame
        runFor(e, t, 0.05);
        CHECK(e.latestFrame() != nullptr && e.latestFrame()->forced);
    }
    {   // AUTO: free-runs after the auto timeout
        AcquisitionEngine e(false);
        s.trigger.mode = TriggerMode::Auto;
        e.setSettings(s);
        double t = 0;
        runFor(e, t, 0.5);
        auto f = e.latestFrame();
        CHECK(f != nullptr && f->state == TrigState::Auto);
        if (f) CHECK_NEAR(f->tr[0].meas[MeasId::Mean].v, 0.2, 1e-6);
    }
    {   // SINGLE: capture once then stop
        AcquisitionEngine e(false);
        auto s2 = baseSettings();
        s2.trigger.mode = TriggerMode::Single;
        e.setSettings(s2);
        e.single();
        double t = 0;
        runFor(e, t, 0.3);
        auto f = e.latestFrame();
        CHECK(f != nullptr && f->state == TrigState::Triggered);
        CHECK(!e.status().running && e.status().state == TrigState::Stopped);
        const auto seq = f ? f->seq : 0;
        runFor(e, t, 0.3);
        CHECK(e.latestFrame()->seq == seq); // no new acquisitions after the single shot
        // while stopped, changing the timebase re-reads the stored record (zoom)
        s2.timebase.secPerDiv = 100e-6;
        e.setSettings(s2);
        runFor(e, t, 0.02);
        CHECK(e.latestFrame()->seq > seq);
        CHECK_NEAR(e.latestFrame()->tEnd - e.latestFrame()->tStart, 1e-3, 1e-12);
        CHECK_NEAR(valueAt(e.latestFrame()->tr[0], 0.0), 0.0, 2e-3);
    }
}

TEST(engine_delay_offset) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.trigger.level = 0.0;
    s.timebase.delay = 1e-3; // trigger point at the left screen edge
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.3);
    auto f = e.latestFrame();
    CHECK(f && f->state == TrigState::Triggered);
    if (!f) return;
    CHECK_NEAR(f->tStart, 0.0, 1e-12);
    CHECK_NEAR(valueAt(f->tr[0], 0.0), 0.0, 2e-3);
    CHECK_NEAR(valueAt(f->tr[0], 0.25e-3), 1.0, 2e-3); // quarter period later: peak
}

TEST(engine_sample_rate_change) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.acq.sampleRate = 100e3;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.3);
    auto f = e.latestFrame();
    CHECK(f && std::fabs(f->fs - 100e3) < 1e-6);
    if (f) {
        CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 1000.0, 0.5);
        CHECK_NEAR(f->tr[0].dt, 1e-5, 1e-15);
        CHECK(f->tr[0].y.size() >= 200 && f->tr[0].y.size() <= 204);
    }
    s.acq.sampleRate = 20e6;
    e.setSettings(s);
    runFor(e, t, 0.5);
    f = e.latestFrame();
    CHECK(f && std::fabs(f->fs - 20e6) < 1e-6);
    if (f) {
        CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 1000.0, 0.05);
        CHECK_NEAR(f->tr[0].dt, 5e-8, 1e-18);
        CHECK(f->tr[0].y.size() >= 40000 && f->tr[0].y.size() <= 40004);
    }
    CHECK(e.status().segmented); // 20 MSa/s exceeds the real-time generation budget
}

TEST(engine_decimation_and_roll) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.gen[0].frequency = 5.0;
    s.timebase.secPerDiv = 0.2; // 2 s window at 1 MSa/s = 2 Mpts > 1 Mpt -> decimate by 2
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 2.5, 0.01);
    auto f = e.latestFrame();
    CHECK(f != nullptr);
    if (!f) return;
    CHECK(f->decim == 2);
    CHECK_NEAR(f->fs, 500e3, 1e-6);
    CHECK(f->state == TrigState::Roll);
    CHECK(f->tr[0].y.size() <= kMaxRecord + 8);
    CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 5.0, 0.01);
}

TEST(engine_multichannel) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    const double fr[4] = {1000, 2000, 3000, 5000}, amp[4] = {1, 2, 3, 4};
    for (int c = 0; c < 4; ++c) {
        s.ch[c].enabled = true;
        s.ch[c].input = c;
        s.gen[c] = sine(fr[c], amp[c], 0.1 * c);
    }
    s.gen[2].shape = Waveform::Square;
    s.timebase.secPerDiv = 1e-3;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.3);
    auto f = e.latestFrame();
    CHECK(f != nullptr);
    if (!f) return;
    for (int c = 0; c < 4; ++c) {
        CHECK(f->tr[c].present);
        CHECK_NEAR(f->tr[c].meas[MeasId::Freq].v, fr[c], 0.05 * fr[c] / 100);
        CHECK_NEAR(f->tr[c].meas[MeasId::PkPk].v, amp[c], 0.01 * amp[c]);
        CHECK_NEAR(f->tr[c].meas[MeasId::Mean].v, 0.1 * c, 0.01);
    }
    // routing: CH2 fed from GEN1 shows the identical waveform
    s.ch[1].input = 0;
    e.setSettings(s);
    runFor(e, t, 0.2);
    f = e.latestFrame();
    for (size_t i = 0; i < f->tr[0].y.size(); i += 97) CHECK(f->tr[0].y[i] == f->tr[1].y[i]);
}

TEST(engine_ac_coupling) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.gen[0] = sine(1000, 2.0, 1.0);
    s.trigger.level = 1.0;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.2);
    CHECK_NEAR(e.latestFrame()->tr[0].meas[MeasId::Mean].v, 1.0, 2e-3);
    s.ch[0].coupling = Coupling::AC;
    s.trigger.level = 0.0;
    e.setSettings(s);
    runFor(e, t, 0.4);
    auto f = e.latestFrame();
    CHECK_NEAR(f->tr[0].meas[MeasId::Mean].v, 0.0, 0.01);
    CHECK_NEAR(f->tr[0].meas[MeasId::PkPk].v, 2.0, 0.02);
    s.ch[0].coupling = Coupling::GND;
    e.setSettings(s);
    runFor(e, t, 0.4);
    CHECK_NEAR(e.latestFrame()->tr[0].meas[MeasId::PkPk].v, 0.0, 1e-9);
}

TEST(engine_segmented_high_rate) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.acq.sampleRate = 100e6;
    s.gen[0] = sine(1e6, 2.0);
    s.timebase.secPerDiv = 200e-9;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.5);
    auto f = e.latestFrame();
    CHECK(f && f->state == TrigState::Triggered);
    if (!f) return;
    CHECK(e.status().segmented);
    CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 1e6, 100.0);
    CHECK_NEAR(valueAt(f->tr[0], 0.0), 0.0, 0.01);
}

TEST(engine_autoscale) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.gen[0] = sine(1000, 2.0, 0.5);
    s.ch[0].voltsPerDiv = 5.0;
    s.timebase.secPerDiv = 50e-3;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.1);
    e.requestAutoscale();
    runFor(e, t, 0.02);
    auto r = e.takeAutoscaleResult();
    CHECK(r.has_value());
    if (!r) return;
    CHECK_NEAR(r->ch[0].voltsPerDiv, 0.5, 1e-12);
    CHECK_NEAR(r->ch[0].position, -1.0, 1e-3);
    CHECK_NEAR(r->timebase.secPerDiv, 200e-6, 1e-12);
    CHECK_NEAR(r->trigger.level, 0.5, 1e-3);
    CHECK(r->trigger.source == 0 && r->trigger.mode == TriggerMode::Auto);
}

// ============================================================ filters / math
TEST(filter_frequency_response) {
    const double fs = 48000, fc = 1000;
    for (int order = 1; order <= 8; ++order) {
        auto lp = IirFilter::butterworthLowpass(order, fc, fs);
        CHECK_NEAR(lp.magnitudeDb(fc, fs), -3.0103, 0.01);
        CHECK(lp.magnitudeDb(fc / 10, fs) > -0.05);
        CHECK(lp.magnitudeDb(fc * 10, fs) < -(20.0 * order - 1.0));
        auto hp = IirFilter::butterworthHighpass(order, fc, fs);
        CHECK_NEAR(hp.magnitudeDb(fc, fs), -3.0103, 0.01);
        CHECK(hp.magnitudeDb(fc * 10, fs) > -0.05);
        CHECK(hp.magnitudeDb(fc / 10, fs) < -(20.0 * order - 1.0));
        CHECK(lp.sections() == static_cast<size_t>((order + 1) / 2));
    }
    auto bp = IirFilter::butterworthBandpass(4, 1000, 4000, fs);
    CHECK(bp.magnitudeDb(2000, fs) > -0.1);
    CHECK_NEAR(bp.magnitudeDb(1000, fs), -3.01, 0.15);
    CHECK_NEAR(bp.magnitudeDb(4000, fs), -3.01, 0.15);
    CHECK(bp.magnitudeDb(100, fs) < -70);
}

TEST(filter_time_domain) {
    const double fs = 100e3;
    MathSettings m;
    m.op = MathOp::LowPass;
    m.order = 4;
    m.cutoff = 1000;
    auto x = gen(sine(1000, 2.0), fs, 20000);
    auto r = computeMath(m, x.data(), x.data(), x.size(), 10000, fs, "V", "V");
    auto meas = measure(r.y.data(), r.y.size(), 1 / fs);
    CHECK_NEAR(meas[MeasId::PkPk].v, 2.0 * std::sqrt(0.5), 0.01);
    CHECK(r.unit == "V" && !r.warning);
    // 10 kHz is attenuated by > 79 dB (4th order, 1 decade)
    x = gen(sine(10000, 2.0), fs, 20000);
    r = computeMath(m, x.data(), x.data(), x.size(), 10000, fs, "V", "V");
    meas = measure(r.y.data(), r.y.size(), 1 / fs);
    CHECK(meas[MeasId::PkPk].v < 2.0 * 1.2e-4);
    // high-pass removes DC completely after settling
    m.op = MathOp::HighPass;
    m.cutoff = 100;
    x = gen(sine(2000, 2.0, 3.0), fs, 30000);
    r = computeMath(m, x.data(), x.data(), x.size(), 20000, fs, "V", "V");
    meas = measure(r.y.data(), r.y.size(), 1 / fs);
    CHECK_NEAR(meas[MeasId::Mean].v, 0.0, 0.01);
    // cut-off above Nyquist is clamped and flagged
    m.op = MathOp::LowPass;
    m.cutoff = 80e3;
    r = computeMath(m, x.data(), x.data(), x.size(), 0, fs, "V", "V");
    CHECK(r.warning);
    // moving average: first null at fs / N
    m.op = MathOp::MovingAverage;
    m.maLength = 100;
    x = gen(sine(1000, 2.0), fs, 20000);
    r = computeMath(m, x.data(), x.data(), x.size(), 100, fs, "V", "V");
    meas = measure(r.y.data(), r.y.size(), 1 / fs);
    CHECK(meas[MeasId::PkPk].v < 1e-3);
}

TEST(math_arithmetic_derivative_integral) {
    const double fs = 1e6;
    auto a = gen(sine(1000, 2.0), fs, 10000);
    auto b = gen(sine(1000, 2.0, 0.0, 180.0), fs, 10000);
    MathSettings m;
    m.op = MathOp::Add;
    auto r = computeMath(m, a.data(), a.data(), a.size(), 0, fs, "V", "V");
    CHECK_NEAR(measure(r.y.data(), r.y.size(), 1 / fs)[MeasId::PkPk].v, 4.0, 1e-3);
    r = computeMath(m, a.data(), b.data(), a.size(), 0, fs, "V", "V");
    CHECK(measure(r.y.data(), r.y.size(), 1 / fs)[MeasId::PkPk].v < 1e-5);
    m.op = MathOp::Subtract;
    r = computeMath(m, a.data(), b.data(), a.size(), 0, fs, "V", "V");
    CHECK_NEAR(measure(r.y.data(), r.y.size(), 1 / fs)[MeasId::PkPk].v, 4.0, 1e-3);
    m.op = MathOp::Multiply;
    r = computeMath(m, a.data(), a.data(), a.size(), 0, fs, "V", "V");
    CHECK_NEAR(measure(r.y.data(), r.y.size(), 1 / fs)[MeasId::Mean].v, 0.5, 1e-3);
    CHECK(r.unit == "V\xC2\xB2");
    m.op = MathOp::Derivative;
    r = computeMath(m, a.data(), a.data(), a.size(), 1, fs, "V", "V");
    auto meas = measure(r.y.data(), r.y.size(), 1 / fs);
    CHECK_NEAR(meas[MeasId::Max].v, 2 * kPi * 1000.0, 2 * kPi * 1000.0 * 1e-3);
    CHECK(r.unit == "V/s");
    m.op = MathOp::Integral;
    std::vector<float> dc(1001, 2.0f);
    r = computeMath(m, dc.data(), dc.data(), dc.size(), 0, fs, "V", "V");
    CHECK_NEAR(r.y.back(), 2.0 * 1000 / fs, 1e-9);
    // NaN gaps propagate instead of corrupting the result
    std::vector<float> gap(a);
    gap[5000] = std::numeric_limits<float>::quiet_NaN();
    m.op = MathOp::LowPass;
    m.cutoff = 5000;
    r = computeMath(m, gap.data(), gap.data(), gap.size(), 0, fs, "V", "V");
    CHECK(std::isnan(r.y[5000]) && std::isfinite(r.y[5001]) && std::isfinite(r.y.back()));
}

TEST(engine_math_and_spectrum) {
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.ch[1].enabled = true;
    s.ch[1].input = 1;
    s.gen[1] = sine(1000, 1.0, 0.0, 0.0);
    s.math.enabled = true;
    s.math.op = MathOp::Add;
    s.math.srcA = 0;
    s.math.srcB = 1;
    s.spectrum.enabled = true;
    s.spectrum.source = kMathTrace;
    s.spectrum.fftSize = 16384;
    s.spectrum.window = WindowType::FlatTop;
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.4);
    auto f = e.latestFrame();
    CHECK(f && f->tr[kMathTrace].present);
    if (f) CHECK_NEAR(f->tr[kMathTrace].meas[MeasId::PkPk].v, 3.0, 0.01);
    auto sp = e.latestSpectrum();
    CHECK(sp && sp->valid);
    if (sp && sp->valid && !sp->peaks.empty()) {
        CHECK_NEAR(sp->peaks[0].freq, 1000.0, 0.1 * sp->binHz);
        CHECK_NEAR(db(sp->peaks[0].power), db(1.5 * 1.5 / 2), 0.05);
        CHECK_NEAR(sp->fs / 2, 500e3, 1e-6);
    }
    // filter comparison: LPF on CH1 square, spectrum of MATH loses harmonics
    s.gen[0].shape = Waveform::Square;
    s.math.op = MathOp::LowPass;
    s.math.srcA = 0;
    s.math.order = 6;
    s.math.cutoff = 2000;
    s.spectrum.window = WindowType::BlackmanHarris;
    e.setSettings(s);
    runFor(e, t, 0.4);
    sp = e.latestSpectrum();
    CHECK(sp && sp->valid && sp->harmonics.valid);
    if (sp && sp->harmonics.harmonics.size() >= 3) {
        const auto& h = sp->harmonics.harmonics;
        // ideal square: H3 = -9.54 dBc; 6th order LPF @2 kHz attenuates 3 kHz by ~21 dB more
        CHECK(db(h[2].power / h[0].power) < -25.0);
    }
}

// ============================================================ file I/O
TEST(file_csv_roundtrip) {
    const double fs = 50e3;
    auto a = gen(sine(1000, 2.0, 0.25), fs, 5000);
    auto b = gen(sine(500, 1.0), fs, 5000);
    const std::string p = tmpPath("wave.csv");
    std::string err;
    CHECK(writeCsv(p, {{"CH1", "V", a.data()}, {"CH2", "V", b.data()}}, a.size(), -0.05, 1.0 / fs, {"test"}, err));
    SignalData d;
    CHECK(readCsv(p, d, err));
    CHECK(d.channels.size() == 2 && d.length() == 5000);
    CHECK_NEAR(d.sampleRate, fs, 1e-3);
    CHECK(d.names[0] == "CH1" && d.units[1] == "V");
    double maxErr = 0;
    for (size_t i = 0; i < a.size(); ++i) maxErr = std::max(maxErr, std::fabs(static_cast<double>(d.channels[0][i]) - a[i]));
    CHECK(maxErr < 1e-6);

    // header-less file with a time column, semicolon separator and decimal comma
    const std::string q = tmpPath("euro.csv");
    FILE* f = std::fopen(q.c_str(), "wb");
    for (int i = 0; i < 100; ++i) std::fprintf(f, "%d,%03d;%d,5\n", i / 1000, i % 1000, i);
    std::fclose(f);
    CHECK(readCsv(q, d, err));
    CHECK_NEAR(d.sampleRate, 1000.0, 1e-6);
    CHECK(d.channels.size() == 1);
    CHECK_NEAR(d.channels[0][10], 10.5, 1e-6);

    // values only: sample rate from the default argument
    const std::string r = tmpPath("plain.csv");
    f = std::fopen(r.c_str(), "wb");
    std::fprintf(f, "a\n");
    for (int i = 0; i < 50; ++i) std::fprintf(f, "%g\n", std::sin(i * 0.1));
    std::fclose(f);
    CHECK(readCsv(r, d, err, 2000.0));
    CHECK(d.channels.size() == 1 && d.length() == 50 && d.sampleRate == 2000.0);
}

TEST(file_wav_roundtrip) {
    const double fs = 48000;
    auto a = gen(sine(1000, 2.0, 0.25), fs, 4800);
    auto b = gen(sine(250, 0.5), fs, 4800);
    std::string err;
    double vfs = 0;
    const std::string p = tmpPath("wave32.wav");
    CHECK(writeWav(p, {{"CH1", "V", a.data()}, {"CH2", "V", b.data()}}, a.size(), fs, 32, 0.0, err, &vfs));
    CHECK_NEAR(vfs, 2.0, 1e-12); // peak 1.25 V -> 2 V full scale
    SignalData d;
    CHECK(readWav(p, d, err));
    CHECK(d.channels.size() == 2 && d.length() == 4800 && d.sampleRate == fs);
    CHECK(d.units[0] == "V");
    double maxErr = 0;
    for (size_t i = 0; i < a.size(); ++i) maxErr = std::max(maxErr, std::fabs(static_cast<double>(d.channels[0][i]) - a[i]));
    CHECK(maxErr < 1e-6);

    const std::string q = tmpPath("wave16.wav");
    CHECK(writeWav(q, {{"CH1", "V", a.data()}}, a.size(), fs, 16, 2.0, err));
    CHECK(readWav(q, d, err));
    maxErr = 0;
    for (size_t i = 0; i < a.size(); ++i) maxErr = std::max(maxErr, std::fabs(static_cast<double>(d.channels[0][i]) - a[i]));
    CHECK(maxErr <= 2.0 / 32767.0);

    // 3 channels -> WAVE_FORMAT_EXTENSIBLE
    const std::string r = tmpPath("wave3.wav");
    CHECK(writeWav(r, {{"A", "V", a.data()}, {"B", "V", b.data()}, {"C", "V", a.data()}}, a.size(), fs, 32, 0, err));
    CHECK(readWav(r, d, err) && d.channels.size() == 3);

    // foreign 16-bit PCM file without the voltage tag -> full-scale units
    std::vector<uint8_t> w;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back((v >> (8 * i)) & 0xFF); };
    auto put16 = [&](uint16_t v) { w.push_back(v & 0xFF); w.push_back(v >> 8); };
    const char* riff = "RIFF";
    w.insert(w.end(), riff, riff + 4); put32(36 + 8);
    w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(16);
    put16(1); put16(1); put32(8000); put32(16000); put16(2); put16(16);
    w.insert(w.end(), {'d', 'a', 't', 'a'}); put32(8);
    put16(16384); put16(static_cast<uint16_t>(-16384)); put16(32767); put16(0);
    const std::string s = tmpPath("foreign.wav");
    FILE* f = std::fopen(s.c_str(), "wb");
    std::fwrite(w.data(), 1, w.size(), f);
    std::fclose(f);
    CHECK(readWav(s, d, err));
    CHECK(d.units[0] == "FS" && d.sampleRate == 8000);
    CHECK_NEAR(d.channels[0][0], 0.5, 1e-6);
    CHECK_NEAR(d.channels[0][1], -0.5, 1e-6);
}

TEST(engine_file_source) {
    const double fs = 48000;
    auto d = std::make_shared<SignalData>();
    d->sampleRate = fs;
    d->channels.push_back(gen(sine(440, 1.0), fs, 48000));
    d->names = {"WAV1"};
    d->units = {"FS"};
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.acq.source = SourceKind::File;
    s.timebase.secPerDiv = 1e-3;
    e.setFileData(d);
    e.setSettings(s);
    double t = 0;
    runFor(e, t, 0.3);
    auto f = e.latestFrame();
    CHECK(f && f->state == TrigState::Triggered);
    if (!f) return;
    CHECK(f->tr[0].unit == "FS");
    CHECK_NEAR(f->fs, fs, 1e-9);
    CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 440.0, 0.1);
    CHECK_NEAR(f->tr[0].meas[MeasId::PkPk].v, 1.0, 0.01);
}

TEST(engine_push_source) {
    auto push = std::make_shared<PushSource>();
    push->configure(48000, 2);
    AcquisitionEngine e(false);
    auto s = baseSettings();
    s.acq.source = SourceKind::Audio;
    s.timebase.secPerDiv = 1e-3;
    s.ch[1].enabled = true;
    s.ch[1].input = 1;
    e.setPushSource(push);
    e.setSettings(s);
    SignalGenerator gl(1), gr(2);
    gl.setSettings(sine(1000, 1.0));
    gr.setSettings(sine(250, 0.5));
    double t = 0;
    std::vector<float> l(240), r(240), inter(480);
    for (int k = 0; k < 120; ++k) { // 0.6 s in 5 ms chunks
        gl.generate(l.data(), 240, 1.0 / 48000);
        gr.generate(r.data(), 240, 1.0 / 48000);
        for (int i = 0; i < 240; ++i) { inter[2 * i] = l[i]; inter[2 * i + 1] = r[i]; }
        push->push(inter.data(), 240);
        e.tick(t);
        t += 0.005;
    }
    auto f = e.latestFrame();
    CHECK(f && f->state == TrigState::Triggered);
    if (!f) return;
    CHECK(f->tr[0].unit == "FS");
    CHECK_NEAR(f->tr[0].meas[MeasId::Freq].v, 1000.0, 0.5);
    CHECK_NEAR(f->tr[1].meas[MeasId::Freq].v, 250.0, 0.5);
    CHECK_NEAR(f->tr[1].meas[MeasId::PkPk].v, 0.5, 0.01);
}

int main(int argc, char** argv) { return slptest::runAll(argc, argv); }
