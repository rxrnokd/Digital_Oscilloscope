#include "Acquisition.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace slp {

const char* trigStateLabel(TrigState s) {
    switch (s) {
    case TrigState::Stopped: return "STOP";
    case TrigState::Armed: return "ARMED";
    case TrigState::Waiting: return "WAIT";
    case TrigState::Triggered: return "TRIG'D";
    case TrigState::Auto: return "AUTO";
    case TrigState::Roll: return "ROLL";
    }
    return "?";
}

namespace {
int pow2Clamp(int n, int lo, int hi) {
    int p = lo;
    while (p < n && p < hi) p <<= 1;
    return p;
}
bool isBinary(MathOp op) { return op == MathOp::Add || op == MathOp::Subtract || op == MathOp::Multiply; }
} // namespace

// =================================================================== lifecycle
AcquisitionEngine::AcquisitionEngine(bool startThread) {
    for (auto& r : rings_) r.reset(kRingCapacity);
    if (startThread) thread_ = std::thread([this] { threadMain(); });
}

AcquisitionEngine::~AcquisitionEngine() {
    quit_ = true;
    {
        std::lock_guard lk(cvMutex_);
        wake_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void AcquisitionEngine::threadMain() {
    const auto t0 = std::chrono::steady_clock::now();
    while (!quit_) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        tick(now);
        std::unique_lock lk(cvMutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(2), [this] { return wake_ || quit_.load(); });
        wake_ = false;
    }
}

void AcquisitionEngine::post(uint32_t cmd) {
    {
        std::lock_guard lk(inMutex_);
        commands_ |= cmd;
    }
    {
        std::lock_guard lk(cvMutex_);
        wake_ = true;
    }
    cv_.notify_all();
}

void AcquisitionEngine::setSettings(const ScopeSettings& s) {
    {
        std::lock_guard lk(inMutex_);
        inSettings_ = s;
        inSettingsDirty_ = true;
    }
    post(0);
}

void AcquisitionEngine::setFileData(std::shared_ptr<const SignalData> data) {
    {
        std::lock_guard lk(inMutex_);
        inFile_ = std::move(data);
        inFileDirty_ = true;
    }
    post(0);
}

void AcquisitionEngine::setPushSource(std::shared_ptr<PushSource> src) {
    {
        std::lock_guard lk(inMutex_);
        inPush_ = std::move(src);
        inPushDirty_ = true;
    }
    post(0);
}

void AcquisitionEngine::run() { post(CmdRun); }
void AcquisitionEngine::stop() { post(CmdStop); }
void AcquisitionEngine::single() { post(CmdSingle); }
void AcquisitionEngine::forceTrigger() { post(CmdForce); }
void AcquisitionEngine::requestAutoscale() { post(CmdAutoscale); }
void AcquisitionEngine::syncGeneratorPhases() { post(CmdSync); }
void AcquisitionEngine::resetSpectrum() { post(CmdResetSpec); }

std::shared_ptr<const Frame> AcquisitionEngine::latestFrame() const {
    std::lock_guard lk(outMutex_);
    return frame_;
}
std::shared_ptr<const SpectrumData> AcquisitionEngine::latestSpectrum() const {
    std::lock_guard lk(outMutex_);
    return spectrum_;
}
EngineStatus AcquisitionEngine::status() const {
    std::lock_guard lk(outMutex_);
    return status_;
}
std::optional<ScopeSettings> AcquisitionEngine::takeAutoscaleResult() {
    std::lock_guard lk(outMutex_);
    auto r = std::move(autoscale_);
    autoscale_.reset();
    return r;
}

// =================================================================== inputs
void AcquisitionEngine::pullInputs() {
    std::unique_lock lk(inMutex_);
    std::optional<ScopeSettings> ns;
    if (inSettingsDirty_) { ns = inSettings_; inSettingsDirty_ = false; }
    if (inFileDirty_) {
        file_ = inFile_;
        inFileDirty_ = false;
        if (s_.acq.source == SourceKind::File) needRestart_ = true;
        needRedraw_ = true;
    }
    if (inPushDirty_) {
        push_ = inPush_;
        inPushDirty_ = false;
        if (s_.acq.source == SourceKind::Audio) needRestart_ = true;
    }
    const uint32_t cmds = commands_;
    commands_ = 0;
    lk.unlock();

    if (ns) applySettings(*ns);
    if (push_ && push_->generation() != pushGeneration_) {
        pushGeneration_ = push_->generation();
        if (s_.acq.source == SourceKind::Audio) needRestart_ = true;
    }
    if (cmds & CmdStop) { run_ = Run::Stopped; state_ = TrigState::Stopped; }
    if (cmds & CmdRun) {
        run_ = s_.trigger.mode == TriggerMode::Single ? Run::Single : Run::Running;
        needRestart_ = true;
    }
    if (cmds & CmdSingle) { run_ = Run::Single; needRestart_ = true; }
    if (cmds & CmdForce) forcePending_ = true;
    if (cmds & CmdAutoscale) autoscalePending_ = true;
    if (cmds & CmdSync)
        for (auto& g : gens_) g.resetPhase();
    if (cmds & CmdResetSpec) specCount_ = 0;
}

void AcquisitionEngine::applySettings(const ScopeSettings& ns) {
    const bool first = !haveSettings_;
    const ScopeSettings old = s_;
    s_ = ns;
    haveSettings_ = true;
    for (int g = 0; g < kNumGenerators; ++g) gens_[g].setSettings(ns.gen[g]);

    if (first || old.acq.source != ns.acq.source || old.acq.sampleRate != ns.acq.sampleRate) needRestart_ = true;
    if (computeDecim(nativeRate()) != decim_) needRestart_ = true;
    for (int c = 0; c < kNumChannels; ++c)
        if (old.ch[c].coupling != ns.ch[c].coupling || old.ch[c].invert != ns.ch[c].invert) ac_[c].reset();

    const int src = std::clamp(ns.trigger.source, 0, kNumChannels - 1);
    const int osrc = std::clamp(old.trigger.source, 0, kNumChannels - 1);
    if (first || !(old.trigger == ns.trigger) || old.ch[osrc].voltsPerDiv != ns.ch[src].voltsPerDiv ||
        old.ch[src].coupling != ns.ch[src].coupling || old.ch[src].invert != ns.ch[src].invert ||
        old.ch[src].input != ns.ch[src].input) {
        configureTrigger();
        trig_.reset();
        events_.clear();
    }
    if (!(old.spectrum == ns.spectrum) || !(old.math == ns.math)) specCount_ = 0;
    if (first) state_ = TrigState::Waiting;
    needRedraw_ = true;
}

double AcquisitionEngine::nativeRate() const {
    switch (s_.acq.source) {
    case SourceKind::Generator: return std::clamp(s_.acq.sampleRate, 10.0, 1e9);
    case SourceKind::File: return file_ && file_->sampleRate > 0 ? file_->sampleRate : 1e6;
    case SourceKind::Audio: return push_ && push_->rate() > 0 ? push_->rate() : 48000.0;
    case SourceKind::Serial: return 1e3;
    }
    return 1e6;
}

int AcquisitionEngine::computeDecim(double native) const {
    const double win = 10.0 * s_.timebase.secPerDiv * native;
    return std::max(1, static_cast<int>(std::ceil(win / static_cast<double>(kMaxRecord) - 1e-9)));
}

bool AcquisitionEngine::randomAccessSource() const {
    return s_.acq.source == SourceKind::Generator || s_.acq.source == SourceKind::File;
}

bool AcquisitionEngine::needed(int c) const {
    if (s_.ch[c].enabled || c == s_.trigger.source) return true;
    const bool mathUses = s_.math.enabled && (c == s_.math.srcA || (isBinary(s_.math.op) && c == s_.math.srcB));
    if (mathUses) return true;
    if (s_.spectrum.enabled) {
        if (s_.spectrum.source == c) return true;
        if (s_.spectrum.source == kMathTrace && (c == s_.math.srcA || (isBinary(s_.math.op) && c == s_.math.srcB)))
            return true;
    }
    return false;
}

std::string AcquisitionEngine::channelUnit(int c) const {
    switch (s_.acq.source) {
    case SourceKind::Generator: return "V";
    case SourceKind::File: {
        const int t = s_.ch[c].input;
        if (file_ && t >= 0 && t < static_cast<int>(file_->units.size())) return file_->units[t];
        return "V";
    }
    case SourceKind::Audio: return "FS";
    case SourceKind::Serial: return "V";
    }
    return "V";
}

void AcquisitionEngine::configureTrigger() {
    const auto& t = s_.trigger;
    const int src = std::clamp(t.source, 0, kNumChannels - 1);
    const double hyst = std::max(0.0, t.hysteresisDiv) * s_.ch[src].voltsPerDiv;
    trig_.configure(t.slope, t.level, hyst, std::max(0.0, t.holdoff) * fs_);
}

// =================================================================== acquisition
void AcquisitionEngine::restartAcquisition(double now) {
    native_ = nativeRate();
    decim_ = computeDecim(native_);
    fs_ = native_ / decim_;
    writeIndex_ = 0;
    for (int c = 0; c < kNumChannels; ++c) {
        rings_[c].restart(0);
        ac_[c].configure(kAcCouplingHz, fs_);
        ac_[c].reset();
    }
    configureTrigger();
    trig_.reset();
    events_.clear();
    clockOrigin_ = now;
    lastAcqTime_ = now;
    credit_ = std::max(segmentSize(), kGenBudget * 0.1);
    lastTrigWall_ = now;
    haveRetained_ = false;
    specCount_ = 0;
    pushAccumCount_ = 0;
    std::fill(pushAccum_.begin(), pushAccum_.end(), 0.0);
    if (push_ && s_.acq.source == SourceKind::Audio) push_->discard();
    needRestart_ = false;
    forcePending_ = false;
    state_ = run_ == Run::Single ? TrigState::Armed : TrigState::Waiting;
}

double AcquisitionEngine::segmentSize() const {
    const double win = 10.0 * s_.timebase.secPerDiv * fs_;
    double seg = 2.0 * win + std::fabs(s_.timebase.delay) * fs_ + 1024.0;
    if (s_.spectrum.enabled) seg = std::max(seg, static_cast<double>(pow2Clamp(s_.spectrum.fftSize, 1024, kMaxFft)) + 64.0);
    if (s_.math.enabled) seg += std::min<double>(static_cast<double>(mathLeadIn(s_.math, fs_)), win);
    return std::clamp(seg, 4096.0, static_cast<double>(kRingCapacity / 2));
}

void AcquisitionEngine::acquire(double now) {
    if (s_.acq.source == SourceKind::Audio) { acquirePush(); return; }
    if (s_.acq.source == SourceKind::Serial) return; // not implemented (future hardware)
    if (s_.acq.source == SourceKind::File && (!file_ || file_->length() == 0)) return;

    const double dtw = std::max(0.0, now - lastAcqTime_);
    lastAcqTime_ = now;
    const double seg = segmentSize();
    credit_ = std::min(credit_ + dtw * kGenBudget, std::max(seg, kGenBudget * 0.1));
    const auto target = static_cast<int64_t>(std::floor((now - clockOrigin_) * fs_));
    const int64_t due = target - writeIndex_;
    if (due <= 0) return;
    if (static_cast<double>(due) <= credit_) {
        generateBlock(writeIndex_, due);
        credit_ -= static_cast<double>(due);
        segmented_ = false;
    } else if (credit_ >= seg) {
        const int64_t start = target - static_cast<int64_t>(seg);
        jumpTo(start);
        generateBlock(start, static_cast<int64_t>(seg));
        credit_ -= seg;
        segmented_ = true;
    }
}

void AcquisitionEngine::jumpTo(int64_t index) {
    if (index == writeIndex_) return;
    const double gap = static_cast<double>(index - writeIndex_) / fs_;
    if (s_.acq.source == SourceKind::Generator)
        for (auto& g : gens_) g.advance(gap);
    for (int c = 0; c < kNumChannels; ++c) {
        rings_[c].restart(index);
        ac_[c].reset();
    }
    trig_.reset();
    events_.clear();
    writeIndex_ = index;
}

void AcquisitionEngine::generateBlock(int64_t start, int64_t count) {
    int64_t done = 0;
    while (done < count) {
        const size_t m = static_cast<size_t>(std::min<int64_t>(count - done, 16384));
        produceRaw(start + done, m);
        commitBlock(start + done, m);
        done += static_cast<int64_t>(m);
    }
}

void AcquisitionEngine::produceRaw(int64_t index, size_t m) {
    const double dt = 1.0 / fs_;
    if (s_.acq.source == SourceKind::Generator) {
        std::array<bool, kNumGenerators> used{};
        for (int c = 0; c < kNumChannels; ++c)
            if (needed(c)) used[std::clamp(s_.ch[c].input, 0, kNumGenerators - 1)] = true;
        for (int g = 0; g < kNumGenerators; ++g) {
            if (used[g]) {
                genBuf_[g].resize(m);
                gens_[g].generate(genBuf_[g].data(), m, dt);
            } else {
                gens_[g].advance(static_cast<double>(m) * dt); // keep relative phases
            }
        }
        for (int c = 0; c < kNumChannels; ++c) {
            if (!needed(c)) continue;
            const auto& src = genBuf_[std::clamp(s_.ch[c].input, 0, kNumGenerators - 1)];
            raw_[c].assign(src.begin(), src.begin() + static_cast<long>(m));
        }
    } else { // File (looped playback at the file's sample rate, decimated)
        const auto len = static_cast<int64_t>(file_->length());
        for (int c = 0; c < kNumChannels; ++c) {
            if (!needed(c)) continue;
            raw_[c].resize(m);
            const int t = s_.ch[c].input;
            if (t < 0 || t >= static_cast<int>(file_->channels.size())) {
                std::fill(raw_[c].begin(), raw_[c].end(), 0.0f);
                continue;
            }
            const auto& d = file_->channels[t];
            int64_t pos = (index * decim_) % len;
            for (size_t k = 0; k < m; ++k) {
                raw_[c][k] = d[static_cast<size_t>(pos)];
                pos += decim_;
                if (pos >= len) pos %= len;
            }
        }
    }
}

void AcquisitionEngine::commitBlock(int64_t index, size_t m) {
    const int tsrc = std::clamp(s_.trigger.source, 0, kNumChannels - 1);
    newEvents_.clear();
    for (int c = 0; c < kNumChannels; ++c) {
        if (!needed(c)) { rings_[c].restart(index + static_cast<int64_t>(m)); continue; }
        proc_.assign(raw_[c].begin(), raw_[c].begin() + static_cast<long>(m));
        const auto& cs = s_.ch[c];
        if (cs.coupling == Coupling::GND) std::fill(proc_.begin(), proc_.end(), 0.0f);
        else if (cs.coupling == Coupling::AC) ac_[c].process(proc_.data(), m);
        if (cs.invert)
            for (auto& v : proc_) v = -v;
        if (rings_[c].writeIndex() != index) {
            rings_[c].restart(index);
            if (c == tsrc) trig_.reset();
        }
        rings_[c].append(proc_.data(), m);
        if (c == tsrc) trig_.process(proc_.data(), m, index, newEvents_);
    }
    for (double e : newEvents_) events_.push_back(e);
    while (events_.size() > 1000000) events_.pop_front();
    writeIndex_ = index + static_cast<int64_t>(m);
    acquired_ += m;
}

void AcquisitionEngine::acquirePush() {
    if (!push_) return;
    const int ch = push_->channels();
    if (ch <= 0) return;
    pushAccum_.resize(static_cast<size_t>(ch), 0.0);
    size_t budget = size_t{1} << 20;
    while (budget > 0) {
        const size_t want = std::min<size_t>(budget, 8192);
        pushBuf_.resize(want * static_cast<size_t>(ch));
        const size_t frames = push_->pull(pushBuf_.data(), want);
        if (frames == 0) break;
        budget -= frames;
        std::array<std::vector<float>, kNumChannels> out;
        // decimate (boxcar average = high-resolution acquisition) when decim > 1
        std::vector<std::vector<float>> dec(static_cast<size_t>(ch));
        for (size_t f = 0; f < frames; ++f) {
            for (int k = 0; k < ch; ++k) pushAccum_[k] += pushBuf_[f * ch + k];
            if (++pushAccumCount_ >= decim_) {
                for (int k = 0; k < ch; ++k) {
                    dec[k].push_back(static_cast<float>(pushAccum_[k] / decim_));
                    pushAccum_[k] = 0.0;
                }
                pushAccumCount_ = 0;
            }
        }
        const size_t m = dec.empty() ? 0 : dec[0].size();
        if (m == 0) continue;
        for (int c = 0; c < kNumChannels; ++c) {
            const int in = s_.ch[c].input;
            if (in >= 0 && in < ch) raw_[c] = dec[in];
            else raw_[c].assign(m, 0.0f);
        }
        commitBlock(writeIndex_, m);
    }
}

// =================================================================== frames
void AcquisitionEngine::window(double& tStart, double& tEnd) const {
    const double w = 10.0 * s_.timebase.secPerDiv;
    tStart = -std::clamp(s_.timebase.triggerPos, 0.0, 100.0) / 100.0 * w + s_.timebase.delay;
    tEnd = tStart + w;
}

bool AcquisitionEngine::isRoll() const {
    return s_.timebase.autoRoll && s_.trigger.mode == TriggerMode::Auto && run_ == Run::Running &&
           s_.timebase.secPerDiv >= kRollMinSecPerDiv;
}

void AcquisitionEngine::produce(double now) {
    double tStart, tEnd;
    window(tStart, tEnd);
    const double fs = fs_;
    const bool spectrumDue = now - lastSpecTime_ >= 1.0 / std::min(30, std::max(1, s_.acq.refreshRate));
    if (spectrumDue) { computeSpectrum(writeIndex_, true); lastSpecTime_ = now; }

    if (isRoll()) {
        if (writeIndex_ <= 0) return;
        state_ = TrigState::Roll;
        publishFrame(buildFrame(static_cast<double>(writeIndex_) - tEnd * fs, TrigState::Roll, false));
        return;
    }

    const int tsrc = std::clamp(s_.trigger.source, 0, kNumChannels - 1);
    const double lo = static_cast<double>(rings_[tsrc].validFrom());
    std::optional<double> best;
    while (!events_.empty()) {
        const double e = events_.front();
        if (e + tStart * fs - 1.0 < lo) { events_.pop_front(); continue; }        // history lost
        if (e + tEnd * fs + 2.0 >= static_cast<double>(writeIndex_)) break;    // post-trigger pending
        best = e;
        events_.pop_front();
    }
    auto finishSingle = [&] {
        if (run_ == Run::Single) {
            run_ = Run::Stopped;
            state_ = TrigState::Stopped;
        }
    };
    if (best) {
        lastTrigWall_ = now;
        state_ = TrigState::Triggered;
        publishFrame(buildFrame(*best, TrigState::Triggered, false));
        finishSingle();
        if (run_ == Run::Stopped) computeSpectrum(static_cast<int64_t>(std::ceil(*best + tEnd * fs)), false);
        return;
    }
    const double haveSamples = static_cast<double>(writeIndex_) - lo;
    if (forcePending_ && haveSamples > 2) {
        forcePending_ = false;
        state_ = TrigState::Triggered;
        publishFrame(buildFrame(static_cast<double>(writeIndex_) - tEnd * fs - 2.0, TrigState::Triggered, true));
        finishSingle();
        return;
    }
    const double w = tEnd - tStart;
    const double autoTimeout = std::max(0.15, 2.0 * w);
    if (s_.trigger.mode == TriggerMode::Auto && run_ == Run::Running && now - lastTrigWall_ > autoTimeout &&
        haveSamples > 2) {
        state_ = TrigState::Auto;
        publishFrame(buildFrame(static_cast<double>(writeIndex_) - tEnd * fs - 2.0, TrigState::Auto, false));
        return;
    }
    state_ = run_ == Run::Single ? TrigState::Armed : TrigState::Waiting;
}

std::shared_ptr<Frame> AcquisitionEngine::buildFrame(double trigPos, TrigState state, bool forced) {
    auto f = std::make_shared<Frame>();
    double tStart, tEnd;
    window(tStart, tEnd);
    const double fs = fs_;
    const auto i0 = static_cast<int64_t>(std::floor(trigPos + tStart * fs)) - 1;
    const auto i1 = static_cast<int64_t>(std::ceil(trigPos + tEnd * fs)) + 1;
    const int64_t len = i1 - i0 + 1;
    int64_t stride = 1;
    if (len > static_cast<int64_t>(kMaxRecord) + 8)
        stride = (len + static_cast<int64_t>(kMaxRecord) - 1) / static_cast<int64_t>(kMaxRecord);
    const size_t count = static_cast<size_t>((len + stride - 1) / stride);
    static const char* labels[] = {"CH1", "CH2", "CH3", "CH4", "MATH"};

    for (int c = 0; c < kNumChannels; ++c) {
        auto& tr = f->tr[c];
        tr.label = labels[c];
        tr.unit = channelUnit(c);
        if (!s_.ch[c].enabled) continue;
        tr.present = true;
        tr.y.resize(count);
        rings_[c].copy(i0, count, tr.y.data(), stride);
        tr.t0 = static_cast<double>(i0 - trigPos) / fs;
        tr.dt = static_cast<double>(stride) / fs;
        measureTrace(tr, tStart, tEnd);
    }

    if (s_.math.enabled) {
        auto& tr = f->tr[kMathTrace];
        tr.label = labels[kMathTrace];
        const int a = std::clamp(s_.math.srcA, 0, kNumChannels - 1);
        const int b = std::clamp(s_.math.srcB, 0, kNumChannels - 1);
        const bool bin = isBinary(s_.math.op);
        const double fsm = fs / static_cast<double>(stride);
        int64_t lo = rings_[a].validFrom();
        if (bin) lo = std::max(lo, rings_[b].validFrom());
        const int64_t maxLead = std::max<int64_t>(0, (i0 - lo) / stride);
        const auto lead = static_cast<size_t>(std::min<int64_t>(maxLead, static_cast<int64_t>(mathLeadIn(s_.math, fsm))));
        const size_t n = lead + count;
        std::vector<float> va(n), vb;
        const int64_t start = i0 - static_cast<int64_t>(lead) * stride;
        rings_[a].copy(start, n, va.data(), stride);
        if (bin) {
            vb.resize(n);
            rings_[b].copy(start, n, vb.data(), stride);
        }
        auto res = computeMath(s_.math, va.data(), bin ? vb.data() : va.data(), n, lead, fsm, channelUnit(a),
                               channelUnit(b));
        tr.present = true;
        tr.y = std::move(res.y);
        tr.unit = res.unit;
        tr.info = res.info;
        tr.warning = res.warning;
        tr.t0 = static_cast<double>(i0 - trigPos) / fs;
        tr.dt = static_cast<double>(stride) / fs;
        measureTrace(tr, tStart, tEnd);
    }

    f->fs = fs;
    f->nativeRate = native_;
    f->decim = decim_;
    f->tStart = tStart;
    f->tEnd = tEnd;
    f->state = state;
    f->forced = forced;
    f->settings = s_;
    retainedTrig_ = trigPos;
    retainedState_ = state;
    haveRetained_ = true;
    return f;
}

void AcquisitionEngine::measureTrace(TraceData& tr, double tStart, double tEnd) const {
    const auto n = static_cast<long>(tr.y.size());
    if (n < 2) return;
    const long k0 = std::max(0L, static_cast<long>(std::ceil((tStart - tr.t0) / tr.dt - 1e-6)));
    const long k1 = std::min(n - 1, static_cast<long>(std::floor((tEnd - tr.t0) / tr.dt + 1e-6)));
    long bestStart = 0, bestLen = 0, curStart = k0, curLen = 0;
    for (long k = k0; k <= k1; ++k) {
        if (std::isfinite(tr.y[k])) {
            if (curLen == 0) curStart = k;
            if (++curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
        } else {
            curLen = 0;
        }
    }
    if (bestLen >= 2) tr.meas = measure(tr.y.data() + bestStart, static_cast<size_t>(bestLen), tr.dt);
}

void AcquisitionEngine::publishFrame(std::shared_ptr<Frame> f) {
    f->seq = ++frameSeq_;
    ++statFrames_;
    std::lock_guard lk(outMutex_);
    frame_ = std::move(f);
}

// =================================================================== spectrum
void AcquisitionEngine::computeSpectrum(int64_t endIndex, bool accumulate) {
    const auto& sp = s_.spectrum;
    if (!sp.enabled) return;
    const int n = pow2Clamp(sp.fftSize, 1024, kMaxFft);
    const int src = std::clamp(sp.source, 0, kNumTraces - 1);
    auto out = std::make_shared<SpectrumData>();
    out->source = src;
    out->fs = fs_;
    out->fftSize = n;
    out->window = sp.window;
    out->binHz = fs_ / n;

    std::vector<float> x(static_cast<size_t>(n));
    const int64_t start = endIndex - n;
    if (src < kNumChannels) {
        if (start < rings_[src].validFrom() || endIndex > rings_[src].writeIndex()) return; // not enough data yet
        rings_[src].copy(start, static_cast<size_t>(n), x.data());
        out->unit = channelUnit(src);
    } else {
        if (!s_.math.enabled) {
            out->message = "MATH is off";
            std::lock_guard lk(outMutex_);
            spectrum_ = out;
            return;
        }
        const int a = std::clamp(s_.math.srcA, 0, kNumChannels - 1), b = std::clamp(s_.math.srcB, 0, kNumChannels - 1);
        const bool bin = isBinary(s_.math.op);
        int64_t lo = rings_[a].validFrom();
        if (bin) lo = std::max(lo, rings_[b].validFrom());
        if (start < lo || endIndex > writeIndex_) return;
        const size_t lead = static_cast<size_t>(std::min<int64_t>(start - lo, static_cast<int64_t>(mathLeadIn(s_.math, fs_))));
        const size_t total = lead + static_cast<size_t>(n);
        std::vector<float> va(total), vb;
        rings_[a].copy(start - static_cast<int64_t>(lead), total, va.data());
        if (bin) {
            vb.resize(total);
            rings_[b].copy(start - static_cast<int64_t>(lead), total, vb.data());
        }
        auto r = computeMath(s_.math, va.data(), bin ? vb.data() : va.data(), total, lead, fs_, channelUnit(a), channelUnit(b));
        x = std::move(r.y);
        out->unit = r.unit;
    }
    for (float v : x)
        if (!std::isfinite(v)) return;

    analyzer_.configure(static_cast<size_t>(n), sp.window);
    analyzer_.compute(x.data(), specTmp_);
    char key[160];
    std::snprintf(key, sizeof key, "%d/%d/%d/%.9g/%s", src, n, static_cast<int>(sp.window), fs_, out->unit.c_str());
    if (key != specKey_ || !accumulate || specAvg_.size() != specTmp_.size()) { specKey_ = key; specCount_ = 0; }
    const int avg = std::clamp(sp.averages, 1, 256);
    if (specCount_ == 0) specAvg_ = specTmp_;
    else {
        const float alpha = 1.0f / static_cast<float>(std::min(specCount_ + 1, avg));
        for (size_t k = 0; k < specAvg_.size(); ++k) specAvg_[k] += alpha * (specTmp_[k] - specAvg_[k]);
    }
    ++specCount_;
    out->valid = true;
    out->enbwBins = analyzer_.enbwBins();
    out->averaged = std::min(specCount_, avg);
    out->power = specAvg_;
    out->peaks = findPeaks(out->power, out->binHz, sp.window, out->enbwBins, 8, 20.0);
    out->harmonics = analyzeHarmonics(out->power, out->binHz, sp.window, out->enbwBins, 10);
    out->seq = ++specSeq_;
    std::lock_guard lk(outMutex_);
    spectrum_ = std::move(out);
}

// =================================================================== autoscale
void AcquisitionEngine::doAutoscale() {
    autoscalePending_ = false;
    ScopeSettings r = s_;
    struct Info { bool ok = false; double vmin = 0, vmax = 0, freq = 0; bool freqOk = false; };
    std::array<Info, kNumChannels> info;
    const size_t N = size_t{1} << 18;

    auto analyse = [&](int c, double dt, Info& inf) {
        std::vector<float> x(N);
        size_t n = N;
        if (s_.acq.source == SourceKind::Generator) {
            SignalGenerator g = gens_[std::clamp(s_.ch[c].input, 0, kNumGenerators - 1)]; // copy: keep live phase
            g.generate(x.data(), N, dt);
        } else if (s_.acq.source == SourceKind::File && file_ && file_->length() > 0) {
            const int t = s_.ch[c].input;
            if (t < 0 || t >= static_cast<int>(file_->channels.size())) return;
            const auto& d = file_->channels[t];
            const size_t step = std::max<size_t>(1, static_cast<size_t>(std::llround(dt * file_->sampleRate)));
            n = std::min(N, d.size() / step);
            for (size_t i = 0; i < n; ++i) x[i] = d[i * step];
        } else {
            const int64_t end = rings_[c].writeIndex();
            const int64_t start = std::max(rings_[c].validFrom(), end - static_cast<int64_t>(N));
            n = static_cast<size_t>(end - start);
            rings_[c].copy(start, n, x.data());
            dt = 1.0 / fs_;
        }
        if (n < 16) return;
        x.resize(n);
        if (s_.ch[c].coupling == Coupling::GND) std::fill(x.begin(), x.end(), 0.0f);
        else if (s_.ch[c].coupling == Coupling::AC && randomAccessSource()) {
            double mean = 0;
            for (float v : x) mean += v;
            mean /= static_cast<double>(n);
            for (auto& v : x) v = static_cast<float>(v - mean);
        }
        if (s_.ch[c].invert)
            for (auto& v : x) v = -v;
        auto m = measure(x.data(), n, dt);
        inf.ok = true;
        inf.vmin = m[MeasId::Min].v;
        inf.vmax = m[MeasId::Max].v;
        if (m[MeasId::Freq].valid) { inf.freq = m[MeasId::Freq].v; inf.freqOk = true; }
    };

    for (int c = 0; c < kNumChannels; ++c) {
        if (!r.ch[c].enabled) continue;
        const double dt1 = randomAccessSource() ? 1.0 / native_ : 1.0 / fs_;
        analyse(c, dt1, info[c]);
        if (info[c].ok && !info[c].freqOk && randomAccessSource() && N * dt1 < 2.0) {
            Info slow;
            analyse(c, 4.0 / static_cast<double>(N), slow); // 4 s span
            if (slow.freqOk) { info[c].freq = slow.freq; info[c].freqOk = true; }
        }
    }

    std::vector<int> active;
    for (int c = 0; c < kNumChannels; ++c)
        if (r.ch[c].enabled && info[c].ok) active.push_back(c);
    const int k = static_cast<int>(active.size());
    int trigCh = -1;
    for (int i = 0; i < k; ++i) {
        const int c = active[i];
        const double band = 8.0 / k;                  // divisions available to this channel
        const double centre = 4.0 - band * (i + 0.5); // band centre (div from screen centre)
        const double vpp = info[c].vmax - info[c].vmin;
        const double mid = 0.5 * (info[c].vmax + info[c].vmin);
        double vdiv;
        if (vpp > 1e-9 && vpp > 1e-6 * std::fabs(mid)) {
            vdiv = std::clamp(ceil125(vpp / (0.8 * band)), 1e-3, 10.0);
            r.ch[c].position = centre - mid / vdiv;
        } else {
            vdiv = std::clamp(ceil125(std::max(std::fabs(mid) / (0.4 * band), 1e-3)), 1e-3, 10.0);
            r.ch[c].position = centre - mid / vdiv;
        }
        r.ch[c].voltsPerDiv = vdiv;
        if (trigCh < 0 && info[c].freqOk) trigCh = c;
    }
    if (trigCh < 0 && k > 0) trigCh = active[0];
    if (trigCh >= 0) {
        r.trigger.source = trigCh;
        r.trigger.level = 0.5 * (info[trigCh].vmax + info[trigCh].vmin);
        r.trigger.slope = TriggerSlope::Rising;
        r.trigger.holdoff = 0.0;
        if (info[trigCh].freqOk) {
            r.timebase.secPerDiv = std::clamp(ceil125(2.0 / info[trigCh].freq / 10.0), 1e-8, 50.0);
            // spectrum span: fundamental + harmonics up to ~20x (or Nyquist)
            const double stop = ceil125(20.0 * info[trigCh].freq);
            r.spectrum.fStart = 0.0;
            r.spectrum.fStop = stop >= 0.5 * native_ ? 0.0 : stop;
        }
    }
    r.trigger.mode = TriggerMode::Auto;
    r.timebase.delay = 0.0;
    r.timebase.triggerPos = 50.0;
    if (r.math.enabled) {
        std::shared_ptr<const Frame> f;
        {
            std::lock_guard lk(outMutex_);
            f = frame_;
        }
        if (f && f->tr[kMathTrace].present && f->tr[kMathTrace].meas[MeasId::PkPk].valid) {
            const auto& m = f->tr[kMathTrace].meas;
            const double vpp = m[MeasId::PkPk].v;
            const double mid = 0.5 * (m[MeasId::Max].v + m[MeasId::Min].v);
            if (vpp > 0) {
                r.math.unitsPerDiv = ceil125(vpp / 6.0);
                r.math.position = -mid / r.math.unitsPerDiv;
            }
        }
    }
    std::lock_guard lk(outMutex_);
    autoscale_ = r;
}

// =================================================================== tick
void AcquisitionEngine::publishStatus(double now) {
    if (now - statT0_ >= 0.5) {
        frameRate_ = statFrames_ / (now - statT0_);
        statFrames_ = 0;
        statT0_ = now;
    }
    EngineStatus st;
    st.running = run_ != Run::Stopped;
    st.state = run_ == Run::Stopped ? TrigState::Stopped : state_;
    st.fs = fs_;
    st.nativeRate = native_;
    st.decim = decim_;
    st.frameRate = frameRate_;
    st.segmented = segmented_ && randomAccessSource();
    st.acquired = acquired_;
    st.audioDropped = push_ ? push_->dropped() : 0;
    if (s_.acq.source == SourceKind::File && (!file_ || file_->length() == 0)) st.message = "No file loaded";
    else if (s_.acq.source == SourceKind::Audio && (!push_ || push_->rate() <= 0)) st.message = "Audio input not started";
    else if (s_.acq.source == SourceKind::Serial) st.message = "Serial ADC input is not implemented yet";
    std::lock_guard lk(outMutex_);
    status_ = st;
}

void AcquisitionEngine::tick(double now) {
    pullInputs();
    if (!haveSettings_) return;
    if (run_ != Run::Stopped) {
        if (needRestart_) restartAcquisition(now);
        acquire(now);
        const double interval = 1.0 / std::clamp(s_.acq.refreshRate, 1, 240);
        if (now - lastFrameTime_ >= interval) {
            lastFrameTime_ = now;
            produce(now);
        }
        needRedraw_ = false;
    } else if (needRedraw_) {
        needRedraw_ = false;
        if (haveRetained_) {
            // Stopped: re-extract the record from acquisition memory with the new
            // timebase / scale / math settings (pan & zoom through the stored data).
            const auto f = buildFrame(retainedTrig_, retainedState_, false);
            publishFrame(f);
            computeSpectrum(std::min<int64_t>(writeIndex_, static_cast<int64_t>(std::ceil(retainedTrig_ + f->tEnd * fs_))), false);
        }
    }
    if (autoscalePending_) doAutoscale();
    publishStatus(now);
}

} // namespace slp
