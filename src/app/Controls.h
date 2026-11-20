// SIGNAL LAB PRO - instrument style widgets and the dark theme.
#pragma once

#include "ScopeSettings.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QIcon>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include <functional>
#include <vector>

class QLineEdit;
class QButtonGroup;
class QLabel;
class QVBoxLayout;

namespace slp::ui {

namespace theme {
inline const QColor bg0{0x0A, 0x0C, 0x10};
inline const QColor bg1{0x10, 0x13, 0x19};
inline const QColor bg2{0x16, 0x1A, 0x21};
inline const QColor border{0x26, 0x2C, 0x36};
inline const QColor text{0xC9, 0xD1, 0xD9};
inline const QColor dim{0x7D, 0x85, 0x90};
inline const QColor accent{0x3F, 0xA9, 0xF5};
inline const QColor green{0x3D, 0xDC, 0x84};
inline const QColor red{0xFF, 0x4D, 0x4F};
inline const QColor amber{0xFF, 0xB0, 0x20};
} // namespace theme

QColor toQColor(uint32_t rgb);
uint32_t fromQColor(const QColor& c);
QFont monoFont(double pointSize = 11.0, bool bold = false);
QString styleSheet();
QIcon waveformIcon(Waveform w, const QColor& color, int size = 26);
QIcon appIcon();
QString qs(const std::string& s);
QString fmt(double v, const char* unit, int digits = 4);

// Rotary encoder style control. The value is owned by the caller: the knob only
// proposes new values through valueEdited(); call setValue() to update the display.
class Knob : public QWidget {
    Q_OBJECT
public:
    using Stepper = std::function<double(double value, int steps, bool fine)>;
    using Formatter = std::function<QString(double)>;
    using Parser = std::function<bool(const QString&, double&)>;

    explicit Knob(const QString& title, QWidget* parent = nullptr);

    void setAccent(const QColor& c);
    void setValue(double v);
    double value() const { return value_; }
    void setRange(double lo, double hi);
    void setDefault(double v) { default_ = v; }
    void setStepper(Stepper s) { stepper_ = std::move(s); }
    void setFormatter(Formatter f) { formatter_ = std::move(f); update(); }
    void setParser(Parser p) { parser_ = std::move(p); }
    void setTitle(const QString& t) { title_ = t; update(); }
    QSize sizeHint() const override { return {86, 104}; }
    QSize minimumSizeHint() const override { return {76, 96}; }

    static Stepper step125Stepper();
    static Stepper linearStepper(double step, double fineStep);
    static Stepper logStepper(); // ~2 significant digits per step

signals:
    void valueEdited(double v);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    void applySteps(int steps, bool fine);
    void propose(double v);
    void beginEdit();

    QString title_;
    QColor accent_ = theme::accent;
    double value_ = 0.0, lo_ = -1e300, hi_ = 1e300, default_ = 0.0;
    double angle_ = 0.0; // visual rotation of the encoder (cosmetic)
    Stepper stepper_;
    Formatter formatter_;
    Parser parser_;
    bool dragging_ = false;
    double dragAccum_ = 0.0;
    int wheelAccum_ = 0;
    QPoint lastPos_;
    QLineEdit* editor_ = nullptr;
};

// Row of mutually exclusive buttons (segmented control).
class Segmented : public QWidget {
    Q_OBJECT
public:
    explicit Segmented(const QStringList& labels, QWidget* parent = nullptr);
    void setCurrent(int i);
    int current() const;
    void setAccent(const QColor& c);
signals:
    void selected(int index);

private:
    std::vector<QPushButton*> buttons_;
    QButtonGroup* group_;
};

// Small square color swatch button that opens a color dialog.
class ColorButton : public QPushButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget* parent = nullptr);
    void setColor(const QColor& c);
    QColor color() const { return color_; }
signals:
    void colorPicked(const QColor& c);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QColor color_;
};

// A titled "card" container with an optional colored accent bar.
class Card : public QFrame {
public:
    explicit Card(const QString& title, const QColor& accent = theme::accent, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }
    QLabel* titleLabel() const { return title_; }
    void setAccent(const QColor& c);

private:
    QVBoxLayout* body_;
    QLabel* title_;
    QFrame* bar_;
};

} // namespace slp::ui
