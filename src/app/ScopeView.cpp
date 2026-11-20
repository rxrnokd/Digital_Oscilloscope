#include "ScopeView.h"

#include "Controls.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slp {

using ui::fmt;
using ui::monoFont;
using ui::toQColor;

namespace {
constexpr double kLeft = 34, kRight = 34, kTop = 24, kBottom = 56;

double lanczos(double x, int a) {
    if (x == 0.0) return 1.0;
    if (std::fabs(x) >= a) return 0.0;
    const double px = std::numbers::pi * x;
    return a * std::sin(px) * std::sin(px / a) / (px * px);
}

void pentagon(QPainter& p, const QRectF& r, bool pointRight, const QColor& fill, const QString& label, const QColor& textColor) {
    QPainterPath path;
    const double tip = r.height() * 0.5;
    if (pointRight) {
        path.moveTo(r.left(), r.top());
        path.lineTo(r.right() - tip, r.top());
        path.lineTo(r.right(), r.center().y());
        path.lineTo(r.right() - tip, r.bottom());
        path.lineTo(r.left(), r.bottom());
    } else {
        path.moveTo(r.right(), r.top());
        path.lineTo(r.left() + tip, r.top());
        path.lineTo(r.left(), r.center().y());
        path.lineTo(r.left() + tip, r.bottom());
        path.lineTo(r.right(), r.bottom());
    }
    path.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(path);
    p.setPen(textColor);
    QRectF tr = r;
    if (pointRight) tr.setRight(r.right() - tip * 0.6);
    else tr.setLeft(r.left() + tip * 0.6);
    p.drawText(tr, Qt::AlignCenter, label);
}
} // namespace

ScopeView::ScopeView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::ClickFocus);
    s_ = ScopeSettings::defaults();
}

void ScopeView::setSettings(const ScopeSettings& s) {
    s_ = s;
    update();
}

void ScopeView::setFrame(std::shared_ptr<const Frame> f) {
    frame_ = std::move(f);
    update();
}

void ScopeView::setStatus(const EngineStatus& st) {
    const bool changed = st.state != st_.state || st.fs != st_.fs || st.message != st_.message;
    st_ = st;
    if (changed) update();
}

// ------------------------------------------------------------------ geometry
QRectF ScopeView::plotRect() const {
    return QRectF(kLeft, kTop, std::max(10.0, width() - kLeft - kRight), std::max(10.0, height() - kTop - kBottom));
}
double ScopeView::windowWidth() const { return 10.0 * s_.timebase.secPerDiv; }
double ScopeView::windowStart() const {
    return -std::clamp(s_.timebase.triggerPos, 0.0, 100.0) / 100.0 * windowWidth() + s_.timebase.delay;
}
double ScopeView::xOf(const QRectF& pr, double t) const { return pr.left() + (t - windowStart()) / windowWidth() * pr.width(); }
double ScopeView::tOf(const QRectF& pr, double x) const { return windowStart() + (x - pr.left()) / pr.width() * windowWidth(); }
double ScopeView::yOf(const QRectF& pr, double v, int i) const {
    return pr.center().y() - (v / traceScale(i) + tracePos(i)) * pr.height() / 8.0;
}
double ScopeView::vOf(const QRectF& pr, double y, int i) const {
    return ((pr.center().y() - y) / (pr.height() / 8.0) - tracePos(i)) * traceScale(i);
}
double ScopeView::traceScale(int i) const { return i < kNumChannels ? s_.ch[i].voltsPerDiv : s_.math.unitsPerDiv; }
double ScopeView::tracePos(int i) const { return i < kNumChannels ? s_.ch[i].position : s_.math.position; }
QColor ScopeView::traceColor(int i) const { return toQColor(i < kNumChannels ? s_.ch[i].color : s_.math.color); }
bool ScopeView::traceEnabled(int i) const { return i < kNumChannels ? s_.ch[i].enabled : s_.math.enabled; }
QString ScopeView::traceUnit(int i) const {
    if (frame_ && frame_->tr[i].present) return ui::qs(frame_->tr[i].unit);
    if (i < kNumChannels) {
        if (s_.acq.source == SourceKind::Audio) return "FS";
        return "V";
    }
    return "V";
}

double ScopeView::traceValueAt(int i, double t) const {
    if (!frame_ || !frame_->tr[i].present) return kNaN;
    const auto& tr = frame_->tr[i];
    const double k = (t - tr.t0) / tr.dt;
    const auto j = static_cast<long>(std::floor(k));
    if (j < 0 || j + 1 >= static_cast<long>(tr.y.size())) return kNaN;
    const double a = k - static_cast<double>(j);
    return tr.y[j] * (1 - a) + tr.y[j + 1] * a;
}

// ------------------------------------------------------------------ painting
void ScopeView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF pr = plotRect();

    // background + graticule are static: render once per size / intensity into a pixmap
    const double dpr = devicePixelRatioF();
    const QString bgKey = QStringLiteral("%1x%2@%3/%4").arg(width()).arg(height()).arg(dpr).arg(s_.display.gridIntensity);
    if (bgKey != backgroundKey_) {
        background_ = QPixmap((QSizeF(size()) * dpr).toSize());
        background_.setDevicePixelRatio(dpr);
        QPainter bp(&background_);
        bp.setRenderHint(QPainter::Antialiasing);
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0, QColor(0x0b, 0x0d, 0x11));
        bg.setColorAt(1, QColor(0x07, 0x08, 0x0b));
        bp.fillRect(rect(), bg);
        QRadialGradient screen(pr.center(), std::max(pr.width(), pr.height()) * 0.75);
        screen.setColorAt(0, QColor(0x0d, 0x11, 0x16));
        screen.setColorAt(1, QColor(0x05, 0x06, 0x08));
        bp.fillRect(pr, screen);
        drawGrid(bp, pr);
        backgroundKey_ = bgKey;
    }
    p.drawPixmap(0, 0, background_);
    if (frame_) {
        if (s_.display.persistence != Persistence::Off) {
            drawPersistence(p, pr);
            drawTraces(p, pr, 1.0, false);
        } else {
            drawTraces(p, pr, 1.0, s_.display.glow);
        }
    }
    drawCursors(p, pr);
    drawMarkers(p, pr);
    drawAxisAndBadges(p, pr);
    drawOverlay(p, pr);
}

void ScopeView::drawGrid(QPainter& p, const QRectF& pr) {
    const double k = std::clamp(s_.display.gridIntensity, 0.0, 1.0);
    const double dx = pr.width() / 10.0, dy = pr.height() / 8.0;
    QPen dotted(QColor(255, 255, 255, static_cast<int>(48 * k)), 1.0);
    dotted.setDashPattern({1.0, 3.0});
    p.setPen(dotted);
    for (int i = 1; i < 10; ++i) {
        if (i == 5) continue;
        p.drawLine(QPointF(pr.left() + i * dx, pr.top()), QPointF(pr.left() + i * dx, pr.bottom()));
    }
    for (int i = 1; i < 8; ++i) {
        if (i == 4) continue;
        p.drawLine(QPointF(pr.left(), pr.top() + i * dy), QPointF(pr.right(), pr.top() + i * dy));
    }
    // centre axes with minor ticks
    const QColor axis(255, 255, 255, static_cast<int>(70 * k));
    p.setPen(QPen(axis, 1.0));
    const double cx = pr.center().x(), cy = pr.center().y();
    p.drawLine(QPointF(cx, pr.top()), QPointF(cx, pr.bottom()));
    p.drawLine(QPointF(pr.left(), cy), QPointF(pr.right(), cy));
    for (int i = 0; i <= 50; ++i) {
        const double x = pr.left() + i * dx / 5.0;
        const double len = (i % 5 == 0) ? 6 : 3.5;
        p.drawLine(QPointF(x, cy - len), QPointF(x, cy + len));
        p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.top() + len * 0.6));
        p.drawLine(QPointF(x, pr.bottom()), QPointF(x, pr.bottom() - len * 0.6));
    }
    for (int i = 0; i <= 40; ++i) {
        const double y = pr.top() + i * dy / 5.0;
        const double len = (i % 5 == 0) ? 6 : 3.5;
        p.drawLine(QPointF(cx - len, y), QPointF(cx + len, y));
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.left() + len * 0.6, y));
        p.drawLine(QPointF(pr.right(), y), QPointF(pr.right() - len * 0.6, y));
    }
    p.setPen(QPen(QColor(255, 255, 255, static_cast<int>(90 * k)), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(pr);
}

void ScopeView::drawTraces(QPainter& p, const QRectF& pr, double alpha, bool glow) {
    const double dpr = devicePixelRatioF();
    const QSize sz = (pr.size() * dpr).toSize();
    if (layer_.size() != sz) {
        layer_ = QImage(sz, QImage::Format_ARGB32_Premultiplied);
        layer_.setDevicePixelRatio(dpr);
    }
    layer_.fill(Qt::transparent);
    bool usedLayer = false;
    // draw selected trace last so it stays on top
    const int sel = std::clamp(s_.display.selectedTrace, 0, kNumTraces - 1);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < kNumTraces; ++i) {
            if ((pass == 0) == (i == sel)) continue;
            if (!traceEnabled(i) || !frame_->tr[i].present) continue;
            usedLayer |= drawTrace(p, pr, frame_->tr[i], i, alpha, glow);
        }
    if (usedLayer) p.drawImage(pr.topLeft(), layer_);
}

bool ScopeView::drawTrace(QPainter& p, const QRectF& pr, const TraceData& tr, int idx, double alpha, bool glow) {
    const auto& y = tr.y;
    const long n = static_cast<long>(y.size());
    if (n < 1 || !(tr.dt > 0)) return false;
    const double W = windowWidth(), t0w = windowStart();
    const long k0 = std::max(0L, static_cast<long>(std::floor((t0w - tr.t0) / tr.dt)) - 1);
    const long k1 = std::min(n - 1, static_cast<long>(std::ceil((t0w + W - tr.t0) / tr.dt)) + 1);
    if (k1 < k0) return false;
    const double dpr = devicePixelRatioF();
    const double pxPerSample = tr.dt / W * pr.width();
    const double scale = traceScale(idx), pos = tracePos(idx), unitPx = pr.height() / 8.0, cy = pr.center().y();
    auto Y = [&](double v) { return std::clamp(cy - (v / scale + pos) * unitPx, pr.top() - 2000.0, pr.bottom() + 2000.0); };
    auto X = [&](double t) { return pr.left() + (t - t0w) / W * pr.width(); };

    std::vector<QPolygonF> segs;
    QPolygonF cur;
    auto flush = [&] {
        if (!cur.isEmpty()) segs.push_back(cur);
        cur.clear();
    };
    const double wdt = idx < kNumChannels ? s_.ch[idx].lineWidth : 1.6;
    bool dots = false;
    // "Busy" sparse data (e.g. heavy noise): stroking a long zig-zag with a thick
    // antialiased pen is very expensive, so it is drawn as an envelope as well.
    bool busy = false;
    if (pxPerSample * dpr >= 1.0 && k1 - k0 > 256) {
        double travel = 0;
        for (long k = k0 + 1; k <= k1; ++k)
            if (std::isfinite(y[k]) && std::isfinite(y[k - 1])) travel += std::fabs(static_cast<double>(y[k]) - y[k - 1]);
        busy = travel / scale * unitPx > 25.0 * pr.height();
    }
    if (pxPerSample * dpr < 1.0 || busy) {
        // Peak-detect envelope: for every device-pixel column the min/max of the
        // piecewise-linear waveform (samples inside the column + values interpolated at
        // the column borders, which keeps steep edges connected), padded to the line
        // width. Each column is rasterized as a vertical span straight into the layer
        // image with fractional (anti-aliased) end pixels - the way a DSO draws - so the
        // cost no longer depends on how noisy the signal is.
        const int cols = layer_.width(), rows = layer_.height();
        const double colT = W / std::max(1, cols);
        QColor cc = traceColor(idx);
        const double a = std::clamp(alpha, 0.0, 1.0);
        const uint32_t sa = static_cast<uint32_t>(std::lround(255 * a));
        const uint32_t sr = cc.red() * sa / 255, sg = cc.green() * sa / 255, sb = cc.blue() * sa / 255;
        auto blend = [&](QRgb* px, double cov) {
            const uint32_t k = static_cast<uint32_t>(cov * 256.0);
            const uint32_t a2 = sa * k >> 8, r2 = sr * k >> 8, g2 = sg * k >> 8, b2 = sb * k >> 8;
            const uint32_t inv = 255 - a2;
            const QRgb d = *px;
            *px = qRgba(static_cast<int>(r2 + qRed(d) * inv / 255), static_cast<int>(g2 + qGreen(d) * inv / 255),
                        static_cast<int>(b2 + qBlue(d) * inv / 255), static_cast<int>(a2 + qAlpha(d) * inv / 255));
        };
        auto interp = [&](double t, float& out) {
            const double kf = (t - tr.t0) / tr.dt;
            const auto i = static_cast<long>(std::floor(kf));
            if (i < 0 || i + 1 >= n) return false;
            const float va = y[i], vb = y[i + 1];
            if (!std::isfinite(va) || !std::isfinite(vb)) return false;
            out = static_cast<float>(va + (vb - va) * (kf - static_cast<double>(i)));
            return true;
        };
        const double minH = wdt * dpr;
        uchar* bits = layer_.bits();
        const qsizetype bpl = layer_.bytesPerLine();
        for (int c = 0; c < cols; ++c) {
            const double ta = t0w + c * colT, tb = ta + colT;
            float mn = std::numeric_limits<float>::infinity(), mx = -mn;
            const long ks = std::max(0L, static_cast<long>(std::ceil((ta - tr.t0) / tr.dt)));
            const long ke = std::min(n - 1, static_cast<long>(std::floor((tb - tr.t0) / tr.dt)));
            for (long k = ks; k <= ke; ++k) {
                const float v = y[k];
                if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            }
            float v;
            if (interp(ta, v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            if (interp(tb, v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            if (!(mx >= mn)) continue;
            double yt = (Y(mx) - pr.top()) * dpr, yb = (Y(mn) - pr.top()) * dpr;
            if (yb - yt < minH) {
                const double mid = 0.5 * (yt + yb);
                yt = mid - 0.5 * minH;
                yb = mid + 0.5 * minH;
            }
            yt = std::max(yt, 0.0);
            yb = std::min(yb, static_cast<double>(rows));
            if (yb <= yt) continue;
            const int y0 = static_cast<int>(std::floor(yt)), y1 = static_cast<int>(std::ceil(yb));
            for (int yy = y0; yy < y1; ++yy) {
                const double cov = std::min(yb, yy + 1.0) - std::max(yt, static_cast<double>(yy));
                if (cov > 0) blend(reinterpret_cast<QRgb*>(bits + yy * bpl) + c, std::min(1.0, cov));
            }
        }
        return true;
    } else if (s_.display.interpolation == Interpolation::SinX && pxPerSample > 3.0) {
        // band-limited reconstruction (sin(x)/x with a Lanczos window, a = 6)
        const double xs = std::max(pr.left(), X(tr.t0 + k0 * tr.dt));
        const double xe = std::min(pr.right(), X(tr.t0 + k1 * tr.dt));
        for (double x = xs; x <= xe; x += 1.0) {
            const double kf = (tOf(pr, x) - tr.t0) / tr.dt;
            const auto i0 = static_cast<long>(std::floor(kf));
            double acc = 0, ws = 0;
            bool bad = false;
            for (long j = i0 - 5; j <= i0 + 6; ++j) {
                if (j < 0 || j >= n) continue;
                const float v = y[j];
                if (!std::isfinite(v)) { bad = true; break; }
                const double w = lanczos(kf - j, 6);
                acc += v * w;
                ws += w;
            }
            if (bad || ws == 0) { flush(); continue; }
            cur << QPointF(x, Y(acc / ws));
        }
        flush();
        dots = pxPerSample > 8.0;
    } else {
        cur.reserve(k1 - k0 + 1);
        for (long k = k0; k <= k1; ++k) {
            const float v = y[k];
            if (std::isfinite(v)) cur << QPointF(X(tr.t0 + k * tr.dt), Y(v));
            else flush();
        }
        flush();
        dots = pxPerSample > 8.0;
    }

    QColor c = traceColor(idx);
    p.save();
    p.setClipRect(pr.adjusted(1, 1, -1, -1));
    p.setBrush(Qt::NoBrush);
    if (glow) {
        QColor g = c;
        g.setAlpha(static_cast<int>(50 * alpha));
        p.setPen(QPen(g, wdt + 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto& s : segs) p.drawPolyline(s);
    }
    c.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    p.setPen(QPen(c, wdt, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const auto& s : segs) {
        if (s.size() == 1) p.drawPoint(s.front());
        else p.drawPolyline(s);
    }
    if (dots) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.lighter(140));
        for (long k = k0; k <= k1; ++k)
            if (std::isfinite(y[k])) p.drawEllipse(QPointF(X(tr.t0 + k * tr.dt), Y(y[k])), 2.2, 2.2);
    }
    p.restore();
    return false;
}

void ScopeView::drawPersistence(QPainter& p, const QRectF& pr) {
    const double dpr = devicePixelRatioF();
    const QSize sz = (pr.size() * dpr).toSize();
    QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(s_.timebase.secPerDiv).arg(s_.timebase.delay)
                      .arg(s_.timebase.triggerPos).arg(static_cast<int>(s_.display.persistence)).arg(sz.width());
    for (int i = 0; i < kNumTraces; ++i) key += QStringLiteral("|%1/%2/%3").arg(traceEnabled(i)).arg(traceScale(i)).arg(tracePos(i));
    if (persist_.size() != sz || key != persistKey_) {
        persist_ = QImage(sz, QImage::Format_ARGB32_Premultiplied);
        persist_.setDevicePixelRatio(dpr);
        persist_.fill(Qt::transparent);
        persistKey_ = key;
        persistSeq_ = 0;
    }
    if (frame_->seq != persistSeq_) {
        QPainter ip(&persist_);
        ip.setRenderHint(QPainter::Antialiasing);
        if (s_.display.persistence != Persistence::Infinite) {
            const int keep = s_.display.persistence == Persistence::Short ? 200 : 240;
            ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            ip.fillRect(QRectF(0, 0, pr.width(), pr.height()), QColor(0, 0, 0, keep));
        }
        ip.setCompositionMode(QPainter::CompositionMode_SourceOver);
        ip.translate(-pr.topLeft());
        drawTraces(ip, pr, 0.35, false);
        persistSeq_ = frame_->seq;
    }
    p.drawImage(pr.topLeft(), persist_);
}

void ScopeView::drawMarkers(QPainter& p, const QRectF& pr) {
    QFont f = monoFont(8.5, true);
    p.setFont(f);
    // channel ground (0-level) markers on the left
    for (int i = 0; i < kNumTraces; ++i) {
        if (!traceEnabled(i)) continue;
        const QColor c = traceColor(i);
        const double y = yOf(pr, 0.0, i);
        const double yc = std::clamp(y, pr.top() + 7, pr.bottom() - 7);
        const QRectF r(4, yc - 7, kLeft - 8, 14);
        const bool sel = i == s_.display.selectedTrace;
        QColor fill = c;
        if (!sel) fill.setAlpha(200);
        pentagon(p, r, true, fill, i < kNumChannels ? QString::number(i + 1) : "M", QColor(0, 0, 0));
        if (y < pr.top() || y > pr.bottom()) { // off-screen indicator
            p.setPen(QPen(c, 1.5));
            const double ay = y < pr.top() ? r.top() - 3 : r.bottom() + 3;
            const double d = y < pr.top() ? -4 : 4;
            p.drawLine(QPointF(r.center().x() - 4, ay), QPointF(r.center().x(), ay + d));
            p.drawLine(QPointF(r.center().x() + 4, ay), QPointF(r.center().x(), ay + d));
        }
    }
    // trigger level marker on the right
    const int src = std::clamp(s_.trigger.source, 0, kNumChannels - 1);
    const QColor tc = traceColor(src);
    const double ly = yOf(pr, s_.trigger.level, src);
    const double lyc = std::clamp(ly, pr.top() + 7, pr.bottom() - 7);
    pentagon(p, QRectF(pr.right() + 4, lyc - 7, kRight - 8, 14), false, tc, "T", QColor(0, 0, 0));
    if (drag_ == Drag::Level || (hover_ && std::fabs(hoverPos_.y() - ly) < 9 && hoverPos_.x() > pr.right())) {
        QPen dash(tc, 1.0, Qt::DashLine);
        p.setPen(dash);
        p.drawLine(QPointF(pr.left(), ly), QPointF(pr.right(), ly));
        p.setFont(monoFont(9));
        p.drawText(QRectF(pr.right() - 220, ly - 18, 214, 16), Qt::AlignRight | Qt::AlignVCenter,
                   tr("Trigger level ") + fmt(s_.trigger.level, traceUnit(src).toUtf8().constData()));
    }
    // trigger position marker on top
    const double tx = xOf(pr, 0.0);
    p.setFont(monoFont(8.5, true));
    const QColor amber = ui::theme::amber;
    if (tx >= pr.left() && tx <= pr.right()) {
        QPainterPath path;
        path.moveTo(tx - 7, pr.top() - 18);
        path.lineTo(tx + 7, pr.top() - 18);
        path.lineTo(tx + 7, pr.top() - 8);
        path.lineTo(tx, pr.top() - 2);
        path.lineTo(tx - 7, pr.top() - 8);
        path.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(amber);
        p.drawPath(path);
        p.setPen(Qt::black);
        p.drawText(QRectF(tx - 7, pr.top() - 18, 14, 11), Qt::AlignCenter, "T");
    } else {
        const bool left = tx < pr.left();
        const double ax = left ? pr.left() + 4 : pr.right() - 4;
        QPainterPath path;
        path.moveTo(ax + (left ? 0 : 0), pr.top() - 11);
        path.lineTo(ax + (left ? 10 : -10), pr.top() - 18);
        path.lineTo(ax + (left ? 10 : -10), pr.top() - 4);
        path.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(amber);
        p.drawPath(path);
        p.setPen(amber);
        p.drawText(QRectF(left ? ax + 13 : ax - 113, pr.top() - 20, 100, 16), (left ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                   "T " + fmt(-windowStart() * (left ? 1 : 1), "s", 3));
    }
}

void ScopeView::drawCursors(QPainter& p, const QRectF& pr) {
    const auto mode = s_.display.cursorMode;
    if (mode == CursorMode::Off) return;
    const int src = std::clamp(s_.display.cursorSource, 0, kNumTraces - 1);
    const QColor cc(0xFF, 0xB0, 0x20);
    const QColor cy(0xE6, 0xED, 0xF3);
    p.save();
    p.setClipRect(pr);
    QPen pen(cc, 1.0, Qt::DashLine);
    const double x1 = xOf(pr, s_.display.cursorX1), x2 = xOf(pr, s_.display.cursorX2);
    p.setPen(pen);
    p.drawLine(QPointF(x1, pr.top()), QPointF(x1, pr.bottom()));
    p.drawLine(QPointF(x2, pr.top()), QPointF(x2, pr.bottom()));
    double y1v = s_.display.cursorY1, y2v = s_.display.cursorY2;
    if (mode == CursorMode::Track) {
        y1v = traceValueAt(src, s_.display.cursorX1);
        y2v = traceValueAt(src, s_.display.cursorX2);
    }
    QPen ypen(cy, 1.0, Qt::DashLine);
    p.setPen(ypen);
    if (std::isfinite(y1v)) p.drawLine(QPointF(pr.left(), yOf(pr, y1v, src)), QPointF(pr.right(), yOf(pr, y1v, src)));
    if (std::isfinite(y2v)) p.drawLine(QPointF(pr.left(), yOf(pr, y2v, src)), QPointF(pr.right(), yOf(pr, y2v, src)));
    if (mode == CursorMode::Track) {
        p.setPen(QPen(cy, 1.5));
        p.setBrush(Qt::NoBrush);
        if (std::isfinite(y1v)) p.drawEllipse(QPointF(x1, yOf(pr, y1v, src)), 4, 4);
        if (std::isfinite(y2v)) p.drawEllipse(QPointF(x2, yOf(pr, y2v, src)), 4, 4);
    }
    p.restore();
    // cursor tags
    p.setFont(monoFont(8.5, true));
    for (int k = 0; k < 2; ++k) {
        const double x = k == 0 ? x1 : x2;
        if (x < pr.left() || x > pr.right()) continue;
        const QRectF r(x - 11, pr.bottom() - 15, 22, 13);
        p.setPen(Qt::NoPen);
        p.setBrush(cc);
        p.drawRoundedRect(r, 3, 3);
        p.setPen(Qt::black);
        p.drawText(r, Qt::AlignCenter, k == 0 ? "X1" : "X2");
    }
    // readout box
    const QString u = traceUnit(src);
    const QByteArray ub = u.toUtf8();
    const double dx = s_.display.cursorX2 - s_.display.cursorX1;
    QStringList lines;
    lines << QStringLiteral("X1 %1   X2 %2").arg(fmt(s_.display.cursorX1, "s"), fmt(s_.display.cursorX2, "s"));
    lines << QString::fromUtf8("\xCE\x94X %1   1/\xCE\x94X %2").arg(fmt(dx, "s"), dx != 0 ? fmt(1.0 / std::fabs(dx), "Hz") : "N/A");
    lines << QStringLiteral("Y1 %1   Y2 %2").arg(fmt(y1v, ub.constData()), fmt(y2v, ub.constData()));
    lines << QString::fromUtf8("\xCE\x94Y %1   (%2)").arg(fmt(y2v - y1v, ub.constData()), src < kNumChannels ? QStringLiteral("CH%1").arg(src + 1) : "MATH");
    if (mode == CursorMode::Track && dx != 0 && std::isfinite(y1v) && std::isfinite(y2v))
        lines << QString::fromUtf8("\xCE\x94Y/\xCE\x94X %1").arg(fmt((y2v - y1v) / dx, (u + "/s").toUtf8().constData()));
    QFont f = monoFont(9);
    p.setFont(f);
    const QFontMetricsF fm(f);
    double w = 0;
    for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const QRectF box(pr.right() - w - 22, pr.top() + 8, w + 14, lines.size() * fm.height() + 10);
    p.setPen(QPen(QColor(0xFF, 0xB0, 0x20, 120), 1));
    p.setBrush(QColor(8, 10, 14, 215));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(QColor(0xE6, 0xED, 0xF3));
    for (int i = 0; i < lines.size(); ++i)
        p.drawText(QPointF(box.left() + 7, box.top() + 5 + fm.ascent() + i * fm.height()), lines[i]);
}

void ScopeView::drawAxisAndBadges(QPainter& p, const QRectF& pr) {
    // time axis labels
    QFont f = monoFont(8.5);
    p.setFont(f);
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    const double dx = pr.width() / 10.0;
    const int every = dx < 62 ? 2 : 1;
    for (int i = 0; i <= 10; i += every) {
        const double t = windowStart() + i * windowWidth() / 10.0;
        const double x = pr.left() + i * dx;
        Qt::Alignment al = Qt::AlignHCenter | Qt::AlignTop;
        QRectF r(x - 40, pr.bottom() + 3, 80, 14);
        if (i == 0) { r.moveLeft(x - 4); al = Qt::AlignLeft | Qt::AlignTop; }
        if (i == 10) { r.moveRight(x + 4); al = Qt::AlignRight | Qt::AlignTop; }
        p.drawText(r, al, std::fabs(t) < 1e-15 * windowWidth() ? QStringLiteral("0") : fmt(t, "s", 3));
    }

    // badges
    badgeRects_.clear();
    const double by = pr.bottom() + 22, bh = 24;
    double x = pr.left();
    QFont bf = monoFont(9, true);
    p.setFont(bf);
    const QFontMetricsF fm(bf);
    for (int i = 0; i < kNumTraces; ++i) {
        if (!traceEnabled(i)) continue;
        const QColor c = traceColor(i);
        const QByteArray u = traceUnit(i).toUtf8();
        QString txt = fmt(traceScale(i), u.constData(), 3) + "/";
        if (i < kNumChannels) {
            const auto& ch = s_.ch[i];
            txt += ch.coupling == Coupling::AC ? "  AC" : ch.coupling == Coupling::GND ? "  GND" : "  DC";
            if (ch.invert) txt += "  INV";
        } else {
            txt += "  " + ui::qs(mathOpLabel(s_.math));
        }
        const double w = fm.horizontalAdvance(txt) + 38;
        if (x + w > pr.right() - 250) break;
        const QRectF r(x, by, w, bh);
        const bool sel = s_.display.selectedTrace == i;
        p.setPen(QPen(c, sel ? 1.6 : 1.0));
        QColor fill = c;
        fill.setAlpha(sel ? 55 : 18);
        p.setBrush(fill);
        p.drawRoundedRect(r, 5, 5);
        const QRectF num(r.left() + 4, r.top() + 4, 18, bh - 8);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(num, 3, 3);
        p.setPen(Qt::black);
        p.drawText(num, Qt::AlignCenter, i < kNumChannels ? QString::number(i + 1) : "M");
        p.setPen(c.lighter(110));
        p.drawText(QRectF(num.right() + 7, r.top(), w, bh), Qt::AlignVCenter | Qt::AlignLeft, txt);
        badgeRects_.push_back({r, i});
        x += w + 8;
    }
    // timebase + trigger badges (right aligned)
    const int src = std::clamp(s_.trigger.source, 0, kNumChannels - 1);
    const QString trig = QStringLiteral("T %1 CH%2 %3")
                             .arg(s_.trigger.slope == TriggerSlope::Rising ? QString::fromUtf8("\xE2\x86\x91") : QString::fromUtf8("\xE2\x86\x93"))
                             .arg(src + 1)
                             .arg(fmt(s_.trigger.level, traceUnit(src).toUtf8().constData(), 3));
    const QString hb = QStringLiteral("H %1/").arg(fmt(s_.timebase.secPerDiv, "s", 3));
    const double wt = fm.horizontalAdvance(trig) + 18, wh = fm.horizontalAdvance(hb) + 18;
    QRectF rt(pr.right() - wt, by, wt, bh), rh(pr.right() - wt - wh - 8, by, wh, bh);
    p.setPen(QPen(QColor(0x3a, 0x44, 0x52), 1));
    p.setBrush(QColor(0x14, 0x18, 0x20));
    p.drawRoundedRect(rh, 5, 5);
    p.setPen(QPen(traceColor(src), 1));
    p.drawRoundedRect(rt, 5, 5);
    p.setPen(QColor(0xE6, 0xED, 0xF3));
    p.drawText(rh, Qt::AlignCenter, hb);
    p.setPen(traceColor(src));
    p.drawText(rt, Qt::AlignCenter, trig);
}

void ScopeView::drawOverlay(QPainter& p, const QRectF& pr) {
    // acquisition status line in the top margin
    QFont f = monoFont(8.5, true);
    p.setFont(f);
    QString state = trigStateLabel(st_.state);
    QColor sc = ui::theme::green;
    switch (st_.state) {
    case TrigState::Stopped: sc = ui::theme::red; break;
    case TrigState::Auto: sc = QColor(0xFF, 0xD6, 0x0A); break;
    case TrigState::Waiting:
    case TrigState::Armed: sc = ui::theme::amber; break;
    case TrigState::Roll: sc = QColor(0x22, 0xD3, 0xEE); break;
    default: break;
    }
    const QRectF pill(pr.left(), 4, 62, 15);
    p.setPen(Qt::NoPen);
    QColor pf = sc;
    pf.setAlpha(45);
    p.setBrush(pf);
    p.drawRoundedRect(pill, 4, 4);
    p.setPen(sc);
    p.drawText(pill, Qt::AlignCenter, state);
    p.setPen(QColor(0x8b, 0x94, 0x9e));
    p.setFont(monoFont(8.5));
    QString info;
    if (frame_) {
        size_t pts = 0;
        for (const auto& t : frame_->tr)
            if (t.present) { pts = t.y.size(); break; }
        info = QStringLiteral("%1   %2 pts").arg(fmt(frame_->fs, "Sa/s", 3)).arg(pts);
        if (frame_->decim > 1) info += QString::fromUtf8("   decim \xC3\x97%1").arg(frame_->decim);
    }
    if (st_.segmented) info += tr("   segmented");
    p.drawText(QRectF(pill.right() + 8, 3, 360, 17), Qt::AlignVCenter | Qt::AlignLeft, info);
    if (s_.timebase.delay != 0.0)
        p.drawText(QRectF(pr.right() - 260, 3, 260, 17), Qt::AlignVCenter | Qt::AlignRight,
                   tr("Delay ") + fmt(s_.timebase.delay, "s"));

    // hover readout
    if (hover_ && pr.contains(hoverPos_) && drag_ == Drag::None) {
        const int sel = std::clamp(s_.display.selectedTrace, 0, kNumTraces - 1);
        const QByteArray u = traceUnit(sel).toUtf8();
        const QString txt = QStringLiteral("t %1   %2 %3")
                                .arg(fmt(tOf(pr, hoverPos_.x()), "s"))
                                .arg(sel < kNumChannels ? QStringLiteral("CH%1").arg(sel + 1) : QStringLiteral("M"))
                                .arg(fmt(vOf(pr, hoverPos_.y(), sel), u.constData()));
        p.setFont(monoFont(9));
        const QFontMetricsF fm(p.font());
        const QRectF r(pr.left() + 8, pr.bottom() - 24, fm.horizontalAdvance(txt) + 14, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 10, 14, 200));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(QColor(0xC9, 0xD1, 0xD9));
        p.drawText(r, Qt::AlignCenter, txt);
    }
    // rubber band
    if (drag_ == Drag::Zoom) {
        const QRectF r = QRectF(pressPos_, curPos_).normalized().intersected(pr);
        p.setPen(QPen(ui::theme::accent, 1, Qt::DashLine));
        QColor fill = ui::theme::accent;
        fill.setAlpha(30);
        p.setBrush(fill);
        p.drawRect(r);
    }
    if (!frame_ || !st_.message.empty()) {
        p.setFont(monoFont(11, true));
        p.setPen(QColor(0x8b, 0x94, 0x9e));
        p.drawText(pr, Qt::AlignCenter, !st_.message.empty() ? ui::qs(st_.message) : tr("Acquiring..."));
    }
}

// ------------------------------------------------------------------ interaction
void ScopeView::emitEdited() {
    update();
    emit settingsEdited(s_);
}

void ScopeView::mousePressEvent(QMouseEvent* e) {
    const QRectF pr = plotRect();
    const QPointF pos = e->position();
    pressPos_ = curPos_ = pos;
    drag_ = Drag::None;
    if (e->button() == Qt::RightButton && pr.contains(pos)) {
        drag_ = Drag::Zoom;
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    for (const auto& [r, idx] : badgeRects_)
        if (r.contains(pos)) {
            s_.display.selectedTrace = idx;
            emitEdited();
            return;
        }
    if (s_.display.cursorMode != CursorMode::Off && pr.contains(pos)) {
        if (std::fabs(pos.x() - xOf(pr, s_.display.cursorX1)) < 6) { drag_ = Drag::CursorX1; return; }
        if (std::fabs(pos.x() - xOf(pr, s_.display.cursorX2)) < 6) { drag_ = Drag::CursorX2; return; }
        if (s_.display.cursorMode == CursorMode::Manual) {
            const int src = std::clamp(s_.display.cursorSource, 0, kNumTraces - 1);
            if (std::fabs(pos.y() - yOf(pr, s_.display.cursorY1, src)) < 6) { drag_ = Drag::CursorY1; return; }
            if (std::fabs(pos.y() - yOf(pr, s_.display.cursorY2, src)) < 6) { drag_ = Drag::CursorY2; return; }
        }
    }
    if (pos.x() < pr.left()) {
        double best = 10;
        for (int i = 0; i < kNumTraces; ++i) {
            if (!traceEnabled(i)) continue;
            const double y = std::clamp(yOf(pr, 0.0, i), pr.top() + 7, pr.bottom() - 7);
            if (std::fabs(y - pos.y()) < best) { best = std::fabs(y - pos.y()); dragTrace_ = i; drag_ = Drag::Position; }
        }
        if (drag_ == Drag::Position) { s_.display.selectedTrace = dragTrace_; emitEdited(); }
        return;
    }
    if (pos.x() > pr.right()) {
        const int src = std::clamp(s_.trigger.source, 0, kNumChannels - 1);
        const double y = std::clamp(yOf(pr, s_.trigger.level, src), pr.top() + 7, pr.bottom() - 7);
        if (std::fabs(y - pos.y()) < 12) drag_ = Drag::Level;
        return;
    }
    if (pos.y() < pr.top()) {
        drag_ = Drag::TrigPos;
        return;
    }
    if (pr.contains(pos)) {
        drag_ = Drag::Pan;
        pressValue_ = s_.timebase.delay;
        setCursor(Qt::ClosedHandCursor);
    }
}

void ScopeView::mouseMoveEvent(QMouseEvent* e) {
    const QRectF pr = plotRect();
    const QPointF pos = e->position();
    curPos_ = pos;
    hoverPos_ = pos;
    hover_ = true;
    switch (drag_) {
    case Drag::None: {
        Qt::CursorShape cs = Qt::CrossCursor;
        if (pos.x() < pr.left() || pos.x() > pr.right()) cs = Qt::SizeVerCursor;
        else if (pos.y() < pr.top()) cs = Qt::SizeHorCursor;
        else if (s_.display.cursorMode != CursorMode::Off &&
                 (std::fabs(pos.x() - xOf(pr, s_.display.cursorX1)) < 6 || std::fabs(pos.x() - xOf(pr, s_.display.cursorX2)) < 6))
            cs = Qt::SizeHorCursor;
        setCursor(cs);
        update();
        return;
    }
    case Drag::Pan:
        s_.timebase.delay = pressValue_ - (pos.x() - pressPos_.x()) / pr.width() * windowWidth();
        break;
    case Drag::CursorX1: s_.display.cursorX1 = tOf(pr, pos.x()); break;
    case Drag::CursorX2: s_.display.cursorX2 = tOf(pr, pos.x()); break;
    case Drag::CursorY1: s_.display.cursorY1 = vOf(pr, pos.y(), std::clamp(s_.display.cursorSource, 0, kNumTraces - 1)); break;
    case Drag::CursorY2: s_.display.cursorY2 = vOf(pr, pos.y(), std::clamp(s_.display.cursorSource, 0, kNumTraces - 1)); break;
    case Drag::Level: s_.trigger.level = vOf(pr, pos.y(), std::clamp(s_.trigger.source, 0, kNumChannels - 1)); break;
    case Drag::Position: {
        const double div = std::round((pr.center().y() - pos.y()) / (pr.height() / 8.0) * 50.0) / 50.0;
        if (dragTrace_ < kNumChannels) s_.ch[dragTrace_].position = div;
        else s_.math.position = div;
        break;
    }
    case Drag::TrigPos:
        s_.timebase.triggerPos = std::clamp(std::round((pos.x() - pr.left()) / pr.width() * 1000.0) / 10.0, 0.0, 100.0);
        break;
    case Drag::Zoom: update(); return;
    }
    emitEdited();
}

void ScopeView::mouseReleaseEvent(QMouseEvent*) {
    const QRectF pr = plotRect();
    if (drag_ == Drag::Zoom) {
        const QRectF r = QRectF(pressPos_, curPos_).normalized().intersected(pr);
        if (r.width() > 8) {
            const double t0 = tOf(pr, r.left()), t1 = tOf(pr, r.right());
            const double w = std::clamp(round125((t1 - t0) / 10.0), 1e-9, 50.0) * 10.0;
            s_.timebase.secPerDiv = w / 10.0;
            const double tc = 0.5 * (t0 + t1);
            s_.timebase.delay = tc - w / 2.0 + s_.timebase.triggerPos / 100.0 * w;
        }
        if (r.height() > 8) {
            const int sel = std::clamp(s_.display.selectedTrace, 0, kNumTraces - 1);
            const double v0 = vOf(pr, r.bottom(), sel), v1 = vOf(pr, r.top(), sel);
            const double scale = std::max(round125((v1 - v0) / 8.0), 1e-12);
            const double vc = 0.5 * (v0 + v1);
            if (sel < kNumChannels) { s_.ch[sel].voltsPerDiv = scale; s_.ch[sel].position = -vc / scale; }
            else { s_.math.unitsPerDiv = scale; s_.math.position = -vc / scale; }
        }
        drag_ = Drag::None;
        emitEdited();
        return;
    }
    drag_ = Drag::None;
    setCursor(Qt::CrossCursor);
    update();
}

void ScopeView::wheelEvent(QWheelEvent* e) {
    const QPoint d = e->angleDelta();
    wheelAccum_ += d.y() != 0 ? d.y() : d.x();
    const int steps = wheelAccum_ / 120;
    wheelAccum_ -= steps * 120;
    if (steps == 0) return;
    const int sel = std::clamp(s_.display.selectedTrace, 0, kNumTraces - 1);
    if (e->modifiers() & Qt::ShiftModifier) {
        if (sel < kNumChannels) s_.ch[sel].voltsPerDiv = std::clamp(step125(s_.ch[sel].voltsPerDiv, -steps), 1e-3, 10.0);
        else s_.math.unitsPerDiv = std::max(step125(s_.math.unitsPerDiv, -steps), 1e-12);
    } else if (e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) {
        if (sel < kNumChannels) s_.ch[sel].position += 0.1 * steps;
        else s_.math.position += 0.1 * steps;
    } else {
        s_.timebase.secPerDiv = std::clamp(step125(s_.timebase.secPerDiv, -steps), 1e-8, 50.0);
    }
    e->accept();
    emitEdited();
}

void ScopeView::leaveEvent(QEvent*) {
    hover_ = false;
    update();
}

} // namespace slp
