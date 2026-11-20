// SIGNAL LAB PRO - microphone / line-in capture (Qt Multimedia).
//
// Audio samples are delivered in normalised full-scale units (FS, -1..+1). They are NOT
// volts: a microphone chain (capsule, preamp, ADC gain, AGC) has no known calibration.
// Measuring real circuit voltages requires a proper ADC front end (see the serial ADC
// interface in the README).
#pragma once

#include "Acquisition.h"

#include <QObject>
#include <QStringList>

#include <memory>
#include <vector>

#if defined(SLP_HAVE_AUDIO)
#include <QAudioFormat>
class QAudioSource;
class QIODevice;
#endif

namespace slp {

class AudioInput : public QObject {
    Q_OBJECT
public:
    explicit AudioInput(std::shared_ptr<PushSource> sink, QObject* parent = nullptr);
    ~AudioInput() override;

    static bool available();
    static QStringList deviceNames();
    bool start(int deviceIndex, QString* error);
    void stop();
    bool running() const;
    QString description() const { return description_; }

signals:
    void stateChanged(const QString& message);

private slots:
    void readData();

private:
    std::shared_ptr<PushSource> sink_;
    std::vector<float> conv_;
    QString description_;
#if defined(SLP_HAVE_AUDIO)
    QAudioSource* src_ = nullptr;
    QIODevice* io_ = nullptr;
    QAudioFormat fmt_;
#endif
};

} // namespace slp
