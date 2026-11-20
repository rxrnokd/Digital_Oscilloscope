#include "AudioInput.h"

#if defined(SLP_HAVE_AUDIO)
#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#endif

#include <cstring>

namespace slp {

AudioInput::AudioInput(std::shared_ptr<PushSource> sink, QObject* parent) : QObject(parent), sink_(std::move(sink)) {}

AudioInput::~AudioInput() { stop(); }

bool AudioInput::available() {
#if defined(SLP_HAVE_AUDIO)
    return true;
#else
    return false;
#endif
}

QStringList AudioInput::deviceNames() {
    QStringList out;
#if defined(SLP_HAVE_AUDIO)
    const auto def = QMediaDevices::defaultAudioInput();
    for (const auto& d : QMediaDevices::audioInputs())
        out << d.description() + (d.id() == def.id() ? QStringLiteral("  (default)") : QString());
#endif
    return out;
}

bool AudioInput::running() const {
#if defined(SLP_HAVE_AUDIO)
    return src_ != nullptr;
#else
    return false;
#endif
}

bool AudioInput::start(int deviceIndex, QString* error) {
#if defined(SLP_HAVE_AUDIO)
    stop();
    const auto devices = QMediaDevices::audioInputs();
    if (devices.isEmpty()) {
        if (error) *error = tr("No audio input device found");
        return false;
    }
    const QAudioDevice dev = (deviceIndex >= 0 && deviceIndex < devices.size()) ? devices[deviceIndex] : QMediaDevices::defaultAudioInput();
    QAudioFormat f = dev.preferredFormat();
    QAudioFormat want = f;
    want.setSampleFormat(QAudioFormat::Float);
    want.setChannelCount(std::min(2, std::max(1, dev.maximumChannelCount())));
    if (want.sampleRate() <= 0) want.setSampleRate(48000);
    if (dev.isFormatSupported(want)) f = want;
    fmt_ = f;
    src_ = new QAudioSource(dev, fmt_, this);
    src_->setBufferSize(fmt_.bytesForDuration(100000)); // 100 ms device buffer
    io_ = src_->start();
    if (!io_) {
        if (error) *error = tr("Could not open the audio device (permission denied?)");
        delete src_;
        src_ = nullptr;
        return false;
    }
    sink_->configure(fmt_.sampleRate(), fmt_.channelCount());
    connect(io_, &QIODevice::readyRead, this, &AudioInput::readData);
    description_ = QStringLiteral("%1  -  %2 Hz, %3 ch").arg(dev.description()).arg(fmt_.sampleRate()).arg(fmt_.channelCount());
    emit stateChanged(tr("Audio input running: ") + description_);
    return true;
#else
    Q_UNUSED(deviceIndex);
    if (error) *error = tr("This build has no Qt Multimedia support");
    return false;
#endif
}

void AudioInput::stop() {
#if defined(SLP_HAVE_AUDIO)
    if (src_) {
        src_->stop();
        src_->deleteLater();
        src_ = nullptr;
        io_ = nullptr;
        sink_->configure(0.0, 0);
        emit stateChanged(tr("Audio input stopped"));
    }
#endif
}

void AudioInput::readData() {
#if defined(SLP_HAVE_AUDIO)
    if (!io_) return;
    const QByteArray data = io_->readAll();
    const int bpf = fmt_.bytesPerFrame();
    const int ch = fmt_.channelCount();
    if (bpf <= 0 || ch <= 0) return;
    const qsizetype frames = data.size() / bpf;
    conv_.resize(static_cast<size_t>(frames * ch));
    const char* p = data.constData();
    const qsizetype n = frames * ch;
    switch (fmt_.sampleFormat()) {
    case QAudioFormat::Float: std::memcpy(conv_.data(), p, static_cast<size_t>(n) * sizeof(float)); break;
    case QAudioFormat::Int16:
        for (qsizetype i = 0; i < n; ++i) {
            int16_t v;
            std::memcpy(&v, p + 2 * i, 2);
            conv_[i] = v / 32768.0f;
        }
        break;
    case QAudioFormat::Int32:
        for (qsizetype i = 0; i < n; ++i) {
            int32_t v;
            std::memcpy(&v, p + 4 * i, 4);
            conv_[i] = static_cast<float>(v / 2147483648.0);
        }
        break;
    case QAudioFormat::UInt8:
        for (qsizetype i = 0; i < n; ++i) conv_[i] = (static_cast<uint8_t>(p[i]) - 128) / 128.0f;
        break;
    default: return;
    }
    sink_->push(conv_.data(), static_cast<size_t>(frames));
#endif
}

} // namespace slp
