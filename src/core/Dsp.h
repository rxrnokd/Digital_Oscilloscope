// SIGNAL LAB PRO - DSP engine: FFT, windows, spectrum analysis, digital filters, math.
//
// FFT backend: Apple Accelerate vDSP (vDSP_fft_zrip) when SLP_HAVE_ACCELERATE is defined
// (macOS), otherwise a portable radix-2 real FFT. Both return the plain, unnormalised DFT
// X[k] = sum_n x[n] exp(-j 2 pi k n / N), k = 0..N/2.
//
// Spectrum normalisation (SpectrumAnalyzer::compute):
//   * window w[n] (periodic form), coherent gain CG = sum(w) / N
//   * single-sided amplitude  A[k] = 2 |X[k]| / sum(w)  (0 < k < N/2), |X[k]| / sum(w) at DC/Nyquist
//     -> a sinusoid of peak amplitude Ap centred on a bin reads exactly Ap.
//   * the analyzer outputs POWER in (unit rms)^2:  P[k] = A[k]^2 / 2 (DC/Nyquist: A^2)
//   * frequency axis f[k] = k * fs / N, k = 0..N/2 (N/2 = Nyquist = fs/2)
//   * dB reference: 0 dBV = 1 Vrms (P = 1 V^2), 0 dBFS = full-scale sine (Ap = 1 FS)
//   * tone power from the power sum over the main lobe divided by the ENBW (in bins),
//     which removes the scalloping loss for all windows except Rectangular.
#pragma once

#include "ScopeSettings.h"

#include <complex>
#include <memory>
#include <string>
#include <vector>

namespace slp {

// ------------------------------------------------------------------------- FFT
class RealFFT {
public:
    explicit RealFFT(size_t n = 1024);
    ~RealFFT();
    RealFFT(const RealFFT&) = delete;
    RealFFT& operator=(const RealFFT&) = delete;

    size_t size() const { return n_; }
    // out receives n/2+1 bins
    void forward(const float* in, std::vector<std::complex<float>>& out);
    // Always available reference implementation (used by tests to cross-check vDSP)
    void forwardPortable(const float* in, std::vector<std::complex<float>>& out);
    static const char* backendName();

private:
    struct Impl;
    size_t n_;
    std::unique_ptr<Impl> d_;
};

// ----------------------------------------------------------------------- windows
std::vector<float> makeWindow(WindowType type, size_t n);
const char* windowName(WindowType type);
int windowMainLobeBins(WindowType type); // half width used for tone power sums

// ------------------------------------------------------------- spectrum analyzer
class SpectrumAnalyzer {
public:
    void configure(size_t n, WindowType w);
    size_t size() const { return n_; }
    WindowType window() const { return type_; }
    double enbwBins() const { return enbw_; }
    double coherentGain() const { return cg_; }
    // x: n samples, power: n/2+1 bins of (unit rms)^2
    void compute(const float* x, std::vector<float>& power);

private:
    size_t n_ = 0;
    WindowType type_ = WindowType::Hann;
    std::unique_ptr<RealFFT> fft_;
    std::vector<float> win_, buf_;
    std::vector<std::complex<float>> spec_;
    double sumW_ = 1.0, enbw_ = 1.0, cg_ = 1.0;
};

struct SpectralPeak {
    double freq = 0.0;  // Hz (interpolated)
    double power = 0.0; // (unit rms)^2 of the tone
    int bin = 0;
};

struct HarmonicInfo {
    int order = 0;     // 1 = fundamental
    double freq = 0.0;
    double power = 0.0;
};

struct HarmonicAnalysis {
    bool valid = false;
    double fundamental = 0.0;   // Hz
    double thdRatio = 0.0;      // sqrt(sum P_h, h>=2) / sqrt(P_1)
    std::vector<HarmonicInfo> harmonics; // includes the fundamental at index 0
};

// Tone power: sum of bins within +-lobe around `bin`, divided by ENBW.
double tonePower(const std::vector<float>& power, int bin, int lobe, double enbwBins);
// Local maxima at least `minRelDb` above the median floor, strongest first.
std::vector<SpectralPeak> findPeaks(const std::vector<float>& power, double binHz, WindowType w,
                                    double enbwBins, int maxPeaks, double minRelDb = 20.0);
HarmonicAnalysis analyzeHarmonics(const std::vector<float>& power, double binHz, WindowType w,
                                  double enbwBins, int maxOrder = 10);

// ------------------------------------------------------------------- filters
// Biquad in transposed direct form II, double precision state (needed for low fc/fs).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    std::complex<double> response(double w) const; // w = 2 pi f / fs
};

class IirFilter {
public:
    // Butterworth designs via the bilinear transform with frequency pre-warping, so the
    // -3.01 dB point is exactly at the requested cut-off.
    static IirFilter butterworthLowpass(int order, double fc, double fs);
    static IirFilter butterworthHighpass(int order, double fc, double fs);
    // Band-pass = HPF(f1) cascaded with LPF(f2), each of the given order.
    static IirFilter butterworthBandpass(int order, double f1, double f2, double fs);

    void reset();
    void prime(double x0); // steady state for a constant input x0 (no start-up step)
    void process(float* x, size_t n);
    std::complex<double> response(double f, double fs) const;
    double magnitudeDb(double f, double fs) const;
    size_t sections() const { return s_.size(); }

private:
    std::vector<Biquad> s_;
};

// One-pole/one-zero AC-coupling high-pass (bilinear), with mean priming.
class AcCoupler {
public:
    void configure(double fc, double fs);
    void reset() { primed_ = false; }
    void process(float* x, size_t n);

private:
    double b0_ = 1, b1_ = -1, a1_ = 0, z_ = 0;
    bool primed_ = false;
};

// --------------------------------------------------------------------- math
struct MathResult {
    std::vector<float> y;  // output samples (lead-in removed)
    std::string unit;
    std::string info;      // human readable description of the operation / filter
    bool warning = false;  // e.g. cut-off clamped below Nyquist
};

// Recommended number of lead-in samples (filter settling) before the region of interest.
size_t mathLeadIn(const MathSettings& m, double fs);
// a and b hold `n` samples, of which the first `lead` are history used only for settling.
MathResult computeMath(const MathSettings& m, const float* a, const float* b, size_t n, size_t lead,
                       double fs, const std::string& unitA, const std::string& unitB);
std::string mathOpLabel(const MathSettings& m);

} // namespace slp
