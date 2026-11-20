#include "Controls.h"

#include "SignalGenerator.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <numbers>

namespace slp::ui {

QColor toQColor(uint32_t rgb) { return QColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }
uint32_t fromQColor(const QColor& c) {
    return (static_cast<uint32_t>(c.red()) << 16) | (static_cast<uint32_t>(c.green()) << 8) | static_cast<uint32_t>(c.blue());
}

QFont monoFont(double pt, bool bold) {
    QFont f;
    f.setFamilies({"SF Mono", "Menlo", "JetBrains Mono", "DejaVu Sans Mono", "Consolas", "monospace"});
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(pt);
    f.setBold(bold);
    return f;
}

QString qs(const std::string& s) { return QString::fromUtf8(s.c_str()); }
QString fmt(double v, const char* unit, int digits) { return qs(formatValue(v, unit, digits)); }

QString styleSheet() {
    return QStringLiteral(R"(
QWidget { background: #10131a; color: #c9d1d9; font-size: 12px; }
QMainWindow, QMainWindow > QWidget { background: #0a0c10; }
QToolTip { background: #1b2029; color: #e6edf3; border: 1px solid #2f3742; padding: 4px; }
QMenuBar { background: #0a0c10; border-bottom: 1px solid #1d222b; }
QMenuBar::item { padding: 4px 10px; background: transparent; }
QMenuBar::item:selected { background: #1b2230; }
QMenu { background: #141821; border: 1px solid #2a313c; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 14px; border-radius: 4px; }
QMenu::item:selected { background: #22416a; color: white; }
QMenu::separator { height: 1px; background: #262c36; margin: 4px 8px; }
QStatusBar { background: #0a0c10; color: #7d8590; border-top: 1px solid #1d222b; }
QSplitter::handle { background: #0a0c10; }
QSplitter::handle:hover { background: #22416a; }
QTabWidget::pane { border: 1px solid #222832; border-radius: 6px; top: -1px; background: #10131a; }
QTabBar { background: transparent; }
QTabBar::tab { background: #0e1117; color: #7d8590; padding: 6px 7px; border: 1px solid #1d222b;
               border-bottom: none; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 2px;
               font-weight: 600; letter-spacing: 0.5px; font-size: 10px; }
QTabBar::tab:selected { background: #10131a; color: #e6edf3; border-color: #2a313c; border-bottom: 2px solid #3fa9f5; }
QTabBar::tab:hover:!selected { color: #c9d1d9; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: #0e1117; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #2a313c; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3a4452; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar:horizontal { background: #0e1117; height: 10px; }
QScrollBar::handle:horizontal { background: #2a313c; border-radius: 5px; min-width: 30px; }
QPushButton { background: #1a1f28; border: 1px solid #2a313c; border-radius: 5px; padding: 5px 10px; color: #c9d1d9; }
QPushButton:hover { background: #212835; border-color: #3a4452; }
QPushButton:pressed { background: #151a22; }
QPushButton:checked { background: #1d3550; border-color: #3fa9f5; color: #ffffff; }
QPushButton:disabled { color: #4b525c; border-color: #1f242c; }
QPushButton#segBtn { border-radius: 0; padding: 5px 6px; font-weight: 600; font-size: 11px; }
QPushButton#segBtn[segPos="first"] { border-top-left-radius: 5px; border-bottom-left-radius: 5px; }
QPushButton#segBtn[segPos="last"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
QPushButton#segBtn[segPos="only"] { border-radius: 5px; }
QToolButton { background: #1a1f28; border: 1px solid #2a313c; border-radius: 5px; padding: 4px; }
QToolButton:hover { background: #212835; }
QToolButton:checked { background: #1d3550; border-color: #3fa9f5; }
QComboBox { background: #161a22; border: 1px solid #2a313c; border-radius: 5px; padding: 4px 8px; min-height: 20px; }
QComboBox:hover { border-color: #3a4452; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #141821; border: 1px solid #2a313c; selection-background-color: #22416a; }
QSpinBox, QDoubleSpinBox, QLineEdit { background: #0d1015; border: 1px solid #2a313c; border-radius: 5px; padding: 3px 6px;
               selection-background-color: #22416a; }
QSpinBox:focus, QDoubleSpinBox:focus, QLineEdit:focus { border-color: #3fa9f5; }
QCheckBox { spacing: 6px; background: transparent; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; border: 1px solid #3a4452; background: #0d1015; }
QCheckBox::indicator:checked { background: #3fa9f5; border-color: #3fa9f5; }
QLabel { background: transparent; }
QLabel#dim { color: #7d8590; font-size: 11px; }
QLabel#section { color: #8b949e; font-size: 10px; font-weight: 700; letter-spacing: 2px; }
QLabel#warn { color: #ffb020; font-size: 11px; }
QFrame#card { background: #141820; border: 1px solid #222832; border-radius: 8px; }
QFrame#card QLabel { background: transparent; }
QFrame#header { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #141922, stop:1 #0d1016); border-bottom: 1px solid #1f2530; }
QFrame#header QLabel { background: transparent; }
QSlider::groove:horizontal { height: 4px; background: #262c36; border-radius: 2px; }
QSlider::handle:horizontal { background: #c9d1d9; width: 12px; margin: -5px 0; border-radius: 6px; }
QSlider::sub-page:horizontal { background: #3fa9f5; border-radius: 2px; }
QTableWidget { background: #0b0e13; gridline-color: #1b2028; border: 1px solid #1f2530; border-radius: 6px;
               selection-background-color: #1b2a3d; }
QHeaderView::section { background: #121620; color: #8b949e; border: none; border-bottom: 1px solid #222832;
                       border-right: 1px solid #1b2028; padding: 4px 6px; font-weight: 600; font-size: 11px; }
QTableCornerButton::section { background: #121620; border: none; }
QDialog { background: #10131a; }
)");
}

QIcon waveformIcon(Waveform w, const QColor& color, int size) {
    const int dpr = 2;
    QPixmap pm(size * dpr, size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const double m = 3.0, wdt = size - 2 * m, h = size - 2 * m, cy = size / 2.0;
    QPainterPath path;
    const int N = 64;
    for (int i = 0; i <= N; ++i) {
        const double ph = 1.5 * i / N; // 1.5 cycles
        const double frac = ph - std::floor(ph);
        double v;
        switch (w) {
        case Waveform::Noise: {
            const unsigned s = static_cast<unsigned>(i) * 2654435761u;
            v = ((s >> 7) % 1000) / 500.0 - 1.0;
            v *= 0.8;
            break;
        }
        case Waveform::Custom:
            v = 0.65 * std::sin(2 * std::numbers::pi * ph) + 0.3 * std::sin(6 * std::numbers::pi * ph);
            break;
        case Waveform::Pulse: v = frac < 0.2 ? 1.0 : -1.0; break;
        case Waveform::DC: v = 0.4; break;
        default: v = SignalGenerator::shape(w, frac, 0.5, 0.0); break;
        }
        const double x = m + wdt * i / N, y = cy - v * h * 0.42;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    p.drawPath(path);
    return QIcon(pm);
}

QIcon appIcon() {
    QPixmap pm(256, 256);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, 0, 256);
    g.setColorAt(0, QColor(0x1b, 0x22, 0x2e));
    g.setColorAt(1, QColor(0x05, 0x07, 0x0a));
    p.setBrush(g);
    p.setPen(QPen(QColor(0x3a, 0x44, 0x52), 4));
    p.drawRoundedRect(QRectF(8, 8, 240, 240), 48, 48);
    p.setPen(QPen(QColor(255, 255, 255, 30), 2));
    for (int i = 1; i < 6; ++i) {
        p.drawLine(QPointF(28 + i * 33.3, 40), QPointF(28 + i * 33.3, 216));
        p.drawLine(QPointF(28, 40 + i * 29.3), QPointF(228, 40 + i * 29.3));
    }
    auto wave = [&](QColor c, double amp, double cyc, double ph, bool square) {
        QPainterPath path;
        for (int i = 0; i <= 200; ++i) {
            const double x = 28 + i;
            const double a = 2 * std::numbers::pi * (cyc * i / 200.0 + ph);
            const double v = square ? (std::sin(a) >= 0 ? 1.0 : -1.0) : std::sin(a);
            const double y = 128 - amp * v;
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 70), 14, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.setPen(QPen(c, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    };
    wave(QColor(0x22, 0xD3, 0xEE), 46, 2.0, 0.0, true);
    wave(QColor(0xFF, 0xD6, 0x0A), 70, 1.5, 0.0, false);
    return QIcon(pm);
}

// ================================================================== Knob
Knob::Knob(const QString& title, QWidget* parent) : QWidget(parent), title_(title) {
    setFocusPolicy(Qt::WheelFocus);
    setCursor(Qt::SizeVerCursor);
    stepper_ = linearStepper(1.0, 0.1);
    formatter_ = [](double v) { return QString::number(v, 'g', 4); };
    parser_ = [](const QString& s, double& v) { return parseSI(s.toStdString(), v); };
    setToolTip(tr("Wheel / drag: adjust (Shift = fine)\nDouble-click: type a value (e.g. 1.5k, 200m, 10u)\nRight-click: reset"));
}

void Knob::setAccent(const QColor& c) { accent_ = c; update(); }
void Knob::setValue(double v) { value_ = v; update(); }
void Knob::setRange(double lo, double hi) { lo_ = lo; hi_ = hi; }

Knob::Stepper Knob::step125Stepper() {
    return [](double v, int s, bool fine) {
        if (fine) return v * std::pow(1.02, s);
        return step125(v, s);
    };
}
Knob::Stepper Knob::linearStepper(double step, double fineStep) {
    return [step, fineStep](double v, int s, bool fine) {
        const double d = fine ? fineStep : step;
        return std::round((v + s * d) / d) * d;
    };
}
Knob::Stepper Knob::logStepper() {
    return [](double v, int s, bool fine) {
        if (v <= 0) return s > 0 ? 1.0 : 0.0;
        for (int i = 0; i < std::abs(s); ++i) {
            const double up = s > 0 ? 1.0 : -1.0;
            double dec = std::pow(10.0, std::floor(std::log10(v) + 1e-9));
            if (up < 0 && std::fabs(v - dec) < 1e-9 * dec) dec /= 10.0; // stepping down across a decade
            const double inc = dec / (fine ? 100.0 : 10.0);
            v = std::round((v + up * inc) / inc) * inc;
        }
        return v;
    };
}

void Knob::propose(double v) {
    if (!std::isfinite(v)) return;
    v = std::clamp(v, lo_, hi_);
    if (v == value_) return;
    value_ = v;
    update();
    emit valueEdited(v);
}

void Knob::applySteps(int steps, bool fine) {
    if (steps == 0) return;
    angle_ += steps * 15.0;
    propose(stepper_(value_, steps, fine));
}

void Knob::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    // title
    QFont tf = font();
    tf.setPointSizeF(8.5);
    tf.setBold(true);
    tf.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    p.setFont(tf);
    p.setPen(theme::dim);
    p.drawText(QRectF(0, 0, r.width(), 16), Qt::AlignCenter, title_.toUpper());

    // encoder
    const double radius = std::min(26.0, (r.height() - 40) / 2.0);
    const QPointF c(r.width() / 2.0, 18 + radius + 3);
    const bool active = dragging_ || hasFocus() || underMouse();
    // outer glow ring
    p.setPen(QPen(QColor(accent_.red(), accent_.green(), accent_.blue(), active ? 160 : 70), 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, radius + 3, radius + 3);
    // body
    QRadialGradient body(c - QPointF(radius * 0.35, radius * 0.45), radius * 1.6);
    body.setColorAt(0, QColor(0x4a, 0x52, 0x5e));
    body.setColorAt(0.45, QColor(0x25, 0x2a, 0x33));
    body.setColorAt(1, QColor(0x0c, 0x0e, 0x12));
    p.setBrush(body);
    p.setPen(QPen(QColor(0, 0, 0, 200), 1.2));
    p.drawEllipse(c, radius, radius);
    // knurl
    p.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
    for (int i = 0; i < 36; ++i) {
        const double a = (angle_ + i * 10.0) * std::numbers::pi / 180.0;
        p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * (radius - 4), c + QPointF(std::cos(a), std::sin(a)) * (radius - 1));
    }
    // cap
    QRadialGradient cap(c - QPointF(4, 6), radius * 0.8);
    cap.setColorAt(0, QColor(0x3a, 0x41, 0x4c));
    cap.setColorAt(1, QColor(0x16, 0x19, 0x1f));
    p.setBrush(cap);
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, radius * 0.68, radius * 0.68);
    // indicator
    const double a = (angle_ - 90.0) * std::numbers::pi / 180.0;
    p.setPen(QPen(accent_, 3.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * radius * 0.25, c + QPointF(std::cos(a), std::sin(a)) * radius * 0.6);

    // value readout
    const QRectF vr(3, r.height() - 21, r.width() - 6, 19);
    p.setPen(QPen(QColor(0x2a, 0x31, 0x3c), 1));
    p.setBrush(QColor(0x0b, 0x0e, 0x12));
    p.drawRoundedRect(vr, 4, 4);
    if (!editor_) {
        p.setFont(monoFont(9.5, true));
        p.setPen(accent_.lighter(115));
        p.drawText(vr, Qt::AlignCenter, formatter_(value_));
    }
}

void Knob::wheelEvent(QWheelEvent* e) {
    const QPoint d = e->angleDelta();
    wheelAccum_ += d.y() != 0 ? d.y() : d.x();
    const int steps = wheelAccum_ / 120;
    wheelAccum_ -= steps * 120;
    applySteps(steps, e->modifiers() & Qt::ShiftModifier);
    e->accept();
}

void Knob::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        dragAccum_ = 0;
        lastPos_ = e->position().toPoint();
        setFocus();
        update();
    }
}

void Knob::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    const QPoint pos = e->position().toPoint();
    dragAccum_ += (lastPos_.y() - pos.y()) + (pos.x() - lastPos_.x()) * 0.5;
    lastPos_ = pos;
    const int steps = static_cast<int>(dragAccum_ / 8.0);
    if (steps != 0) {
        dragAccum_ -= steps * 8.0;
        applySteps(steps, e->modifiers() & Qt::ShiftModifier);
    }
}

void Knob::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = false;
    update();
}

void Knob::mouseDoubleClickEvent(QMouseEvent*) { beginEdit(); }

void Knob::contextMenuEvent(QContextMenuEvent* e) {
    propose(default_);
    e->accept();
}

void Knob::beginEdit() {
    if (editor_) return;
    editor_ = new QLineEdit(this);
    editor_->setAlignment(Qt::AlignCenter);
    editor_->setFont(monoFont(9.5, true));
    editor_->setGeometry(3, height() - 22, width() - 6, 21);
    editor_->setText(formatter_(value_));
    editor_->selectAll();
    editor_->show();
    editor_->setFocus();
    connect(editor_, &QLineEdit::editingFinished, this, [this] {
        if (!editor_) return;
        double v;
        const QString text = editor_->text();
        editor_->deleteLater();
        editor_ = nullptr;
        if (parser_(text, v)) propose(v);
        update();
    });
    update();
}

// ================================================================== Segmented
Segmented::Segmented(const QStringList& labels, QWidget* parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    for (int i = 0; i < labels.size(); ++i) {
        auto* b = new QPushButton(labels[i], this);
        b->setObjectName("segBtn");
        b->setCheckable(true);
        b->setProperty("segPos", labels.size() == 1 ? "only" : i == 0 ? "first" : i == labels.size() - 1 ? "last" : "mid");
        b->setFocusPolicy(Qt::NoFocus);
        b->setMinimumWidth(10);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        group_->addButton(b, i);
        lay->addWidget(b);
        buttons_.push_back(b);
    }
    group_->setExclusive(true);
    connect(group_, &QButtonGroup::idClicked, this, &Segmented::selected);
}

void Segmented::setCurrent(int i) {
    if (i >= 0 && i < static_cast<int>(buttons_.size())) buttons_[i]->setChecked(true);
}
int Segmented::current() const { return group_->checkedId(); }

void Segmented::setAccent(const QColor& c) {
    const QString css = QStringLiteral("QPushButton#segBtn:checked { background: rgba(%1,%2,%3,70); border-color: %4; color: white; }")
                            .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.name());
    setStyleSheet(css);
}

// ================================================================== ColorButton
ColorButton::ColorButton(QWidget* parent) : QPushButton(parent) {
    setFixedSize(30, 22);
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(color_, this, tr("Trace color"));
        if (c.isValid()) {
            setColor(c);
            emit colorPicked(c);
        }
    });
}
void ColorButton::setColor(const QColor& c) { color_ = c; update(); }
void ColorButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0x3a, 0x44, 0x52), 1));
    p.setBrush(color_);
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
}

// ================================================================== Card
Card::Card(const QString& title, const QColor& accent, QWidget* parent) : QFrame(parent) {
    setObjectName("card");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 8, 10, 10);
    outer->setSpacing(6);
    auto* head = new QHBoxLayout;
    head->setSpacing(8);
    bar_ = new QFrame(this);
    bar_->setFixedSize(3, 14);
    title_ = new QLabel(title, this);
    title_->setObjectName("section");
    head->addWidget(bar_);
    head->addWidget(title_);
    head->addStretch();
    outer->addLayout(head);
    body_ = new QVBoxLayout;
    body_->setSpacing(6);
    outer->addLayout(body_);
    setAccent(accent);
}

void Card::setAccent(const QColor& c) {
    bar_->setStyleSheet(QStringLiteral("background: %1; border-radius: 1px;").arg(c.name()));
}

} // namespace slp::ui
