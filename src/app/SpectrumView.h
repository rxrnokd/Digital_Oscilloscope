// SIGNAL LAB PRO - spectrum analyzer display + real-time spectrogram (waterfall).
#pragma once

#include "Acquisition.h"
#include "ScopeSettings.h"

#include <QImage>
#include <QWidget>

#include <array>
#include <memory>

namespace slp {

class SpectrumView : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumView(QWidget* parent = nullptr);
    void setSettings(const ScopeSettings& s);
    void setSpectrum(std::shared_ptr<const SpectrumData> d);
    QSize minimumSizeHint() const override { return {400, 160}; }

signals:
    void settingsEdited(const slp::ScopeSettings& s);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF specRect() const;
    QRectF waterfallRect() const;
    double fStart() const;
    double fStop() const;
    double nyquist() const;
    double levelOf(double power) const; // dB or linear display value
    double yOfLevel(const QRectF& r, double level) const;
    QString levelUnit() const;
    QString levelText(double power) const;
    void columnLevels(int cols, std::vector<double>& out) const; // max level per column
    void drawSpectrum(QPainter& p, const QRectF& r);
    void drawWaterfall(QPainter& p, const QRectF& r);
    void pushWaterfallRow();

    ScopeSettings s_;
    std::shared_ptr<const SpectrumData> d_;
    uint64_t lastSeq_ = 0;
    QImage waterfall_;
    QString waterfallKey_;
    std::array<QRgb, 256> cmap_{};
    bool dragging_ = false;
    QPointF pressPos_, hoverPos_;
    bool hover_ = false;
    double pressStart_ = 0, pressStop_ = 0, pressRef_ = 0;
    int wheelAccum_ = 0;
};

} // namespace slp
