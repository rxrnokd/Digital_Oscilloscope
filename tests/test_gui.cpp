// SIGNAL LAB PRO - GUI interaction tests (Qt Test, runs with QT_QPA_PLATFORM=offscreen).
#include "Controls.h"
#include "ScopeView.h"
#include "SpectrumView.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <numbers>

Q_DECLARE_METATYPE(slp::ScopeSettings)

using namespace slp;

namespace {
constexpr double kL = 34, kR = 34, kT = 24, kB = 56; // ScopeView margins

QRectF plotRect(const QWidget& w) { return QRectF(kL, kT, w.width() - kL - kR, w.height() - kT - kB); }

void mouse(QWidget& w, QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QMouseEvent e(t, p, w.mapToGlobal(p), b, bs, m);
    QApplication::sendEvent(&w, &e);
}
void drag(QWidget& w, QPointF a, QPointF b, Qt::MouseButton button = Qt::LeftButton) {
    mouse(w, QEvent::MouseButtonPress, a, button, button);
    for (int i = 1; i <= 5; ++i) mouse(w, QEvent::MouseMove, a + (b - a) * (i / 5.0), Qt::NoButton, button);
    mouse(w, QEvent::MouseButtonRelease, b, button, Qt::NoButton);
}
void wheel(QWidget& w, QPointF p, int delta, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QWheelEvent e(p, w.mapToGlobal(p), QPoint(), QPoint(0, delta), Qt::NoButton, m, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&w, &e);
}
ScopeSettings lastEdit(const QSignalSpy& spy) { return spy.last().at(0).value<ScopeSettings>(); }
} // namespace


class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<ScopeSettings>("slp::ScopeSettings"); }

    void wheelChangesTimebaseAndVoltsPerDiv() {
        ScopeView v;
        v.resize(1000, 600);
        auto s = ScopeSettings::defaults();
        v.setSettings(s);
        QSignalSpy spy(&v, &ScopeView::settingsEdited);
        wheel(v, plotRect(v).center(), 120);
        QCOMPARE(spy.count(), 1);
        QVERIFY(qFuzzyCompare(lastEdit(spy).timebase.secPerDiv, 100e-6));
        v.setSettings(lastEdit(spy));
        wheel(v, plotRect(v).center(), 120, Qt::ShiftModifier);
        QVERIFY(qFuzzyCompare(lastEdit(spy).ch[0].voltsPerDiv, 0.2));
    }

    void dragTriggerLevelMarker() {
        ScopeView v;
        v.resize(1000, 600);
        auto s = ScopeSettings::defaults(); // CH1: 0.5 V/div, position +1.8 div, level 0 V
        v.setSettings(s);
        QSignalSpy spy(&v, &ScopeView::settingsEdited);
        const QRectF pr = plotRect(v);
        const double unit = pr.height() / 8.0;
        const QPointF a(pr.right() + 15, pr.center().y() - s.ch[0].position * unit);
        drag(v, a, a - QPointF(0, 2 * unit));
        QVERIFY(spy.count() > 0);
        QVERIFY(std::fabs(lastEdit(spy).trigger.level - 1.0) < 1e-6);
    }

    void dragChannelPositionMarker() {
        ScopeView v;
        v.resize(1000, 600);
        auto s = ScopeSettings::defaults();
        v.setSettings(s);
        QSignalSpy spy(&v, &ScopeView::settingsEdited);
        const QRectF pr = plotRect(v);
        const double unit = pr.height() / 8.0;
        const QPointF a(15, pr.center().y() - s.ch[1].position * unit); // CH2 marker
        drag(v, a, QPointF(15, pr.center().y() - 1.0 * unit));
        QVERIFY(std::fabs(lastEdit(spy).ch[1].position - 1.0) < 0.021);
        QCOMPARE(lastEdit(spy).display.selectedTrace, 1);
    }

    void panAndZoomBox() {
        ScopeView v;
        v.resize(1000, 600);
        auto s = ScopeSettings::defaults(); // 200 us/div, trigger at 50 %
        v.setSettings(s);
        QSignalSpy spy(&v, &ScopeView::settingsEdited);
        const QRectF pr = plotRect(v);
        // left drag 1 division to the right -> view moves to earlier times (delay = -1 div)
        drag(v, pr.center(), pr.center() + QPointF(pr.width() / 10.0, 0));
        QVERIFY(std::fabs(lastEdit(spy).timebase.delay + 200e-6) < 1e-9);
        v.setSettings(s);
        // right-drag box from t = -500 us to +500 us -> 100 us/div centred on t = 0
        const double xa = pr.left() + pr.width() * 0.25, xb = pr.left() + pr.width() * 0.75;
        drag(v, QPointF(xa, pr.center().y()), QPointF(xb, pr.center().y() + 3), Qt::RightButton);
        QVERIFY(qFuzzyCompare(lastEdit(spy).timebase.secPerDiv, 100e-6));
        QVERIFY(std::fabs(lastEdit(spy).timebase.delay) < 1e-9);
    }

    void cursorDrag() {
        ScopeView v;
        v.resize(1000, 600);
        auto s = ScopeSettings::defaults();
        s.display.cursorMode = CursorMode::Manual;
        s.display.cursorX1 = -200e-6;
        v.setSettings(s);
        QSignalSpy spy(&v, &ScopeView::settingsEdited);
        const QRectF pr = plotRect(v);
        const double x1 = pr.left() + (s.display.cursorX1 + 1e-3) / 2e-3 * pr.width();
        drag(v, QPointF(x1, pr.center().y() + 37), QPointF(pr.center().x(), pr.center().y() + 37));
        QVERIFY(std::fabs(lastEdit(spy).display.cursorX1) < 2e-6);
    }

    void knobWheelAndRange() {
        ui::Knob k("Test");
        k.resize(90, 104);
        k.setStepper(ui::Knob::step125Stepper());
        k.setRange(1e-3, 10.0);
        k.setValue(0.5);
        QSignalSpy spy(&k, &ui::Knob::valueEdited);
        wheel(k, QPointF(45, 50), 120);
        QCOMPARE(spy.count(), 1);
        QVERIFY(qFuzzyCompare(spy.last().at(0).toDouble(), 1.0));
        k.setValue(10.0);
        wheel(k, QPointF(45, 50), 120); // clamped at the maximum -> no change, no signal
        QCOMPARE(spy.count(), 1);
        auto log = ui::Knob::logStepper();
        QVERIFY(qFuzzyCompare(log(1000.0, 1, false), 1100.0));
        QVERIFY(qFuzzyCompare(log(1000.0, -1, false), 990.0));
        QVERIFY(qFuzzyCompare(log(100.0, -1, true), 99.9));
    }

    void spectrumWheelZoom() {
        SpectrumView v;
        v.resize(800, 400);
        auto s = ScopeSettings::defaults();
        s.spectrum.fStart = 0;
        s.spectrum.fStop = 10000;
        v.setSettings(s);
        QSignalSpy spy(&v, &SpectrumView::settingsEdited);
        wheel(v, QPointF(56, 100), 120); // zoom in at the left edge: start stays 0
        QCOMPARE(spy.count(), 1);
        const auto r = lastEdit(spy);
        QVERIFY(r.spectrum.fStart < 1e-9);
        QVERIFY(std::fabs(r.spectrum.fStop - 8000.0) < 1e-6);
        QTest::mouseDClick(&v, Qt::LeftButton, Qt::NoModifier, QPoint(300, 100));
        QCOMPARE(lastEdit(spy).spectrum.fStop, 0.0); // full span
    }

    void rendersFrameWithoutCrash() {
        ScopeView v;
        v.resize(900, 500);
        auto s = ScopeSettings::defaults();
        s.display.persistence = Persistence::Long;
        s.display.cursorMode = CursorMode::Track;
        v.setSettings(s);
        auto f = std::make_shared<Frame>();
        f->seq = 1;
        for (int c = 0; c < 2; ++c) {
            auto& tr = f->tr[c];
            tr.present = true;
            tr.dt = 1e-6;
            tr.t0 = -1e-3;
            tr.y.resize(2001);
            for (size_t i = 0; i < tr.y.size(); ++i) tr.y[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000 * (tr.t0 + i * tr.dt)));
            tr.y[500] = std::numeric_limits<float>::quiet_NaN(); // gaps must not break drawing
        }
        v.setFrame(f);
        QImage img(v.size(), QImage::Format_ARGB32);
        v.render(&img);
        // something bright (a trace) was drawn in the plot area
        int bright = 0;
        for (int y = 30; y < img.height() - 60; y += 2)
            for (int x = 40; x < img.width() - 40; x += 2)
                if (qGray(img.pixel(x, y)) > 120) ++bright;
        QVERIFY(bright > 200);
    }
};

QTEST_MAIN(GuiTest)
#include "test_gui.moc"
