// SIGNAL LAB PRO - acquisition engine (runs in its own thread, independent of the GUI).
//
// Data flow:
//   SampleSource (generator / file / audio push)  --->  per-channel processing
//   (AC coupling, GND, invert)  --->  SampleRing acquisition memory (4 Mpts / channel)
//   ---> EdgeTrigger (on real samples) ---> Frame extraction (pre-trigger history comes
//   from the ring) ---> Math / Measurements / Spectrum ---> published to the GUI.
//
// Three rates are managed independently:
//   * sample rate        : virtual ADC rate (generator) or file/device rate, decimated
//                          automatically when the screen would need > 1 Mpts.
//   * generation rate    : virtual sources stream in real time; when that would exceed
//                          the CPU budget (4 MSa/s/channel) the engine acquires
//                          contiguous segments with dead time between them, exactly like
//                          a real DSO. The signal stays phase coherent across gaps.
//   * display frame rate : frames are produced at most `refreshRate` times per second.
#pragma once

#include "Dsp.h"
#include "FileIO.h"
#include "Measurements.h"
#include "RingBuffer.h"
#include "ScopeSettings.h"
#include "SignalGenerator.h"
#include "Trigger.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace slp {

constexpr size_t kRingCapacity = size_t{1} << 22; // samples per channel
constexpr size_t kMaxRecord = size_t{1} << 20;    // max samples per trace on screen
constexpr double kGenBudget = 4.0e6;              // virtual-source samples / s / channel
constexpr double kAcCouplingHz = 10.0;            // AC coupling corner (1st-order HPF)
constexpr int kMaxFft = 1 << 18;
constexpr double kRollMinSecPerDiv = 0.05;        // auto roll threshold

enum class TrigState { Stopped, Armed, Waiting, Triggered, Auto, Roll };
const char* trigStateLabel(TrigState s);

struct TraceData {
    bool present = false;
    std::vector<float> y;
    double t0 = 0.0;  // time of y[0] relative to the trigger point (s)
    double dt = 1.0;  // sample interval (s)
    std::string unit = "V";
    std::string label;
    std::string info; // math description
    bool warning = false;
    MeasurementSet meas;
};

struct Frame {
    uint64_t seq = 0;
    std::array<TraceData, kNumTraces> tr;
    double fs = 0.0;
    double nativeRate = 0.0;
    int decim = 1;
    double tStart = 0.0, tEnd = 0.0; // screen window relative to the trigger (s)
    TrigState state = TrigState::Waiting;
    bool forced = false;
    ScopeSettings settings;          // settings in effect for this frame
};

struct SpectrumData {
    uint64_t seq = 0;
    bool valid = false;
    int source = 0;
    std::string unit = "V";
    double fs = 0.0, binHz = 0.0;
    int fftSize = 0;
    WindowType window = WindowType::Hann;
    double enbwBins = 1.0;
    int averaged = 0;
    std::vector<float> power; // (unit rms)^2 per bin, N/2+1 bins
    std::vector<SpectralPeak> peaks;
    HarmonicAnalysis harmonics;
    std::string message;
};

struct EngineStatus {
    bool running = false;
    TrigState state = TrigState::Stopped;
    double fs = 0.0, nativeRate = 0.0;
    int decim = 1;
    double frameRate = 0.0; // displayed waveforms per second
    bool segmented = false; // acquisition with dead time (virtual source above budget)
    uint64_t acquired = 0;  // samples acquired since start
    uint64_t audioDropped = 0;
    std::string message;
};

// Real-time push source (microphone / future serial ADC). Producer thread pushes
// interleaved float frames; the acquisition thread pulls them.
class PushSource {
public:
    explicit PushSource(size_t capacityFloats = size_t{1} << 21) : ring_(capacityFloats) {}
    void configure(double rate, int channels) {
        rate_.store(rate);
        channels_.store(channels);
        generation_.fetch_add(1);
    }
    void push(const float* interleaved, size_t frames) {
        const size_t want = frames * static_cast<size_t>(std::max(1, channels_.load()));
        const size_t got = ring_.push(interleaved, want);
        if (got < want) dropped_.fetch_add(want - got);
    }
    size_t pull(float* interleaved, size_t maxFrames) {
        const int ch = std::max(1, channels_.load());
        const size_t frames = std::min(maxFrames, ring_.available() / static_cast<size_t>(ch));
        return ring_.pop(interleaved, frames * static_cast<size_t>(ch)) / static_cast<size_t>(ch);
    }
    void discard() { ring_.discard(); }
    double rate() const { return rate_.load(); }
    int channels() const { return channels_.load(); }
    uint64_t generation() const { return generation_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

private:
    SpscRing<float> ring_;
    std::atomic<double> rate_{0.0};
    std::atomic<int> channels_{0};
    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> dropped_{0};
};

class AcquisitionEngine {
public:
    explicit AcquisitionEngine(bool startThread = true);
    ~AcquisitionEngine();
    AcquisitionEngine(const AcquisitionEngine&) = delete;
    AcquisitionEngine& operator=(const AcquisitionEngine&) = delete;

    // ---- thread-safe control API (GUI thread) --------------------------------------
    void setSettings(const ScopeSettings& s);
    void setFileData(std::shared_ptr<const SignalData> data);
    void setPushSource(std::shared_ptr<PushSource> src);
    void run();          // continuous acquisition (in SINGLE mode: arm once)
    void stop();
    void single();       // arm a single acquisition, stop after the first trigger
    void forceTrigger();
    void requestAutoscale();
    void syncGeneratorPhases();
    void resetSpectrum();

    std::shared_ptr<const Frame> latestFrame() const;
    std::shared_ptr<const SpectrumData> latestSpectrum() const;
    EngineStatus status() const;
    std::optional<ScopeSettings> takeAutoscaleResult();

    // Advances the engine to time `now` (seconds, monotonic). Called by the internal
    // thread; tests call it directly with a simulated clock (startThread = false).
    void tick(double now);

private:
    enum class Run { Stopped, Running, Single };
    enum Cmd : uint32_t { CmdRun = 1, CmdStop = 2, CmdSingle = 4, CmdForce = 8, CmdAutoscale = 16, CmdSync = 32, CmdResetSpec = 64 };

    void post(uint32_t cmd);
    void threadMain();
    void pullInputs();
    void applySettings(const ScopeSettings& ns);
    double nativeRate() const;
    int computeDecim(double native) const;
    bool randomAccessSource() const;
    bool needed(int ch) const;
    std::string channelUnit(int ch) const;
    void configureTrigger();
    void restartAcquisition(double now);
    double segmentSize() const;
    void acquire(double now);
    void acquirePush();
    void jumpTo(int64_t index);
    void generateBlock(int64_t start, int64_t count);
    void produceRaw(int64_t index, size_t m);
    void commitBlock(int64_t index, size_t m);
    void produce(double now);
    bool isRoll() const;
    void window(double& tStart, double& tEnd) const;
    std::shared_ptr<Frame> buildFrame(double trigPos, TrigState state, bool forced);
    void measureTrace(TraceData& tr, double tStart, double tEnd) const;
    void publishFrame(std::shared_ptr<Frame> f);
    void computeSpectrum(int64_t endIndex, bool accumulate);
    void doAutoscale();
    void publishStatus(double now);

    // ---- inputs (inMutex_) -----------------------------------------------------------
    mutable std::mutex inMutex_;
    ScopeSettings inSettings_;
    bool inSettingsDirty_ = false;
    std::shared_ptr<const SignalData> inFile_;
    bool inFileDirty_ = false;
    std::shared_ptr<PushSource> inPush_;
    bool inPushDirty_ = false;
    uint32_t commands_ = 0;

    // ---- outputs (outMutex_) ---------------------------------------------------------
    mutable std::mutex outMutex_;
    std::shared_ptr<const Frame> frame_;
    std::shared_ptr<const SpectrumData> spectrum_;
    EngineStatus status_;
    std::optional<ScopeSettings> autoscale_;

    // ---- thread ------------------------------------------------------------------------
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex cvMutex_;
    std::condition_variable cv_;
    bool wake_ = false;

    // ---- engine state (engine thread only) ------------------------------------------
    ScopeSettings s_;
    bool haveSettings_ = false;
    std::shared_ptr<const SignalData> file_;
    std::shared_ptr<PushSource> push_;
    uint64_t pushGeneration_ = 0;
    std::array<SignalGenerator, kNumGenerators> gens_{SignalGenerator(1), SignalGenerator(2), SignalGenerator(3), SignalGenerator(4)};
    std::array<SampleRing, kNumChannels> rings_;
    std::array<AcCoupler, kNumChannels> ac_;
    EdgeTrigger trig_;
    std::deque<double> events_;
    std::vector<double> newEvents_;
    double fs_ = 1e6, native_ = 1e6;
    int decim_ = 1;
    int64_t writeIndex_ = 0;
    double clockOrigin_ = 0, lastAcqTime_ = 0, credit_ = 0;
    Run run_ = Run::Running;
    TrigState state_ = TrigState::Waiting;
    bool needRestart_ = true;
    bool needRedraw_ = false;
    bool forcePending_ = false;
    bool autoscalePending_ = false;
    bool segmented_ = false;
    double lastFrameTime_ = -1e9, lastTrigWall_ = 0, lastSpecTime_ = -1e9;
    double retainedTrig_ = 0;
    TrigState retainedState_ = TrigState::Stopped;
    bool haveRetained_ = false;
    uint64_t frameSeq_ = 0, specSeq_ = 0, acquired_ = 0;
    // spectrum
    SpectrumAnalyzer analyzer_;
    std::vector<float> specAvg_, specTmp_;
    int specCount_ = 0;
    std::string specKey_;
    // statistics
    double statT0_ = 0;
    int statFrames_ = 0;
    double frameRate_ = 0;
    // scratch
    std::array<std::vector<float>, kNumGenerators> genBuf_;
    std::array<std::vector<float>, kNumChannels> raw_;
    std::vector<float> proc_, pushBuf_;
    std::vector<double> pushAccum_;
    int pushAccumCount_ = 0;
};

} // namespace slp
