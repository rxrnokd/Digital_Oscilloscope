// SIGNAL LAB PRO - waveform renderer (10 x 8 division graticule).
//
// The view always maps the frame's physical data (seconds / volts) with the CURRENT
// settings, so V/div, position and time/div changes are visible immediately, even while
// the acquisition is stopped. Rendering:
//   * > 2 samples per device pixel : min/max envelope per pixel column (peak detect)
//   * sparse data                  : sin(x)/x (Lanczos-6) or linear reconstruction
//   * optional phosphor persistence and glow
#pragma once

#include "Acquisition.h"
#include "ScopeSettings.h"

#include <QImage>
#include <QWidget>

#include <memory>
#include <vector>

namespace slp {

class ScopeView : public QWidget {
    Q_OBJECT
public:
    explicit ScopeView(QWidget* parent = nullptr);

    void setSettings(const ScopeSettings& s);
    void setFrame(std::shared_ptr<const Frame> f);
    void setStatus(const EngineStatus& st);
    std::shared_ptr<const Frame> frame() const { return frame_; }
    QSize minimumSizeHint() const override { return {480, 300}; }

signals:
    void settingsEdited(const slp::ScopeSettings& s);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    enum class Drag { None, Pan, Zoom, CursorX1, CursorX2, CursorY1, CursorY2, Level, Position, TrigPos };

    QRectF plotRect() const;
    double windowWidth() const;
    double windowStart() const;
    double xOf(const QRectF& pr, double t) const;
    double tOf(const QRectF& pr, double x) const;
    double yOf(const QRectF& pr, double v, int trace) const;
    double vOf(const QRectF& pr, double y, int trace) const;
    double traceScale(int i) const;
    double tracePos(int i) const;
    QColor traceColor(int i) const;
    bool traceEnabled(int i) const;
    QString traceUnit(int i) const;
    double traceValueAt(int i, double t) const;

    void drawGrid(QPainter& p, const QRectF& pr);
    void drawTraces(QPainter& p, const QRectF& pr, double alpha, bool glow);
    void drawTrace(QPainter& p, const QRectF& pr, const TraceData& tr, int idx, double alpha, bool glow);
    void drawPersistence(QPainter& p, const QRectF& pr);
    void drawMarkers(QPainter& p, const QRectF& pr);
    void drawCursors(QPainter& p, const QRectF& pr);
    void drawAxisAndBadges(QPainter& p, const QRectF& pr);
    void drawOverlay(QPainter& p, const QRectF& pr);
    void emitEdited();

    ScopeSettings s_;
    std::shared_ptr<const Frame> frame_;
    EngineStatus st_;
    Drag drag_ = Drag::None;
    int dragTrace_ = 0;
    QPointF pressPos_, curPos_, hoverPos_;
    bool hover_ = false;
    double pressValue_ = 0.0;
    int wheelAccum_ = 0;
    std::vector<std::pair<QRectF, int>> badgeRects_;
    QImage persist_;
    uint64_t persistSeq_ = 0;
    QString persistKey_;
};

} // namespace slp
