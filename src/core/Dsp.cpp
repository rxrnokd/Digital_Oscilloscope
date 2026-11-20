#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>

#if defined(SLP_HAVE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#endif

namespace slp {

namespace {
constexpr double kPi = std::numbers::pi;
} // namespace

// ================================================================== RealFFT
struct RealFFT::Impl {
    int log2n = 0;
    size_t m = 0; // n/2 (size of the packed complex FFT)
    std::vector<uint32_t> rev;
    std::vector<float> twr, twi;     // exp(-j 2 pi k / m), k < m/2
    std::vector<float> postr, posti; // exp(-j 2 pi k / n), k <= m
    std::vector<float> zr, zi;
#if defined(SLP_HAVE_ACCELERATE)
    FFTSetup setup = nullptr;
    std::vector<float> re, im;
#endif
};

RealFFT::RealFFT(size_t n) : n_(n), d_(std::make_unique<Impl>()) {
    if (n < 8 || (n & (n - 1)) != 0) throw std::invalid_argument("RealFFT size must be a power of two >= 8");
    auto& d = *d_;
    while ((size_t{1} << d.log2n) < n) ++d.log2n;
    d.m = n / 2;
    const size_t m = d.m;
    int bits = d.log2n - 1;
    d.rev.resize(m);
    for (size_t i = 0; i < m; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (size_t{1} << b)) r |= 1u << (bits - 1 - b);
        d.rev[i] = r;
    }
    d.twr.resize(m / 2);
    d.twi.resize(m / 2);
    for (size_t k = 0; k < m / 2; ++k) {
        const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(m);
        d.twr[k] = static_cast<float>(std::cos(a));
        d.twi[k] = static_cast<float>(std::sin(a));
    }
    d.postr.resize(m + 1);
    d.posti.resize(m + 1);
    for (size_t k = 0; k <= m; ++k) {
        const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        d.postr[k] = static_cast<float>(std::cos(a));
        d.posti[k] = static_cast<float>(std::sin(a));
    }
    d.zr.resize(m);
    d.zi.resize(m);
#if defined(SLP_HAVE_ACCELERATE)
    d.setup = vDSP_create_fftsetup(static_cast<vDSP_Length>(d.log2n), kFFTRadix2);
    d.re.resize(m);
    d.im.resize(m);
#endif
}

RealFFT::~RealFFT() {
#if defined(SLP_HAVE_ACCELERATE)
    if (d_ && d_->setup) vDSP_destroy_fftsetup(d_->setup);
#endif
}

const char* RealFFT::backendName() {
#if defined(SLP_HAVE_ACCELERATE)
    return "Apple Accelerate vDSP";
#else
    return "portable radix-2";
#endif
}

void RealFFT::forward(const float* in, std::vector<std::complex<float>>& out) {
#if defined(SLP_HAVE_ACCELERATE)
    auto& d = *d_;
    const size_t m = d.m;
    DSPSplitComplex sc;
    sc.realp = d.re.data();
    sc.imagp = d.im.data();
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(in), 2, &sc, 1, static_cast<vDSP_Length>(m));
    vDSP_fft_zrip(d.setup, &sc, 1, static_cast<vDSP_Length>(d.log2n), kFFTDirection_Forward);
    // vDSP's real FFT returns 2*X[k]; DC is packed in realp[0] and Nyquist in imagp[0].
    out.resize(m + 1);
    out[0] = {0.5f * d.re[0], 0.0f};
    out[m] = {0.5f * d.im[0], 0.0f};
    for (size_t k = 1; k < m; ++k) out[k] = {0.5f * d.re[k], 0.5f * d.im[k]};
#else
    forwardPortable(in, out);
#endif
}

void RealFFT::forwardPortable(const float* in, std::vector<std::complex<float>>& out) {
    auto& d = *d_;
    const size_t m = d.m;
    float* zr = d.zr.data();
    float* zi = d.zi.data();
    // pack even/odd samples into one complex sequence (bit-reversed order)
    for (size_t k = 0; k < m; ++k) {
        const uint32_t r = d.rev[k];
        zr[r] = in[2 * k];
        zi[r] = in[2 * k + 1];
    }
    // iterative radix-2 decimation-in-time
    for (size_t len = 2; len <= m; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = m / len;
        for (size_t i = 0; i < m; i += len) {
            for (size_t j = 0; j < half; ++j) {
                const float wr = d.twr[j * step], wi = d.twi[j * step];
                const size_t a = i + j, b = a + half;
                const float vr = zr[b] * wr - zi[b] * wi;
                const float vi = zr[b] * wi + zi[b] * wr;
                zr[b] = zr[a] - vr;
                zi[b] = zi[a] - vi;
                zr[a] += vr;
                zi[a] += vi;
            }
        }
    }
    // split into the spectrum of the real sequence:
    // E[k] = (Z[k] + conj Z[m-k]) / 2,  O[k] = -j (Z[k] - conj Z[m-k]) / 2,  X[k] = E + W^k O
    out.resize(m + 1);
    for (size_t k = 0; k <= m; ++k) {
        const size_t k1 = k % m, k2 = (m - k) % m;
        const double r1 = zr[k1], i1 = zi[k1];
        const double r2 = zr[k2], i2 = -zi[k2];
        const double er = 0.5 * (r1 + r2), ei = 0.5 * (i1 + i2);
        const double orr = 0.5 * (i1 - i2), oi = -0.5 * (r1 - r2);
        const double wr = d.postr[k], wi = d.posti[k];
        out[k] = {static_cast<float>(er + wr * orr - wi * oi), static_cast<float>(ei + wr * oi + wi * orr)};
    }
}

// ================================================================== windows
std::vector<float> makeWindow(WindowType t, size_t n) {
    std::vector<float> w(n);
    for (size_t i = 0; i < n; ++i) {
        const double x = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n); // periodic
        double v = 1.0;
        switch (t) {
        case WindowType::Rectangular: v = 1.0; break;
        case WindowType::Hann: v = 0.5 - 0.5 * std::cos(x); break;
        case WindowType::Hamming: v = 0.54 - 0.46 * std::cos(x); break;
        case WindowType::BlackmanHarris:
            v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
            break;
        case WindowType::FlatTop:
            v = 0.21557895 - 0.41663158 * std::cos(x) + 0.277263158 * std::cos(2 * x) -
                0.083578947 * std::cos(3 * x) + 0.006947368 * std::cos(4 * x);
            break;
        }
        w[i] = static_cast<float>(v);
    }
    return w;
}

const char* windowName(WindowType t) {
    switch (t) {
    case WindowType::Rectangular: return "Rectangular";
    case WindowType::Hann: return "Hann";
    case WindowType::Hamming: return "Hamming";
    case WindowType::BlackmanHarris: return "Blackman-Harris";
    case WindowType::FlatTop: return "Flat Top";
    }
    return "?";
}

int windowMainLobeBins(WindowType t) {
    switch (t) {
    case WindowType::Rectangular: return 1;
    case WindowType::Hann:
    case WindowType::Hamming: return 3;
    case WindowType::BlackmanHarris: return 5;
    case WindowType::FlatTop: return 6;
    }
    return 3;
}

// ======================================================== SpectrumAnalyzer
void SpectrumAnalyzer::configure(size_t n, WindowType w) {
    if (fft_ && n == n_ && w == type_) return;
    n_ = n;
    type_ = w;
    fft_ = std::make_unique<RealFFT>(n);
    win_ = makeWindow(w, n);
    double s = 0, s2 = 0;
    for (float v : win_) { s += v; s2 += static_cast<double>(v) * v; }
    sumW_ = s;
    cg_ = s / static_cast<double>(n);
    enbw_ = static_cast<double>(n) * s2 / (s * s);
    buf_.resize(n);
}

void SpectrumAnalyzer::compute(const float* x, std::vector<float>& power) {
#if defined(SLP_HAVE_ACCELERATE)
    vDSP_vmul(x, 1, win_.data(), 1, buf_.data(), 1, static_cast<vDSP_Length>(n_));
#else
    for (size_t i = 0; i < n_; ++i) buf_[i] = x[i] * win_[i];
#endif
    fft_->forward(buf_.data(), spec_);
    const size_t m = n_ / 2;
    power.resize(m + 1);
    const double inv = 1.0 / sumW_;
    for (size_t k = 0; k <= m; ++k) {
        const double re = spec_[k].real() * inv, im = spec_[k].imag() * inv;
        const double a2 = re * re + im * im; // (|X|/sum w)^2
        power[k] = static_cast<float>((k == 0 || k == m) ? a2 : 2.0 * a2);
    }
}

// ======================================================== peaks / harmonics
double tonePower(const std::vector<float>& p, int bin, int lobe, double enbw) {
    const int m = static_cast<int>(p.size());
    double s = 0;
    for (int k = std::max(0, bin - lobe); k <= std::min(m - 1, bin + lobe); ++k) s += p[k];
    return s / enbw;
}

std::vector<SpectralPeak> findPeaks(const std::vector<float>& power, double binHz, WindowType w, double enbw,
                                    int maxPeaks, double minRelDb) {
    std::vector<SpectralPeak> out;
    const int m = static_cast<int>(power.size());
    if (m < 8 || maxPeaks <= 0) return out;
    std::vector<float> db(m);
    for (int k = 0; k < m; ++k) db[k] = static_cast<float>(10.0 * std::log10(power[k] + 1e-30));
    std::vector<float> tmp(db);
    std::nth_element(tmp.begin(), tmp.begin() + m / 2, tmp.end());
    const double floorDb = tmp[m / 2];
    const int lobe = windowMainLobeBins(w);
    std::vector<int> cand;
    for (int k = std::max(1, lobe); k < m - 1; ++k)
        if (db[k] > db[k - 1] && db[k] >= db[k + 1] && db[k] > floorDb + minRelDb) cand.push_back(k);
    std::sort(cand.begin(), cand.end(), [&](int a, int b) { return db[a] > db[b]; });
    std::vector<int> chosen;
    for (int k : cand) {
        bool nearStronger = false;
        for (int c : chosen)
            if (std::abs(c - k) <= 2 * lobe) { nearStronger = true; break; }
        if (nearStronger) continue;
        chosen.push_back(k);
        if (static_cast<int>(chosen.size()) >= maxPeaks) break;
    }
    for (int k : chosen) {
        double delta;
        if (w == WindowType::Rectangular) {
            // parabolic interpolation of the dB values around the maximum
            const double a = db[k - 1], b = db[k], c = db[k + 1];
            const double den = a - 2.0 * b + c;
            delta = den != 0.0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
        } else {
            // power centroid over the main lobe: |W(f)|^2 is symmetric about the tone
            // frequency, so the centroid is unbiased (< 0.003 bin for Hann..Flat Top)
            double s = 0, sk = 0;
            for (int j = std::max(0, k - lobe); j <= std::min(m - 1, k + lobe); ++j) {
                s += power[j];
                sk += power[j] * (j - k);
            }
            delta = s > 0 ? std::clamp(sk / s, -1.0, 1.0) : 0.0;
        }
        SpectralPeak pk;
        pk.bin = k;
        pk.freq = (k + delta) * binHz;
        pk.power = tonePower(power, k, lobe, enbw);
        out.push_back(pk);
    }
    return out;
}

HarmonicAnalysis analyzeHarmonics(const std::vector<float>& power, double binHz, WindowType w, double enbw,
                                  int maxOrder) {
    HarmonicAnalysis h;
    auto peaks = findPeaks(power, binHz, w, enbw, 1, 20.0);
    if (peaks.empty() || !(binHz > 0)) return h;
    const auto& f = peaks.front();
    const int m = static_cast<int>(power.size());
    const int lobe = windowMainLobeBins(w);
    h.valid = true;
    h.fundamental = f.freq;
    h.harmonics.push_back({1, f.freq, f.power});
    double sumH = 0.0;
    for (int order = 2; order <= maxOrder; ++order) {
        const double fh = order * f.freq;
        const int bin = static_cast<int>(std::lround(fh / binHz));
        if (bin + lobe >= m - 1) break;
        int best = bin;
        for (int k = std::max(1, bin - 2); k <= std::min(m - 1, bin + 2); ++k)
            if (power[k] > power[best]) best = k;
        const double p = tonePower(power, best, lobe, enbw);
        h.harmonics.push_back({order, fh, p});
        sumH += p;
    }
    h.thdRatio = f.power > 0 ? std::sqrt(sumH / f.power) : 0.0;
    return h;
}

// ================================================================== filters
std::complex<double> Biquad::response(double w) const {
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

namespace {
Biquad lowpass2(double w0, double q) {
    const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * q), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 - cw) * 0.5 / a0;
    b.b1 = (1.0 - cw) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * cw / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}
Biquad highpass2(double w0, double q) {
    const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * q), a0 = 1.0 + alpha;
    Biquad b;
    b.b0 = (1.0 + cw) * 0.5 / a0;
    b.b1 = -(1.0 + cw) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * cw / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}
Biquad lowpass1(double w0) {
    const double k = std::tan(0.5 * w0);
    Biquad b;
    b.b0 = k / (1.0 + k);
    b.b1 = b.b0;
    b.a1 = (k - 1.0) / (k + 1.0);
    return b;
}
Biquad highpass1(double w0) {
    const double k = std::tan(0.5 * w0);
    Biquad b;
    b.b0 = 1.0 / (1.0 + k);
    b.b1 = -b.b0;
    b.a1 = (k - 1.0) / (k + 1.0);
    return b;
}
double clampFc(double fc, double fs) { return std::clamp(fc, 1e-7 * fs, 0.499 * fs); }
} // namespace

IirFilter IirFilter::butterworthLowpass(int order, double fc, double fs) {
    order = std::clamp(order, 1, 8);
    const double w0 = 2.0 * kPi * clampFc(fc, fs) / fs;
    IirFilter f;
    for (int k = 0; k < order / 2; ++k) {
        const double zeta = std::sin(kPi * (2 * k + 1) / (2.0 * order));
        f.s_.push_back(lowpass2(w0, 1.0 / (2.0 * zeta)));
    }
    if (order % 2) f.s_.push_back(lowpass1(w0));
    return f;
}

IirFilter IirFilter::butterworthHighpass(int order, double fc, double fs) {
    order = std::clamp(order, 1, 8);
    const double w0 = 2.0 * kPi * clampFc(fc, fs) / fs;
    IirFilter f;
    for (int k = 0; k < order / 2; ++k) {
        const double zeta = std::sin(kPi * (2 * k + 1) / (2.0 * order));
        f.s_.push_back(highpass2(w0, 1.0 / (2.0 * zeta)));
    }
    if (order % 2) f.s_.push_back(highpass1(w0));
    return f;
}

IirFilter IirFilter::butterworthBandpass(int order, double f1, double f2, double fs) {
    if (f2 < f1) std::swap(f1, f2);
    IirFilter hp = butterworthHighpass(order, f1, fs);
    IirFilter lp = butterworthLowpass(order, f2, fs);
    hp.s_.insert(hp.s_.end(), lp.s_.begin(), lp.s_.end());
    return hp;
}

void IirFilter::reset() {
    for (auto& s : s_) s.z1 = s.z2 = 0.0;
}

void IirFilter::prime(double x0) {
    for (auto& s : s_) {
        const double den = 1.0 + s.a1 + s.a2;
        const double y = den != 0.0 ? x0 * (s.b0 + s.b1 + s.b2) / den : 0.0;
        s.z2 = s.b2 * x0 - s.a2 * y;
        s.z1 = s.b1 * x0 - s.a1 * y + s.z2;
        x0 = y;
    }
}

void IirFilter::process(float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        double v = x[i];
        for (auto& s : s_) v = s.process(v);
        x[i] = static_cast<float>(v);
    }
    for (auto& s : s_) { // flush denormals
        if (std::fabs(s.z1) < 1e-200) s.z1 = 0.0;
        if (std::fabs(s.z2) < 1e-200) s.z2 = 0.0;
    }
}

std::complex<double> IirFilter::response(double f, double fs) const {
    const double w = 2.0 * kPi * f / fs;
    std::complex<double> h = 1.0;
    for (const auto& s : s_) h *= s.response(w);
    return h;
}

double IirFilter::magnitudeDb(double f, double fs) const {
    return 20.0 * std::log10(std::max(1e-300, std::abs(response(f, fs))));
}

void AcCoupler::configure(double fc, double fs) {
    fc = std::min(fc, 0.45 * fs);
    const double k = std::tan(kPi * fc / fs);
    b0_ = 1.0 / (1.0 + k);
    b1_ = -b0_;
    a1_ = (k - 1.0) / (k + 1.0);
}

void AcCoupler::process(float* x, size_t n) {
    if (n == 0) return;
    if (!primed_) {
        double mean = 0;
        size_t cnt = 0;
        for (size_t i = 0; i < n; ++i)
            if (std::isfinite(x[i])) { mean += x[i]; ++cnt; }
        mean = cnt ? mean / static_cast<double>(cnt) : 0.0;
        z_ = -b0_ * mean; // steady state of the high-pass for a DC input = mean
        primed_ = true;
    }
    for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        const double y = b0_ * v + z_;
        z_ = b1_ * v - a1_ * y;
        x[i] = static_cast<float>(y);
    }
}

// ================================================================== math
std::string mathOpLabel(const MathSettings& m) {
    const char* a[] = {"CH1", "CH2", "CH3", "CH4"};
    const std::string A = a[std::clamp(m.srcA, 0, 3)], B = a[std::clamp(m.srcB, 0, 3)];
    switch (m.op) {
    case MathOp::Add: return A + " + " + B;
    case MathOp::Subtract: return A + " - " + B;
    case MathOp::Multiply: return A + " x " + B;
    case MathOp::Derivative: return "d/dt " + A;
    case MathOp::Integral: return "\xE2\x88\xAB" + A + " dt";
    case MathOp::LowPass: return "LPF(" + A + ")";
    case MathOp::HighPass: return "HPF(" + A + ")";
    case MathOp::BandPass: return "BPF(" + A + ")";
    case MathOp::MovingAverage: return "AVG(" + A + ")";
    }
    return "?";
}

size_t mathLeadIn(const MathSettings& m, double fs) {
    switch (m.op) {
    case MathOp::LowPass:
    case MathOp::HighPass:
    case MathOp::BandPass: {
        double fcMin = m.cutoff;
        if (m.op == MathOp::BandPass) fcMin = std::min(m.cutoff, m.cutoffHigh);
        fcMin = std::max(fcMin, 1e-3);
        const double lead = fs * 1.5 * std::clamp(m.order, 1, 8) / fcMin;
        return static_cast<size_t>(std::min(lead, static_cast<double>(1 << 20)));
    }
    case MathOp::MovingAverage: return static_cast<size_t>(std::max(1, m.maLength));
    case MathOp::Derivative: return 1;
    default: return 0;
    }
}

MathResult computeMath(const MathSettings& m, const float* a, const float* b, size_t n, size_t lead, double fs,
                       const std::string& unitA, const std::string& unitB) {
    MathResult r;
    lead = std::min(lead, n);
    const size_t outN = n - lead;
    const bool binary = m.op == MathOp::Add || m.op == MathOp::Subtract || m.op == MathOp::Multiply;
    // Working copies with NaN gaps bridged by holding the last valid value.
    std::vector<float> wa(a, a + n), wb;
    std::vector<uint8_t> bad(n, 0);
    auto bridge = [&](std::vector<float>& v) {
        size_t first = n;
        for (size_t i = 0; i < n; ++i)
            if (std::isfinite(v[i])) { first = i; break; }
        float last = first < n ? v[first] : 0.0f; // leading gap: back-fill with first valid value
        for (size_t i = 0; i < n; ++i) {
            if (std::isfinite(v[i])) last = v[i];
            else { bad[i] = 1; v[i] = last; }
        }
    };
    bridge(wa);
    if (binary) { wb.assign(b, b + n); bridge(wb); }

    r.y.resize(outN);
    char buf[256];
    const double nyq = 0.5 * fs;
    switch (m.op) {
    case MathOp::Add:
        for (size_t i = 0; i < outN; ++i) r.y[i] = wa[lead + i] + wb[lead + i];
        r.unit = unitA;
        r.info = "Sum: " + mathOpLabel(m);
        break;
    case MathOp::Subtract:
        for (size_t i = 0; i < outN; ++i) r.y[i] = wa[lead + i] - wb[lead + i];
        r.unit = unitA;
        r.info = "Difference: " + mathOpLabel(m);
        break;
    case MathOp::Multiply:
        for (size_t i = 0; i < outN; ++i) r.y[i] = wa[lead + i] * wb[lead + i];
        r.unit = unitA == unitB ? unitA + "\xC2\xB2" : unitA + "\xC2\xB7" + unitB;
        r.info = "Product: " + mathOpLabel(m);
        break;
    case MathOp::Derivative: {
        // central difference (no half-sample delay); one-sided at the ends
        for (size_t i = 0; i < outN; ++i) {
            const size_t k = lead + i;
            double d;
            if (k > 0 && k + 1 < n) d = (static_cast<double>(wa[k + 1]) - wa[k - 1]) * 0.5 * fs;
            else if (k + 1 < n) d = (static_cast<double>(wa[k + 1]) - wa[k]) * fs;
            else if (k > 0) d = (static_cast<double>(wa[k]) - wa[k - 1]) * fs;
            else d = 0.0;
            r.y[i] = static_cast<float>(d);
        }
        r.unit = unitA + "/s";
        r.info = "Derivative (central difference)";
        break;
    }
    case MathOp::Integral: {
        double acc = 0.0;
        const double dt = 1.0 / fs;
        for (size_t i = 0; i < outN; ++i) {
            const size_t k = lead + i;
            if (i > 0) acc += 0.5 * (static_cast<double>(wa[k]) + wa[k - 1]) * dt;
            r.y[i] = static_cast<float>(acc);
        }
        r.unit = unitA + "\xC2\xB7s";
        r.info = "Integral (trapezoidal, 0 at screen start)";
        break;
    }
    case MathOp::LowPass:
    case MathOp::HighPass:
    case MathOp::BandPass: {
        double f1 = m.cutoff, f2 = m.cutoffHigh;
        if (m.op == MathOp::BandPass && f2 < f1) std::swap(f1, f2);
        if (f1 >= 0.499 * fs || (m.op == MathOp::BandPass && f2 >= 0.499 * fs)) r.warning = true;
        IirFilter flt;
        const int ord = std::clamp(m.order, 1, 8);
        if (m.op == MathOp::LowPass) flt = IirFilter::butterworthLowpass(ord, f1, fs);
        else if (m.op == MathOp::HighPass) flt = IirFilter::butterworthHighpass(ord, f1, fs);
        else flt = IirFilter::butterworthBandpass(ord, f1, f2, fs);
        flt.prime(n ? wa[0] : 0.0);
        flt.process(wa.data(), n);
        std::copy(wa.begin() + static_cast<long>(lead), wa.end(), r.y.begin());
        r.unit = unitA;
        const char* kind = m.op == MathOp::LowPass ? "LPF" : m.op == MathOp::HighPass ? "HPF" : "BPF";
        if (m.op == MathOp::BandPass)
            std::snprintf(buf, sizeof buf, "Butterworth %s, order %d+%d, %s - %s (fs %s, Nyquist %s)%s", kind, ord, ord,
                          formatSI(std::min(f1, 0.499 * fs), "Hz").c_str(), formatSI(std::min(f2, 0.499 * fs), "Hz").c_str(),
                          formatSI(fs, "Sa/s").c_str(), formatSI(nyq, "Hz").c_str(),
                          r.warning ? "  [cut-off clamped below Nyquist]" : "");
        else
            std::snprintf(buf, sizeof buf, "Butterworth %s, order %d, fc %s (fc/fs %.3g, Nyquist %s)%s", kind, ord,
                          formatSI(std::min(f1, 0.499 * fs), "Hz").c_str(), std::min(f1, 0.499 * fs) / fs,
                          formatSI(nyq, "Hz").c_str(), r.warning ? "  [cut-off clamped below Nyquist]" : "");
        r.info = buf;
        break;
    }
    case MathOp::MovingAverage: {
        const size_t len = static_cast<size_t>(std::clamp(m.maLength, 1, 1 << 20));
        double sum = 0.0;
        std::vector<float> y(n);
        for (size_t i = 0; i < n; ++i) {
            sum += wa[i];
            if (i >= len) sum -= wa[i - len];
            y[i] = static_cast<float>(sum / static_cast<double>(std::min(i + 1, len)));
        }
        std::copy(y.begin() + static_cast<long>(lead), y.end(), r.y.begin());
        r.unit = unitA;
        std::snprintf(buf, sizeof buf, "Moving average, %zu samples (%s window, first null at %s)", len,
                      formatSI(static_cast<double>(len) / fs, "s").c_str(),
                      formatSI(fs / static_cast<double>(len), "Hz").c_str());
        r.info = buf;
        break;
    }
    }
    // Propagate invalid input samples to the output
    for (size_t i = 0; i < outN; ++i)
        if (bad[lead + i]) r.y[i] = std::numeric_limits<float>::quiet_NaN();
    return r;
}

} // namespace slp
