#include "SpectrumView.h"

#include "Controls.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slp {

using ui::fmt;
using ui::monoFont;
using ui::toQColor;

namespace {
constexpr double kL = 56, kR = 16, kT = 22, kB = 20;

QColor lerp(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}
} // namespace

SpectrumView::SpectrumView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    s_ = ScopeSettings::defaults();
    // perceptual "inferno"-like colormap
    const std::pair<double, QColor> stops[] = {
        {0.00, QColor(0, 0, 4)},      {0.15, QColor(22, 11, 57)},  {0.32, QColor(85, 15, 109)},
        {0.50, QColor(140, 35, 105)}, {0.66, QColor(195, 58, 80)}, {0.80, QColor(237, 105, 37)},
        {0.92, QColor(251, 180, 26)}, {1.00, QColor(252, 255, 164)}};
    for (int i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        int k = 0;
        while (k < 6 && stops[k + 1].first < t) ++k;
        const double u = (t - stops[k].first) / (stops[k + 1].first - stops[k].first);
        cmap_[i] = lerp(stops[k].second, stops[k + 1].second, std::clamp(u, 0.0, 1.0)).rgb();
    }
}

void SpectrumView::setSettings(const ScopeSettings& s) {
    s_ = s;
    update();
}

void SpectrumView::setSpectrum(std::shared_ptr<const SpectrumData> d) {
    d_ = std::move(d);
    if (d_ && d_->valid && d_->seq != lastSeq_) {
        lastSeq_ = d_->seq;
        if (s_.spectrum.spectrogram) pushWaterfallRow();
    }
    update();
}

// ------------------------------------------------------------------ geometry
QRectF SpectrumView::specRect() const {
    const double h = height();
    const double specH = s_.spectrum.spectrogram ? (h - kT - kB) * 0.58 : h - kT - kB;
    return QRectF(kL, kT, std::max(10.0, width() - kL - kR), std::max(10.0, specH));
}
QRectF SpectrumView::waterfallRect() const {
    const QRectF s = specRect();
    const double top = s.bottom() + kB + 6;
    return QRectF(kL, top, s.width(), std::max(0.0, height() - top - 6));
}
double SpectrumView::nyquist() const { return d_ && d_->fs > 0 ? d_->fs / 2 : 0.5 * s_.acq.sampleRate; }
double SpectrumView::fStart() const { return std::clamp(s_.spectrum.fStart, 0.0, nyquist()); }
double SpectrumView::fStop() const {
    const double ny = nyquist();
    double f = s_.spectrum.fStop > 0 ? std::min(s_.spectrum.fStop, ny) : ny;
    if (f <= fStart()) f = ny;
    return f;
}

double SpectrumView::levelOf(double p) const {
    const bool fs = d_ && d_->unit == "FS";
    if (s_.spectrum.logScale) return 10.0 * std::log10(std::max(p, 1e-30)) + (fs ? 3.0103 : 0.0);
    return std::sqrt(std::max(p, 0.0));
}
double SpectrumView::yOfLevel(const QRectF& r, double level) const {
    if (s_.spectrum.logScale) {
        const double range = 10.0 * s_.spectrum.dbPerDiv;
        return r.top() + (s_.spectrum.refLevel - level) / range * r.height();
    }
    const double top = std::pow(10.0, s_.spectrum.refLevel / 20.0);
    return r.bottom() - level / top * r.height();
}
QString SpectrumView::levelUnit() const {
    const std::string u = d_ ? d_->unit : "V";
    if (s_.spectrum.logScale) return u == "FS" ? "dBFS" : u == "V" ? "dBV" : ui::qs("dB" + u);
    return ui::qs(u + "rms");
}
QString SpectrumView::levelText(double p) const {
    const double l = levelOf(p);
    if (s_.spectrum.logScale) return QString::number(l, 'f', 2) + " " + levelUnit();
    return fmt(l, (d_ ? d_->unit + "rms" : std::string("Vrms")).c_str());
}

void SpectrumView::columnLevels(int cols, std::vector<double>& out) const {
    out.assign(cols, -1e300);
    if (!d_ || !d_->valid || d_->power.empty()) return;
    const auto& pw = d_->power;
    const int m = static_cast<int>(pw.size());
    const double bin = d_->binHz, f0 = fStart(), span = fStop() - f0;
    for (int c = 0; c < cols; ++c) {
        const double fa = f0 + span * c / cols, fb = f0 + span * (c + 1) / cols;
        int ka = static_cast<int>(std::ceil(fa / bin)), kb = static_cast<int>(std::floor(fb / bin));
        ka = std::max(ka, 0);
        kb = std::min(kb, m - 1);
        double p;
        if (kb >= ka) {
            p = 0;
            for (int k = ka; k <= kb; ++k) p = std::max(p, static_cast<double>(pw[k]));
        } else {
            const double kf = std::clamp(0.5 * (fa + fb) / bin, 0.0, m - 1.0);
            const int k = std::min(static_cast<int>(kf), m - 2);
            const double a = kf - k;
            p = pw[k] * (1 - a) + pw[k + 1] * a;
        }
        out[c] = levelOf(p);
    }
}

// ------------------------------------------------------------------ painting
void SpectrumView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x08, 0x0a, 0x0d));
    drawSpectrum(p, specRect());
    if (s_.spectrum.spectrogram) drawWaterfall(p, waterfallRect());
}

void SpectrumView::drawSpectrum(QPainter& p, const QRectF& r) {
    QLinearGradient bg(r.topLeft(), r.bottomLeft());
    bg.setColorAt(0, QColor(0x0b, 0x0e, 0x13));
    bg.setColorAt(1, QColor(0x06, 0x07, 0x09));
    p.fillRect(r, bg);
    const QColor grid(255, 255, 255, static_cast<int>(40 * s_.display.gridIntensity + 8));
    QPen gp(grid, 1.0);
    gp.setDashPattern({1, 3});
    p.setPen(gp);
    for (int i = 1; i < 10; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 10, r.top()), QPointF(r.left() + r.width() * i / 10, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 10), QPointF(r.right(), r.top() + r.height() * i / 10));
    }
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    // axis labels
    p.setFont(monoFont(8.5));
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    for (int i = 0; i <= 10; ++i) {
        const double y = r.top() + r.height() * i / 10;
        QString lbl;
        if (s_.spectrum.logScale) lbl = QString::number(s_.spectrum.refLevel - i * s_.spectrum.dbPerDiv, 'f', s_.spectrum.dbPerDiv < 1 ? 1 : 0);
        else lbl = fmt(std::pow(10.0, s_.spectrum.refLevel / 20.0) * (10 - i) / 10.0, "", 3);
        p.drawText(QRectF(2, y - 7, kL - 6, 14), Qt::AlignRight | Qt::AlignVCenter, lbl);
    }
    const double f0 = fStart(), f1 = fStop();
    for (int i = 0; i <= 10; i += (r.width() < 600 ? 2 : 1)) {
        const double x = r.left() + r.width() * i / 10;
        const double f = f0 + (f1 - f0) * i / 10;
        Qt::Alignment al = Qt::AlignHCenter | Qt::AlignTop;
        QRectF lr(x - 40, r.bottom() + 3, 80, 14);
        if (i == 0) { lr.moveLeft(x - 2); al = Qt::AlignLeft | Qt::AlignTop; }
        if (i == 10) { lr.moveRight(x + 2); al = Qt::AlignRight | Qt::AlignTop; }
        p.drawText(lr, al, fmt(f, "Hz", 3));
    }
    p.save();
    p.translate(10, r.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-60, -10, 120, 14), Qt::AlignCenter, levelUnit());
    p.restore();

    const int src = std::clamp(s_.spectrum.source, 0, kNumTraces - 1);
    const QColor tc = toQColor(src < kNumChannels ? s_.ch[src].color : s_.math.color);
    const QString srcName = src < kNumChannels ? QStringLiteral("CH%1").arg(src + 1) : QStringLiteral("MATH");

    // header line
    p.setFont(monoFont(8.5, true));
    p.setPen(tc);
    p.drawText(QRectF(r.left(), 3, 70, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("FFT  ") + srcName);
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    p.setFont(monoFont(8.5));
    if (!s_.spectrum.enabled) {
        p.drawText(r, Qt::AlignCenter, tr("Spectrum analyzer is off"));
        return;
    }
    if (!d_ || !d_->valid) {
        p.drawText(r, Qt::AlignCenter, d_ && !d_->message.empty() ? ui::qs(d_->message) : tr("Waiting for data..."));
        return;
    }
    const QString hdr = QString::fromUtf8("N %1   %2   RBW %3   \xCE\x94" "f %4   Nyquist %5   Avg %6/%7")
                            .arg(d_->fftSize)
                            .arg(windowName(d_->window))
                            .arg(fmt(d_->binHz * d_->enbwBins, "Hz", 3))
                            .arg(fmt(d_->binHz, "Hz", 3))
                            .arg(fmt(d_->fs / 2, "Hz", 3))
                            .arg(d_->averaged)
                            .arg(s_.spectrum.averages);
    p.drawText(QRectF(r.left() + 76, 3, r.width() - 76, 16), Qt::AlignLeft | Qt::AlignVCenter, hdr);

    // trace
    const int cols = std::max(2, static_cast<int>(r.width()));
    std::vector<double> lv;
    columnLevels(cols, lv);
    QPolygonF line;
    line.reserve(cols);
    for (int c = 0; c < cols; ++c) {
        const double y = std::clamp(yOfLevel(r, lv[c]), r.top(), r.bottom());
        line << QPointF(r.left() + (c + 0.5) * r.width() / cols, y);
    }
    p.save();
    p.setClipRect(r);
    QPolygonF fill = line;
    fill << QPointF(r.right(), r.bottom()) << QPointF(r.left(), r.bottom());
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    QColor c0 = tc, c1 = tc;
    c0.setAlpha(110);
    c1.setAlpha(8);
    g.setColorAt(0, c0);
    g.setColorAt(1, c1);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawPolygon(fill);
    QColor glow = tc;
    glow.setAlpha(60);
    p.setPen(QPen(glow, 3.5));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(line);
    p.setPen(QPen(tc.lighter(115), 1.3));
    p.drawPolyline(line);

    // peak markers
    p.setFont(monoFont(8));
    int shown = 0;
    std::vector<double> usedX;
    for (const auto& pk : d_->peaks) {
        if (pk.freq < f0 || pk.freq > f1 || shown >= 5) continue;
        const double x = r.left() + (pk.freq - f0) / (f1 - f0) * r.width();
        bool crowded = false;
        for (double ux : usedX) crowded |= std::fabs(ux - x) < 90.0;
        if (crowded) continue;
        usedX.push_back(x);
        const double y = std::clamp(yOfLevel(r, levelOf(pk.power)), r.top() + 40, r.bottom());
        QPainterPath tri;
        tri.moveTo(x, y - 3);
        tri.lineTo(x - 5, y - 11);
        tri.lineTo(x + 5, y - 11);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(ui::theme::amber);
        p.drawPath(tri);
        p.setPen(QColor(0xE6, 0xED, 0xF3));
        const QString t1 = fmt(pk.freq, "Hz", 4);
        p.drawText(QRectF(x - 60, y - 36, 120, 12), Qt::AlignCenter, t1);
        p.setPen(QColor(0xFF, 0xB0, 0x20));
        p.drawText(QRectF(x - 60, y - 24, 120, 12), Qt::AlignCenter, levelText(pk.power));
        ++shown;
    }
    p.restore();

    // harmonic / THD box
    if (d_->harmonics.valid && d_->harmonics.harmonics.size() >= 2) {
        const auto& h = d_->harmonics;
        QStringList lines;
        lines << tr("Fund   ") + fmt(h.fundamental, "Hz", 5) + "  " + levelText(h.harmonics[0].power);
        const double thdDb = 20.0 * std::log10(std::max(h.thdRatio, 1e-12));
        lines << QStringLiteral("THD    %1 %   (%2 dB)").arg(h.thdRatio * 100.0, 0, 'f', h.thdRatio < 0.01 ? 4 : 3).arg(thdDb, 0, 'f', 1);
        QString hl;
        for (size_t i = 1; i < h.harmonics.size() && i < 6; ++i) {
            const double dbc = 10.0 * std::log10(std::max(h.harmonics[i].power, 1e-30) / h.harmonics[0].power);
            hl += QStringLiteral("H%1 %2  ").arg(h.harmonics[i].order).arg(dbc, 0, 'f', 1);
        }
        lines << hl.trimmed() + " dBc";
        QFont f = monoFont(8.5);
        p.setFont(f);
        const QFontMetricsF fm(f);
        double w = 0;
        for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const QRectF box(r.right() - w - 18, r.top() + 6, w + 12, lines.size() * fm.height() + 8);
        p.setPen(QPen(QColor(tc.red(), tc.green(), tc.blue(), 110), 1));
        p.setBrush(QColor(8, 10, 14, 215));
        p.drawRoundedRect(box, 5, 5);
        p.setPen(QColor(0xE6, 0xED, 0xF3));
        for (int i = 0; i < lines.size(); ++i) p.drawText(QPointF(box.left() + 6, box.top() + 4 + fm.ascent() + i * fm.height()), lines[i]);
    }

    // hover readout
    if (hover_ && r.contains(hoverPos_)) {
        const double f = f0 + (hoverPos_.x() - r.left()) / r.width() * (f1 - f0);
        const int k = std::clamp(static_cast<int>(std::lround(f / d_->binHz)), 0, static_cast<int>(d_->power.size()) - 1);
        const QString txt = fmt(k * d_->binHz, "Hz", 5) + "   " + levelText(d_->power[k]);
        p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
        p.drawLine(QPointF(hoverPos_.x(), r.top()), QPointF(hoverPos_.x(), r.bottom()));
        p.setFont(monoFont(9));
        const QFontMetricsF fm(p.font());
        const QRectF b(r.left() + 6, r.bottom() - 22, fm.horizontalAdvance(txt) + 12, 17);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 10, 14, 210));
        p.drawRoundedRect(b, 4, 4);
        p.setPen(QColor(0xC9, 0xD1, 0xD9));
        p.drawText(b, Qt::AlignCenter, txt);
    }
}

void SpectrumView::pushWaterfallRow() {
    const QRectF r = waterfallRect();
    const double dpr = devicePixelRatioF();
    const QSize sz(std::max(1, static_cast<int>(r.width() * dpr)), std::max(1, static_cast<int>(r.height() * dpr)));
    const QString key = QStringLiteral("%1x%2|%3|%4|%5|%6|%7|%8")
                            .arg(sz.width()).arg(sz.height()).arg(fStart()).arg(fStop()).arg(s_.spectrum.refLevel)
                            .arg(s_.spectrum.dbPerDiv).arg(s_.spectrum.logScale).arg(s_.spectrum.source);
    if (waterfall_.size() != sz || key != waterfallKey_) {
        waterfall_ = QImage(sz, QImage::Format_RGB32);
        waterfall_.setDevicePixelRatio(dpr);
        waterfall_.fill(QColor(0, 0, 4));
        waterfallKey_ = key;
    }
    if (sz.height() > 1) {
        const qsizetype bpl = waterfall_.bytesPerLine();
        uchar* bits = waterfall_.bits();
        std::memmove(bits + bpl, bits, static_cast<size_t>(bpl) * (sz.height() - 1));
    }
    std::vector<double> lv;
    columnLevels(sz.width(), lv);
    auto* row = reinterpret_cast<QRgb*>(waterfall_.scanLine(0));
    const double lo = s_.spectrum.logScale ? s_.spectrum.refLevel - 10.0 * s_.spectrum.dbPerDiv : 0.0;
    const double hi = s_.spectrum.logScale ? s_.spectrum.refLevel : std::pow(10.0, s_.spectrum.refLevel / 20.0);
    for (int c = 0; c < sz.width(); ++c) {
        const double t = std::clamp((lv[c] - lo) / (hi - lo), 0.0, 1.0);
        row[c] = cmap_[static_cast<int>(t * 255.0)];
    }
}

void SpectrumView::drawWaterfall(QPainter& p, const QRectF& r) {
    if (r.height() < 8) return;
    p.fillRect(r, QColor(0, 0, 4));
    if (!waterfall_.isNull()) p.drawImage(r, waterfall_);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    p.setFont(monoFont(8.5));
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    p.save();
    p.translate(10, r.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-60, -10, 120, 14), Qt::AlignCenter, tr("time \xE2\x86\x93"));
    p.restore();
    // colour bar
    const QRectF cb(kL - 16, r.top(), 6, r.height());
    for (int i = 0; i < static_cast<int>(cb.height()); ++i) {
        const int idx = std::clamp(static_cast<int>(255.0 * (1.0 - i / cb.height())), 0, 255);
        p.setPen(QColor::fromRgb(cmap_[idx]));
        p.drawLine(QPointF(cb.left(), cb.top() + i), QPointF(cb.right(), cb.top() + i));
    }
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    p.drawText(QRectF(2, r.top(), kL - 20, 12), Qt::AlignRight | Qt::AlignTop,
               s_.spectrum.logScale ? QString::number(s_.spectrum.refLevel, 'f', 0) : QStringLiteral("max"));
    p.drawText(QRectF(2, r.bottom() - 12, kL - 20, 12), Qt::AlignRight | Qt::AlignBottom,
               s_.spectrum.logScale ? QString::number(s_.spectrum.refLevel - 10 * s_.spectrum.dbPerDiv, 'f', 0) : QStringLiteral("0"));
}

// ------------------------------------------------------------------ interaction
void SpectrumView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    pressPos_ = e->position();
    pressStart_ = fStart();
    pressStop_ = fStop();
    pressRef_ = s_.spectrum.refLevel;
    setCursor(Qt::ClosedHandCursor);
}

void SpectrumView::mouseMoveEvent(QMouseEvent* e) {
    hoverPos_ = e->position();
    hover_ = true;
    if (!dragging_) { update(); return; }
    const QRectF r = specRect();
    const double ny = nyquist();
    if (e->modifiers() & Qt::ShiftModifier) {
        const double dy = (e->position().y() - pressPos_.y()) / r.height();
        s_.spectrum.refLevel = std::round((pressRef_ + dy * 10.0 * s_.spectrum.dbPerDiv) * 10.0) / 10.0;
    } else {
        const double span = pressStop_ - pressStart_;
        double df = -(e->position().x() - pressPos_.x()) / r.width() * span;
        df = std::clamp(df, -pressStart_, ny - pressStop_);
        s_.spectrum.fStart = pressStart_ + df;
        s_.spectrum.fStop = pressStop_ + df >= ny * 0.999999 ? 0.0 : pressStop_ + df;
    }
    update();
    emit settingsEdited(s_);
}

void SpectrumView::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = false;
    setCursor(Qt::ArrowCursor);
}

void SpectrumView::mouseDoubleClickEvent(QMouseEvent*) {
    s_.spectrum.fStart = 0;
    s_.spectrum.fStop = 0;
    update();
    emit settingsEdited(s_);
}

void SpectrumView::wheelEvent(QWheelEvent* e) {
    const QPoint d = e->angleDelta();
    wheelAccum_ += d.y() != 0 ? d.y() : d.x();
    const int steps = wheelAccum_ / 120;
    wheelAccum_ -= steps * 120;
    if (steps == 0) return;
    const QRectF r = specRect();
    const double ny = nyquist(), f0 = fStart(), f1 = fStop();
    const double fm = f0 + std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0) * (f1 - f0);
    const double factor = std::pow(0.8, steps);
    const double minSpan = d_ && d_->binHz > 0 ? 8 * d_->binHz : 1.0;
    double span = std::clamp((f1 - f0) * factor, minSpan, ny);
    double a = fm - (fm - f0) * span / (f1 - f0);
    a = std::clamp(a, 0.0, ny - span);
    s_.spectrum.fStart = a;
    s_.spectrum.fStop = a + span >= ny * 0.999999 ? 0.0 : a + span;
    e->accept();
    update();
    emit settingsEdited(s_);
}

void SpectrumView::leaveEvent(QEvent*) {
    hover_ = false;
    update();
}

} // namespace slp
