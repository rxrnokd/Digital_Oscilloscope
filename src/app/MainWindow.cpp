#include "MainWindow.h"

#include "Dsp.h"
#include "FileIO.h"
#include "SettingsJson.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#if QT_CONFIG(permissions)
#define SLP_HAVE_PERMISSIONS 1
#endif
#endif

#include <algorithm>
#include <cmath>

namespace slp {

using namespace ui;

namespace {

const char* kTraceNames[] = {"CH1", "CH2", "CH3", "CH4", "MATH"};
const MeasId kTableMeas[] = {MeasId::Max, MeasId::Min, MeasId::PkPk, MeasId::Rms, MeasId::Mean,
                             MeasId::Freq, MeasId::Period, MeasId::Duty, MeasId::Rise, MeasId::Fall};
const double kSampleRates[] = {1e3, 2e3, 5e3, 10e3, 20e3, 44.1e3, 48e3, 50e3, 96e3, 100e3, 192e3, 200e3,
                               500e3, 1e6, 2e6, 5e6, 10e6, 20e6, 50e6, 100e6};
const int kFftSizes[] = {1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144};

QLabel* dimLabel(const QString& t, QWidget* parent = nullptr) {
    auto* l = new QLabel(t, parent);
    l->setObjectName("dim");
    l->setWordWrap(true);
    return l;
}

QHBoxLayout* row(std::initializer_list<QWidget*> ws, int stretchAfter = -1) {
    auto* h = new QHBoxLayout;
    h->setSpacing(6);
    int i = 0;
    for (auto* w : ws) {
        h->addWidget(w);
        if (i++ == stretchAfter) h->addStretch();
    }
    return h;
}

QWidget* labeled(const QString& text, QWidget* w) {
    auto* box = new QWidget;
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    auto* l = new QLabel(text);
    l->setMinimumWidth(78);
    h->addWidget(l);
    h->addWidget(w, 1);
    return box;
}

// 1-2-5 stepping that can also reach 0 (holdoff, edge time, noise)
Knob::Stepper zero125(double minNonZero) {
    return [minNonZero](double v, int s, bool fine) {
        if (v <= 0) return s > 0 ? minNonZero : 0.0;
        const double r = fine ? v * std::pow(1.05, s) : step125(v, s);
        return r < minNonZero * 0.99 ? 0.0 : r;
    };
}

QString sourceName(SourceKind k) {
    switch (k) {
    case SourceKind::Generator: return "GENERATOR";
    case SourceKind::File: return "FILE";
    case SourceKind::Audio: return "AUDIO IN";
    case SourceKind::Serial: return "SERIAL ADC";
    }
    return "?";
}

} // namespace

// =========================================================================== ctor
MainWindow::MainWindow(const AppOptions& opt, QWidget* parent) : QMainWindow(parent), opt_(opt) {
    setWindowTitle("SIGNAL LAB PRO");
    setWindowIcon(appIcon());
    clock_.start();

    s_ = ScopeSettings::defaults();
    if (opt_.settingsPath.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(dir);
        opt_.settingsPath = dir + "/session.json";
    }
    QString err;
    if (QFileInfo::exists(opt_.settingsPath)) loadSettingsFile(s_, opt_.settingsPath, &err);

    push_ = std::make_shared<PushSource>();
    audio_ = new AudioInput(push_, this);
    engine_ = std::make_unique<AcquisitionEngine>(true);
    engine_->setPushSource(push_);

    // --- layout
    scope_ = new ScopeView;
    spectrum_ = new SpectrumView;
    measPanel_ = buildMeasurementPanel();
    vsplit_ = new QSplitter(Qt::Vertical);
    vsplit_->addWidget(scope_);
    vsplit_->addWidget(spectrum_);
    vsplit_->addWidget(measPanel_);
    vsplit_->setStretchFactor(0, 5);
    vsplit_->setStretchFactor(1, 3);
    vsplit_->setStretchFactor(2, 0);
    vsplit_->setChildrenCollapsible(false);
    vsplit_->setHandleWidth(4);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(false);
    tabs_->addTab(scrollPage(buildChannelsTab()), "CH");
    tabs_->addTab(scrollPage(buildHorizontalTab()), "H/TRIG");
    tabs_->addTab(scrollPage(buildGeneratorTab()), "GEN");
    tabs_->addTab(scrollPage(buildMathTab()), "MATH");
    tabs_->addTab(scrollPage(buildSpectrumTab()), "FFT");
    tabs_->addTab(scrollPage(buildDisplayTab()), "DISP");
    tabs_->addTab(scrollPage(buildInputTab()), "INPUT");
    tabs_->setMinimumWidth(370);
    tabs_->tabBar()->setExpanding(true);
    tabs_->setMaximumWidth(460);

    hsplit_ = new QSplitter(Qt::Horizontal);
    hsplit_->addWidget(vsplit_);
    hsplit_->addWidget(tabs_);
    hsplit_->setStretchFactor(0, 1);
    hsplit_->setStretchFactor(1, 0);
    hsplit_->setHandleWidth(4);
    hsplit_->setChildrenCollapsible(false);

    auto* central = new QWidget;
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());
    auto* body = new QWidget;
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(6, 6, 6, 6);
    bl->addWidget(hsplit_);
    v->addWidget(body, 1);
    setCentralWidget(central);
    buildMenus();
    statusBar()->showMessage(tr("Ready - FFT backend: %1").arg(RealFFT::backendName()), 5000);

    connect(scope_, &ScopeView::settingsEdited, this, [this](const ScopeSettings& s) { adopt(s); });
    connect(spectrum_, &SpectrumView::settingsEdited, this, [this](const ScopeSettings& s) { adopt(s); });
    connect(audio_, &AudioInput::stateChanged, this, [this](const QString& m) {
        statusBar()->showMessage(m, 4000);
        if (audioInfo_) audioInfo_->setText(audio_->running() ? audio_->description() : tr("stopped"));
        if (audioBtn_) audioBtn_->setText(audio_->running() ? tr("Stop audio input") : tr("Start audio input"));
    });

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setTimerType(Qt::PreciseTimer);
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::refresh);
    refreshTimer_->start(16);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    connect(saveTimer_, &QTimer::timeout, this, &MainWindow::saveSession);

    // restore the file of the last session
    if (s_.acq.source == SourceKind::File && !s_.filePath.empty()) {
        QString e;
        if (!loadSignalFile(qs(s_.filePath), &e)) statusBar()->showMessage(tr("Could not reload %1: %2").arg(qs(s_.filePath), e), 6000);
    }
    if (!opt_.openFile.isEmpty()) {
        QString e;
        if (loadSignalFile(opt_.openFile, &e)) s_.acq.source = SourceKind::File;
        else statusBar()->showMessage(e, 6000);
    }
    if (!opt_.loadSetupPath.isEmpty()) {
        QString e;
        if (!applySetupFile(opt_.loadSetupPath, &e)) statusBar()->showMessage(tr("Setup error: ") + e, 6000);
    }
    adopt(s_);
    if (s_.trigger.mode == TriggerMode::Single) engine_->single();
    else engine_->run();
    if (!opt_.openFile.isEmpty() && file_) QTimer::singleShot(300, this, &MainWindow::autoscale);

    resize(opt_.size.isValid() ? opt_.size : QSize(1600, 1000));
    vsplit_->setSizes({620, 330, 150});
    if (opt_.tab >= 0 && opt_.tab < tabs_->count()) tabs_->setCurrentIndex(opt_.tab);
    hsplit_->setSizes({1200, 390});
    if (opt_.quitAfterMs > 0) {
        QTimer::singleShot(opt_.quitAfterMs, this, [this] {
            if (!opt_.screenshotPath.isEmpty()) grab().save(opt_.screenshotPath);
            close();
        });
    }
}

MainWindow::~MainWindow() {
    audio_->stop();
    engine_.reset();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveSession();
    if (!opt_.dumpSettingsPath.isEmpty()) saveSettingsFile(s_, opt_.dumpSettingsPath);
    e->accept();
}

QWidget* MainWindow::scrollPage(QWidget* content) {
    auto* sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setWidget(content);
    return sa;
}

Knob* MainWindow::knob(const QString& title, Knob::Stepper st, Knob::Formatter f, double lo, double hi, double def,
                       std::function<void(double)> onEdit) {
    auto* k = new Knob(title);
    k->setStepper(std::move(st));
    k->setFormatter(std::move(f));
    k->setRange(lo, hi);
    k->setDefault(def);
    connect(k, &Knob::valueEdited, this, [this, onEdit](double v) {
        if (!syncing_) onEdit(v);
    });
    return k;
}

// =========================================================================== header
QWidget* MainWindow::buildHeader() {
    auto* h = new QFrame;
    h->setObjectName("header");
    h->setFixedHeight(52);
    auto* l = new QHBoxLayout(h);
    l->setContentsMargins(14, 6, 14, 6);
    l->setSpacing(10);

    auto* logo = new QLabel;
    logo->setTextFormat(Qt::RichText);
    logo->setText("<span style='font-size:17px; font-weight:800; letter-spacing:3px; color:#e6edf3'>SIGNAL LAB</span>"
                  "<span style='font-size:17px; font-weight:800; letter-spacing:3px; color:#3fa9f5'> PRO</span>");
    auto* sub = new QLabel("DIGITAL OSCILLOSCOPE  \xC2\xB7  SPECTRUM ANALYZER");
    sub->setStyleSheet("color:#5d6672; font-size:9px; letter-spacing:2px;");
    auto* lv = new QVBoxLayout;
    lv->setSpacing(0);
    lv->addWidget(logo);
    lv->addWidget(sub);
    l->addLayout(lv);
    l->addSpacing(18);

    auto mk = [&](const QString& t, int w) {
        auto* b = new QPushButton(t);
        b->setFixedHeight(34);
        b->setMinimumWidth(w);
        b->setFocusPolicy(Qt::NoFocus);
        l->addWidget(b);
        return b;
    };
    runBtn_ = mk("RUN / STOP", 112);
    connect(runBtn_, &QPushButton::clicked, this, &MainWindow::runStop);
    singleBtn_ = mk("SINGLE", 84);
    connect(singleBtn_, &QPushButton::clicked, this, &MainWindow::single);
    auto* as = mk("AUTO SCALE", 104);
    as->setStyleSheet("QPushButton { color:#e6edf3; border-color:#3a5572; background:#17283a; font-weight:700; }"
                      "QPushButton:hover { background:#1d3550; }");
    connect(as, &QPushButton::clicked, this, &MainWindow::autoscale);
    auto* force = mk("FORCE", 70);
    connect(force, &QPushButton::clicked, this, [this] { engine_->forceTrigger(); });
    l->addSpacing(12);

    statePill_ = new QLabel("STOP");
    statePill_->setAlignment(Qt::AlignCenter);
    statePill_->setFixedSize(84, 28);
    statePill_->setFont(monoFont(10.5, true));
    l->addWidget(statePill_);
    auto info = [&](QLabel*& lab, int w) {
        lab = new QLabel;
        lab->setFont(monoFont(10));
        lab->setStyleSheet("color:#aab3bd;");
        lab->setMinimumWidth(w);
        l->addWidget(lab);
    };
    info(rateLabel_, 150);
    info(fpsLabel_, 90);
    info(srcLabel_, 160);
    l->addStretch();

    auto* fs = mk("FULL", 50);
    fs->setToolTip(tr("Full screen"));
    connect(fs, &QPushButton::clicked, this, &MainWindow::toggleFullScreen);
    return h;
}

// =========================================================================== CH tab
QWidget* MainWindow::buildChannelsTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    for (int c = 0; c < kNumChannels; ++c) {
        auto& u = chUi_[c];
        u.card = new Card(QStringLiteral("CHANNEL %1").arg(c + 1), toQColor(s_.ch[c].color));
        u.enable = new QPushButton("ON");
        u.enable->setCheckable(true);
        u.enable->setFixedWidth(52);
        connect(u.enable, &QPushButton::toggled, this, [this, c](bool on) {
            if (syncing_) return;
            edit([&](ScopeSettings& s) {
                s.ch[c].enabled = on;
                if (on) s.display.selectedTrace = c;
            });
        });
        u.input = new QComboBox;
        connect(u.input, &QComboBox::currentIndexChanged, this, [this, c](int i) {
            if (syncing_ || i < 0) return;
            edit([&](ScopeSettings& s) { s.ch[c].input = i; });
        });
        u.invert = new QCheckBox("Invert");
        connect(u.invert, &QCheckBox::toggled, this, [this, c](bool on) {
            if (syncing_) return;
            edit([&](ScopeSettings& s) { s.ch[c].invert = on; });
        });
        auto* top = new QHBoxLayout;
        top->addWidget(u.enable);
        top->addWidget(u.input, 1);
        top->addWidget(u.invert);
        u.card->body()->addLayout(top);

        u.vdiv = knob("Volts/div", Knob::step125Stepper(),
                      [this](double v) {
                          const std::string unit = s_.acq.source == SourceKind::Audio ? "FS" : "V";
                          return fmt(v, unit.c_str(), 3) + "/";
                      },
                      1e-3, 10.0, 0.5, [this, c](double v) { edit([&](ScopeSettings& s) { s.ch[c].voltsPerDiv = v; }); });
        u.pos = knob("Position", Knob::linearStepper(0.1, 0.02), [](double v) { return QString::number(v, 'f', 2) + " div"; },
                     -8.0, 8.0, 0.0, [this, c](double v) { edit([&](ScopeSettings& s) { s.ch[c].position = v; }); });
        u.coupling = new Segmented({"DC", "AC", "GND"});
        connect(u.coupling, &Segmented::selected, this, [this, c](int i) {
            edit([&](ScopeSettings& s) { s.ch[c].coupling = static_cast<Coupling>(i); });
        });
        auto* kr = new QHBoxLayout;
        kr->addWidget(u.vdiv);
        kr->addWidget(u.pos);
        auto* side = new QVBoxLayout;
        side->addWidget(dimLabel(tr("Coupling")));
        side->addWidget(u.coupling);
        side->addStretch();
        kr->addLayout(side, 1);
        u.card->body()->addLayout(kr);
        v->addWidget(u.card);
    }
    v->addWidget(dimLabel(tr("AC coupling = 1st-order high-pass at %1 Hz (like a real scope input). "
                             "Mouse: wheel = time/div, Shift+wheel = V/div of the selected trace, "
                             "Alt+wheel = position, drag markers on the left to move a trace.")
                              .arg(kAcCouplingHz)));
    v->addStretch();
    return w;
}

// =========================================================================== H/TRIG tab
QWidget* MainWindow::buildHorizontalTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);

    auto* hc = new Card("HORIZONTAL");
    tdiv_ = knob("Time/div", Knob::step125Stepper(), [](double v) { return fmt(v, "s", 3) + "/"; }, 1e-8, 50.0, 200e-6,
                 [this](double v) { edit([&](ScopeSettings& s) { s.timebase.secPerDiv = v; }); });
    delay_ = knob("Delay",
                  [this](double v, int st, bool fine) { return v + st * s_.timebase.secPerDiv * (fine ? 0.02 : 0.2); },
                  [](double v) { return fmt(v, "s", 4); }, -1e3, 1e3, 0.0,
                  [this](double v) { edit([&](ScopeSettings& s) { s.timebase.delay = std::fabs(v) < 1e-15 ? 0.0 : v; }); });
    trigPos_ = knob("Trig pos", Knob::linearStepper(5.0, 1.0), [](double v) { return QString::number(v, 'f', 0) + " %"; }, 0, 100,
                    50.0, [this](double v) { edit([&](ScopeSettings& s) { s.timebase.triggerPos = v; }); });
    hc->body()->addLayout(row({tdiv_, delay_, trigPos_}));
    autoRoll_ = new QCheckBox(tr("Roll mode at \xE2\x89\xA5 50 ms/div (AUTO trigger)"));
    connect(autoRoll_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.timebase.autoRoll = on; });
    });
    hc->body()->addWidget(autoRoll_);
    refresh_ = new QComboBox;
    for (int r : {15, 30, 60, 120}) refresh_->addItem(QStringLiteral("%1 frames/s").arg(r), r);
    connect(refresh_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.acq.refreshRate = refresh_->itemData(i).toInt(); });
    });
    hc->body()->addWidget(labeled(tr("Display rate"), refresh_));
    auto* zero = new QPushButton(tr("Delay = 0"));
    connect(zero, &QPushButton::clicked, this, [this] { edit([&](ScopeSettings& s) { s.timebase.delay = 0; }); });
    hc->body()->addLayout(row({zero}, 0));
    v->addWidget(hc);

    auto* tc = new Card("TRIGGER", theme::amber);
    trigMode_ = new Segmented({"AUTO", "NORMAL", "SINGLE"});
    trigMode_->setAccent(theme::amber);
    connect(trigMode_, &Segmented::selected, this, [this](int i) { setTriggerMode(static_cast<TriggerMode>(i)); });
    tc->body()->addWidget(trigMode_);
    slope_ = new Segmented({"\xE2\x86\x91  RISING", "\xE2\x86\x93  FALLING"});
    slope_->setAccent(theme::amber);
    connect(slope_, &Segmented::selected, this, [this](int i) {
        edit([&](ScopeSettings& s) { s.trigger.slope = static_cast<TriggerSlope>(i); });
    });
    tc->body()->addWidget(slope_);
    trigSrc_ = new QComboBox;
    for (int c = 0; c < kNumChannels; ++c) trigSrc_->addItem(QStringLiteral("CH%1").arg(c + 1));
    connect(trigSrc_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.trigger.source = i; });
    });
    tc->body()->addWidget(labeled(tr("Source"), trigSrc_));
    level_ = knob("Level",
                  [this](double v, int st, bool fine) {
                      const double d = s_.ch[std::clamp(s_.trigger.source, 0, 3)].voltsPerDiv * (fine ? 0.02 : 0.1);
                      return std::round((v + st * d) / d) * d;
                  },
                  [this](double v) { return fmt(v, s_.acq.source == SourceKind::Audio ? "FS" : "V", 4); }, -1e6, 1e6, 0.0,
                  [this](double v) { edit([&](ScopeSettings& s) { s.trigger.level = v; }); });
    holdoff_ = knob("Holdoff", zero125(100e-9), [](double v) { return v <= 0 ? QStringLiteral("off") : fmt(v, "s", 3); }, 0.0,
                    10.0, 0.0, [this](double v) { edit([&](ScopeSettings& s) { s.trigger.holdoff = v; }); });
    auto* half = new QPushButton("50 %");
    half->setToolTip(tr("Set the level to the middle of the source signal"));
    connect(half, &QPushButton::clicked, this, [this] {
        auto f = scope_->frame();
        const int src = std::clamp(s_.trigger.source, 0, 3);
        if (f && f->tr[src].present && f->tr[src].meas[MeasId::Max].valid) {
            const auto& m = f->tr[src].meas;
            edit([&](ScopeSettings& s) { s.trigger.level = 0.5 * (m[MeasId::Max].v + m[MeasId::Min].v); });
        }
    });
    auto* force = new QPushButton(tr("FORCE"));
    connect(force, &QPushButton::clicked, this, [this] { engine_->forceTrigger(); });
    auto* side = new QVBoxLayout;
    side->addWidget(half);
    side->addWidget(force);
    side->addStretch();
    auto* kr = new QHBoxLayout;
    kr->addWidget(level_);
    kr->addWidget(holdoff_);
    kr->addLayout(side, 1);
    tc->body()->addLayout(kr);
    noiseRej_ = new QCheckBox(tr("Noise reject (hysteresis 0.5 div instead of 0.08 div)"));
    connect(noiseRej_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.trigger.hysteresisDiv = on ? 0.5 : 0.08; });
    });
    tc->body()->addWidget(noiseRej_);
    tc->body()->addWidget(dimLabel(tr("AUTO: free-runs when no trigger occurs.  NORMAL: updates only on a trigger.  "
                                      "SINGLE: captures one triggered record, then stops. The trigger instant is "
                                      "interpolated between samples; pre-trigger data comes from acquisition memory.")));
    v->addWidget(tc);

    auto* ac = new Card("ACQUISITION", theme::green);
    acqInfo_ = dimLabel("");
    acqInfo_->setFont(monoFont(9));
    ac->body()->addWidget(acqInfo_);
    ac->body()->addWidget(dimLabel(tr("Memory %1 Mpts/channel, max %2 Mpts on screen (automatic decimation beyond). "
                                      "Virtual sources stream up to %3 MSa/s/ch in real time; above that the engine "
                                      "acquires segments with dead time, like a real DSO.")
                                       .arg(kRingCapacity / (1 << 20))
                                       .arg(kMaxRecord / (1 << 20))
                                       .arg(kGenBudget / 1e6)));
    v->addWidget(ac);
    v->addStretch();
    return w;
}

// =========================================================================== GEN tab
QWidget* MainWindow::buildGeneratorTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);

    auto* gc = new Card("FUNCTION GENERATOR", theme::green);
    genSelect_ = new Segmented({"GEN 1", "GEN 2", "GEN 3", "GEN 4"});
    genSelect_->setAccent(theme::green);
    connect(genSelect_, &Segmented::selected, this, [this](int i) {
        genSel_ = i;
        syncControls();
    });
    gc->body()->addWidget(genSelect_);
    genOutput_ = new QPushButton("OUTPUT ON");
    genOutput_->setCheckable(true);
    connect(genOutput_, &QPushButton::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.gen[genSel_].output = on; });
    });
    genRoute_ = dimLabel("");
    gc->body()->addLayout(row({genOutput_, genRoute_}));
    v->addWidget(gc);

    auto* wc = new Card("WAVEFORM", theme::green);
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    waveGroup_ = new QButtonGroup(this);
    const Waveform shapes[] = {Waveform::Sine, Waveform::Square, Waveform::Triangle, Waveform::Sawtooth,
                               Waveform::Pulse, Waveform::DC, Waveform::Noise, Waveform::Custom};
    for (int i = 0; i < 8; ++i) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setIcon(waveformIcon(shapes[i], QColor(0x3D, 0xDC, 0x84), 26));
        b->setIconSize(QSize(26, 26));
        b->setText(toString(shapes[i]));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setFixedHeight(54);
        b->setFont(monoFont(8));
        waveGroup_->addButton(b, i);
        grid->addWidget(b, i / 4, i % 4);
        waveBtns_.push_back(b);
    }
    connect(waveGroup_, &QButtonGroup::idClicked, this, [this, shapes](int i) {
        edit([&](ScopeSettings& s) {
            s.gen[genSel_].shape = shapes[i];
            if (shapes[i] == Waveform::Pulse && s.gen[genSel_].duty == 50.0) s.gen[genSel_].duty = 20.0;
        });
    });
    wc->body()->addLayout(grid);
    customBtn_ = new QPushButton(tr("Edit custom waveform (harmonics)..."));
    connect(customBtn_, &QPushButton::clicked, this, &MainWindow::editCustomWaveform);
    wc->body()->addWidget(customBtn_);
    v->addWidget(wc);

    auto* pc = new Card("PARAMETERS", theme::green);
    auto* pg = new QGridLayout;
    pg->setSpacing(2);
    auto genEdit = [this](auto member) {
        return [this, member](double val) { edit([&](ScopeSettings& s) { s.gen[genSel_].*member = val; }); };
    };
    gFreq_ = knob("Frequency", Knob::logStepper(), [](double v) { return fmt(v, "Hz", 5); }, 1e-3, 1e9, 1000.0,
                  genEdit(&GeneratorSettings::frequency));
    gAmp_ = knob("Amplitude", Knob::logStepper(), [](double v) { return fmt(v, "Vpp", 4); }, 0.0, 100.0, 2.0,
                 genEdit(&GeneratorSettings::vpp));
    gOff_ = knob("Offset", Knob::linearStepper(0.1, 0.01), [](double v) { return fmt(v, "V", 4); }, -50.0, 50.0, 0.0,
                 genEdit(&GeneratorSettings::offset));
    gPhase_ = knob("Phase", Knob::linearStepper(15.0, 1.0), [](double v) { return QString::number(v, 'f', 1) + "\xC2\xB0"; },
                   -360.0, 360.0, 0.0, genEdit(&GeneratorSettings::phaseDeg));
    gDuty_ = knob("Duty", Knob::linearStepper(5.0, 0.5), [](double v) { return QString::number(v, 'f', 1) + " %"; }, 1.0, 99.0,
                  50.0, genEdit(&GeneratorSettings::duty));
    gEdge_ = knob("Edge 10-90", zero125(1e-9), [](double v) { return v <= 0 ? QStringLiteral("ideal") : fmt(v, "s", 3); }, 0.0, 1.0,
                  0.0, genEdit(&GeneratorSettings::edgeTime));
    gNoise_ = knob("Noise rms", zero125(1e-3), [](double v) { return v <= 0 ? QStringLiteral("off") : fmt(v, "V", 3); }, 0.0, 10.0,
                   0.0, genEdit(&GeneratorSettings::noiseRms));
    pg->addWidget(gFreq_, 0, 0);
    pg->addWidget(gAmp_, 0, 1);
    pg->addWidget(gOff_, 0, 2);
    pg->addWidget(gPhase_, 1, 0);
    pg->addWidget(gDuty_, 1, 1);
    pg->addWidget(gEdge_, 1, 2);
    pg->addWidget(gNoise_, 2, 0);
    pc->body()->addLayout(pg);
    genWarn_ = new QLabel;
    genWarn_->setObjectName("warn");
    genWarn_->setWordWrap(true);
    pc->body()->addWidget(genWarn_);
    v->addWidget(pc);

    auto* sc = new Card("SAMPLING", theme::green);
    sampleRate_ = new QComboBox;
    for (double r : kSampleRates) sampleRate_->addItem(fmt(r, "Sa/s", 4), r);
    connect(sampleRate_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.acq.sampleRate = sampleRate_->itemData(i).toDouble(); });
    });
    sc->body()->addWidget(labeled(tr("Sample rate"), sampleRate_));
    auto* sync = new QPushButton(tr("Sync phases (restart all generators at 0\xC2\xB0)"));
    connect(sync, &QPushButton::clicked, this, [this] { engine_->syncGeneratorPhases(); });
    sc->body()->addWidget(sync);
    sc->body()->addWidget(dimLabel(tr("All four generators share the virtual ADC sample rate. The waveform is a "
                                      "phase-continuous function of the sample time t = n / fs, so the time axis is "
                                      "exact for every sample rate. Frequencies above fs/2 alias, as on real hardware.")));
    v->addWidget(sc);
    v->addStretch();
    return w;
}

// =========================================================================== MATH tab
QWidget* MainWindow::buildMathTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    auto* mc = new Card("MATH / DIGITAL FILTER", toQColor(s_.math.color));
    mathEnable_ = new QPushButton("MATH ON");
    mathEnable_->setCheckable(true);
    connect(mathEnable_, &QPushButton::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) {
            s.math.enabled = on;
            if (on) s.display.selectedTrace = kMathTrace;
        });
    });
    mc->body()->addWidget(mathEnable_);
    mathOp_ = new QComboBox;
    mathOp_->addItems({"A + B", "A \xE2\x88\x92 B", "A \xC3\x97 B", "d/dt A (derivative)", "\xE2\x88\xAB A dt (integral)",
                       "Low-pass  (Butterworth)", "High-pass (Butterworth)", "Band-pass (Butterworth)", "Moving average"});
    connect(mathOp_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.math.op = static_cast<MathOp>(i); });
    });
    mc->body()->addWidget(labeled(tr("Operation"), mathOp_));
    mathA_ = new QComboBox;
    mathB_ = new QComboBox;
    for (int c = 0; c < kNumChannels; ++c) {
        mathA_->addItem(QStringLiteral("CH%1").arg(c + 1));
        mathB_->addItem(QStringLiteral("CH%1").arg(c + 1));
    }
    connect(mathA_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.math.srcA = i; });
    });
    connect(mathB_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.math.srcB = i; });
    });
    mc->body()->addWidget(labeled(tr("Source A"), mathA_));
    mathBRow_ = labeled(tr("Source B"), mathB_);
    mc->body()->addWidget(mathBRow_);

    mathFilterBox_ = new QWidget;
    auto* fb = new QVBoxLayout(mathFilterBox_);
    fb->setContentsMargins(0, 0, 0, 0);
    mathOrder_ = new QSpinBox;
    mathOrder_->setRange(1, 8);
    connect(mathOrder_, &QSpinBox::valueChanged, this, [this](int o) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.math.order = o; });
    });
    fb->addWidget(labeled(tr("Order"), mathOrder_));
    mathFc_ = knob("Cut-off", Knob::logStepper(), [](double v) { return fmt(v, "Hz", 4); }, 1e-3, 1e9, 2000.0,
                   [this](double v) { edit([&](ScopeSettings& s) { s.math.cutoff = v; }); });
    mathFc2_ = knob("Upper edge", Knob::logStepper(), [](double v) { return fmt(v, "Hz", 4); }, 1e-3, 1e9, 8000.0,
                    [this](double v) { edit([&](ScopeSettings& s) { s.math.cutoffHigh = v; }); });
    fb->addLayout(row({mathFc_, mathFc2_}, 1));
    mc->body()->addWidget(mathFilterBox_);
    mathMa_ = new QSpinBox;
    mathMa_->setRange(1, 1 << 20);
    connect(mathMa_, &QSpinBox::valueChanged, this, [this](int n) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.math.maLength = n; });
    });
    mathMaRow_ = labeled(tr("Length (samples)"), mathMa_);
    mc->body()->addWidget(mathMaRow_);

    mathScale_ = knob("Scale/div", Knob::step125Stepper(), [this](double v) {
        auto f = scope_ ? scope_->frame() : nullptr;
        const std::string u = f && f->tr[kMathTrace].present ? f->tr[kMathTrace].unit : "V";
        return fmt(v, u.c_str(), 3) + "/";
    }, 1e-12, 1e12, 0.5, [this](double v) { edit([&](ScopeSettings& s) { s.math.unitsPerDiv = v; }); });
    mathPos_ = knob("Position", Knob::linearStepper(0.1, 0.02), [](double v) { return QString::number(v, 'f', 2) + " div"; }, -8, 8,
                    0, [this](double v) { edit([&](ScopeSettings& s) { s.math.position = v; }); });
    auto* auto_ = new QPushButton(tr("Auto scale MATH"));
    connect(auto_, &QPushButton::clicked, this, [this] {
        auto f = scope_->frame();
        if (!f || !f->tr[kMathTrace].present) return;
        const auto& m = f->tr[kMathTrace].meas;
        if (!m[MeasId::PkPk].valid) return;
        const double vpp = std::max(m[MeasId::PkPk].v, 1e-12), mid = 0.5 * (m[MeasId::Max].v + m[MeasId::Min].v);
        edit([&](ScopeSettings& s) {
            s.math.unitsPerDiv = ceil125(vpp / 6.0);
            s.math.position = -mid / s.math.unitsPerDiv;
        });
    });
    auto* side = new QVBoxLayout;
    side->addWidget(auto_);
    side->addStretch();
    auto* kr = new QHBoxLayout;
    kr->addWidget(mathScale_);
    kr->addWidget(mathPos_);
    kr->addLayout(side, 1);
    mc->body()->addLayout(kr);
    mathInfo_ = dimLabel("");
    mathInfo_->setFont(monoFont(9));
    mc->body()->addWidget(mathInfo_);
    v->addWidget(mc);
    v->addWidget(dimLabel(tr("Filters run on the acquired samples (causal IIR, double precision, bilinear "
                             "transform with pre-warping so |H(fc)| = -3.01 dB). Each record is processed with "
                             "settling history taken from acquisition memory. Compare before/after by showing the "
                             "source channel together with MATH, and set the FFT source to MATH.")));
    v->addStretch();
    return w;
}

// =========================================================================== FFT tab
QWidget* MainWindow::buildSpectrumTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    auto* c = new Card("SPECTRUM ANALYZER", theme::accent);
    specEnable_ = new QPushButton("FFT ON");
    specEnable_->setCheckable(true);
    connect(specEnable_, &QPushButton::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.spectrum.enabled = on; });
    });
    c->body()->addWidget(specEnable_);
    specSrc_ = new QComboBox;
    for (auto* n : kTraceNames) specSrc_->addItem(n);
    connect(specSrc_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.spectrum.source = i; });
    });
    c->body()->addWidget(labeled(tr("Source"), specSrc_));
    specSize_ = new QComboBox;
    for (int n : kFftSizes) specSize_->addItem(QStringLiteral("%1 points").arg(n), n);
    connect(specSize_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.spectrum.fftSize = specSize_->itemData(i).toInt(); });
    });
    c->body()->addWidget(labeled(tr("FFT size"), specSize_));
    specWin_ = new QComboBox;
    for (auto wt : {WindowType::Rectangular, WindowType::Hann, WindowType::Hamming, WindowType::BlackmanHarris, WindowType::FlatTop})
        specWin_->addItem(windowName(wt));
    connect(specWin_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.spectrum.window = static_cast<WindowType>(i); });
    });
    c->body()->addWidget(labeled(tr("Window"), specWin_));
    specScale_ = new Segmented({"dB (log)", "LINEAR"});
    connect(specScale_, &Segmented::selected, this, [this](int i) { edit([&](ScopeSettings& s) { s.spectrum.logScale = i == 0; }); });
    c->body()->addWidget(specScale_);
    specRef_ = knob("Ref level", Knob::linearStepper(5.0, 1.0), [](double v) { return QString::number(v, 'f', 0) + " dB"; }, -200,
                    100, 10.0, [this](double v) { edit([&](ScopeSettings& s) { s.spectrum.refLevel = v; }); });
    specStart_ = knob("Start", Knob::logStepper(), [](double v) { return fmt(v, "Hz", 4); }, 0.0, 1e10, 0.0,
                      [this](double v) { edit([&](ScopeSettings& s) { s.spectrum.fStart = v; }); });
    specStop_ = knob("Stop", Knob::logStepper(), [](double v) {
        return v <= 0 ? QStringLiteral("Nyquist") : fmt(v, "Hz", 4);
    }, 0.0, 1e10, 0.0, [this](double v) { edit([&](ScopeSettings& s) { s.spectrum.fStop = v; }); });
    c->body()->addLayout(row({specRef_, specStart_, specStop_}));
    specDbDiv_ = new QComboBox;
    for (int d : {1, 2, 5, 10, 20}) specDbDiv_->addItem(QStringLiteral("%1 dB/div").arg(d), d);
    connect(specDbDiv_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.spectrum.dbPerDiv = specDbDiv_->itemData(i).toDouble(); });
    });
    c->body()->addWidget(labeled(tr("Vertical"), specDbDiv_));
    specAvg_ = new QSpinBox;
    specAvg_->setRange(1, 256);
    connect(specAvg_, &QSpinBox::valueChanged, this, [this](int n) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.spectrum.averages = n; });
    });
    c->body()->addWidget(labeled(tr("Averages"), specAvg_));
    specGram_ = new QCheckBox(tr("Real-time spectrogram"));
    connect(specGram_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.spectrum.spectrogram = on; });
    });
    c->body()->addWidget(specGram_);
    auto* full = new QPushButton(tr("Full span (0 - Nyquist)"));
    connect(full, &QPushButton::clicked, this, [this] {
        edit([&](ScopeSettings& s) { s.spectrum.fStart = 0; s.spectrum.fStop = 0; });
    });
    c->body()->addWidget(full);
    specInfo_ = dimLabel("");
    specInfo_->setFont(monoFont(9));
    c->body()->addWidget(specInfo_);
    v->addWidget(c);
    v->addWidget(dimLabel(tr("Single-sided amplitude spectrum, window coherent gain corrected: a sine of "
                             "amplitude A reads A/\xE2\x88\x9A" "2 rms. 0 dBV = 1 Vrms, 0 dBFS = full-scale sine. "
                             "Bin spacing \xCE\x94" "f = fs/N, RBW = ENBW\xC2\xB7\xCE\x94" "f, axis 0..fs/2. Peak frequency by "
                             "main-lobe power centroid, tone level by main-lobe power sum / ENBW (no scalloping loss). "
                             "Mouse: wheel = zoom, drag = pan, Shift+drag = ref level, double-click = full span.")));
    v->addStretch();
    return w;
}

// =========================================================================== DISP tab
QWidget* MainWindow::buildDisplayTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    auto* d = new Card("DISPLAY");
    persist_ = new Segmented({"OFF", "SHORT", "LONG", "INFINITE"});
    connect(persist_, &Segmented::selected, this, [this](int i) {
        edit([&](ScopeSettings& s) { s.display.persistence = static_cast<Persistence>(i); });
    });
    d->body()->addWidget(dimLabel(tr("Persistence")));
    d->body()->addWidget(persist_);
    interp_ = new Segmented({"LINEAR", "SIN(x)/x"});
    connect(interp_, &Segmented::selected, this, [this](int i) {
        edit([&](ScopeSettings& s) { s.display.interpolation = static_cast<Interpolation>(i); });
    });
    d->body()->addWidget(dimLabel(tr("Interpolation (sparse samples)")));
    d->body()->addWidget(interp_);
    glow_ = new QCheckBox(tr("Trace glow"));
    connect(glow_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.display.glow = on; });
    });
    d->body()->addWidget(glow_);
    gridInt_ = new QSlider(Qt::Horizontal);
    gridInt_->setRange(0, 100);
    connect(gridInt_, &QSlider::valueChanged, this, [this](int x) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.display.gridIntensity = x / 100.0; });
    });
    d->body()->addWidget(labeled(tr("Grid"), gridInt_));
    lineWidth_ = new QSlider(Qt::Horizontal);
    lineWidth_->setRange(5, 40);
    connect(lineWidth_, &QSlider::valueChanged, this, [this](int x) {
        if (!syncing_) edit([&](ScopeSettings& s) { for (auto& c : s.ch) c.lineWidth = x / 10.0; });
    });
    d->body()->addWidget(labeled(tr("Line width"), lineWidth_));
    auto* colors = new QHBoxLayout;
    for (int i = 0; i < kNumTraces; ++i) {
        auto* lab = new QLabel(i < kNumChannels ? QString::number(i + 1) : QStringLiteral("M"));
        colorBtns_[i] = new ColorButton;
        connect(colorBtns_[i], &ColorButton::colorPicked, this, [this, i](const QColor& c) {
            edit([&](ScopeSettings& s) {
                if (i < kNumChannels) s.ch[i].color = fromQColor(c);
                else s.math.color = fromQColor(c);
            });
        });
        colors->addWidget(lab);
        colors->addWidget(colorBtns_[i]);
    }
    colors->addStretch();
    d->body()->addWidget(dimLabel(tr("Trace colors")));
    d->body()->addLayout(colors);
    showSpec_ = new QCheckBox(tr("Show spectrum panel"));
    showMeas_ = new QCheckBox(tr("Show measurement panel"));
    connect(showSpec_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.display.showSpectrum = on; });
    });
    connect(showMeas_, &QCheckBox::toggled, this, [this](bool on) {
        if (!syncing_) edit([&](ScopeSettings& s) { s.display.showMeasurements = on; });
    });
    d->body()->addWidget(showSpec_);
    d->body()->addWidget(showMeas_);
    v->addWidget(d);

    auto* cc = new Card("CURSORS", theme::amber);
    cursorMode_ = new Segmented({"OFF", "MANUAL", "TRACK"});
    cursorMode_->setAccent(theme::amber);
    connect(cursorMode_, &Segmented::selected, this, [this](int i) {
        edit([&](ScopeSettings& s) {
            s.display.cursorMode = static_cast<CursorMode>(i);
            if (i != 0) { // place cursors on screen
                const double w = 10 * s.timebase.secPerDiv;
                const double t0 = -s.timebase.triggerPos / 100.0 * w + s.timebase.delay;
                if (s.display.cursorX1 < t0 || s.display.cursorX1 > t0 + w) s.display.cursorX1 = t0 + 0.3 * w;
                if (s.display.cursorX2 < t0 || s.display.cursorX2 > t0 + w) s.display.cursorX2 = t0 + 0.7 * w;
            }
        });
    });
    cc->body()->addWidget(cursorMode_);
    cursorSrc_ = new QComboBox;
    for (auto* n : kTraceNames) cursorSrc_->addItem(n);
    connect(cursorSrc_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!syncing_ && i >= 0) edit([&](ScopeSettings& s) { s.display.cursorSource = i; });
    });
    cc->body()->addWidget(labeled(tr("Source"), cursorSrc_));
    cc->body()->addWidget(dimLabel(tr("Drag the X1/X2 (time) and Y1/Y2 (level) cursors on the screen. TRACK "
                                      "places Y on the waveform at X1/X2. Right-drag on the screen = zoom box, "
                                      "left-drag = pan.")));
    v->addWidget(cc);
    v->addStretch();
    return w;
}

// =========================================================================== INPUT tab
QWidget* MainWindow::buildInputTab() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);
    auto* sc = new Card("SIGNAL SOURCE");
    source_ = new Segmented({"GEN", "FILE", "AUDIO", "SERIAL"});
    connect(source_, &Segmented::selected, this, [this](int i) { setSource(static_cast<SourceKind>(i)); });
    sc->body()->addWidget(source_);
    v->addWidget(sc);

    auto* fc = new Card("FILE  (CSV / WAV)");
    auto* open = new QPushButton(tr("Open CSV / WAV file..."));
    connect(open, &QPushButton::clicked, this, [this] { openSignalFile(); });
    fc->body()->addWidget(open);
    fileInfo_ = dimLabel(tr("No file loaded"));
    fileInfo_->setFont(monoFont(9));
    fc->body()->addWidget(fileInfo_);
    fc->body()->addWidget(dimLabel(tr("The file is played back in a loop at its own sample rate. Channels select "
                                      "file columns in the CH tab. CSV: optional time column (t / time), units in "
                                      "the header like \"CH1 (V)\". WAV without a SIGNAL LAB voltage tag is shown in FS.")));
    v->addWidget(fc);

    auto* ac = new Card("AUDIO INPUT (MICROPHONE / LINE)");
    audioDev_ = new QComboBox;
    audioDev_->addItems(AudioInput::deviceNames());
    if (!AudioInput::available()) audioDev_->addItem(tr("(Qt Multimedia not available)"));
    ac->body()->addWidget(audioDev_);
    audioBtn_ = new QPushButton(tr("Start audio input"));
    audioBtn_->setEnabled(AudioInput::available());
    connect(audioBtn_, &QPushButton::clicked, this, &MainWindow::toggleAudio);
    ac->body()->addWidget(audioBtn_);
    audioInfo_ = dimLabel(tr("stopped"));
    ac->body()->addWidget(audioInfo_);
    auto* warn = new QLabel(tr("Audio samples are displayed in FS (full scale, -1..+1), NOT volts. A microphone "
                               "path has no voltage calibration (capsule, preamp gain, AGC). To measure real "
                               "circuit voltages you need a calibrated ADC front end."));
    warn->setObjectName("warn");
    warn->setWordWrap(true);
    ac->body()->addWidget(warn);
    v->addWidget(ac);

    auto* serial = new Card("USB / SERIAL ADC (PLANNED)");
    serial->body()->addWidget(dimLabel(tr("Not implemented yet. The engine already accepts real-time push sources "
                                          "(PushSource: interleaved float frames + sample rate), which is the "
                                          "interface a serial ADC driver will feed. See README > Hardware input.")));
    v->addWidget(serial);
    v->addStretch();
    return w;
}

// =========================================================================== measurements
QWidget* MainWindow::buildMeasurementPanel() {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(4);
    auto* head = new QLabel("AUTOMATIC MEASUREMENTS");
    head->setObjectName("section");
    v->addWidget(head);
    meas_ = new QTableWidget(0, static_cast<int>(std::size(kTableMeas)));
    QStringList hdr;
    for (auto m : kTableMeas) hdr << measName(m);
    meas_->setHorizontalHeaderLabels(hdr);
    meas_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    meas_->verticalHeader()->setDefaultSectionSize(22);
    meas_->verticalHeader()->setFixedWidth(54);
    meas_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    meas_->setSelectionMode(QAbstractItemView::NoSelection);
    meas_->setFocusPolicy(Qt::NoFocus);
    meas_->setFont(monoFont(9.5));
    v->addWidget(meas_);
    w->setMinimumHeight(60);
    return w;
}

void MainWindow::updateMeasurements(const Frame& f) {
    std::vector<int> rows;
    for (int i = 0; i < kNumTraces; ++i)
        if (f.tr[i].present) rows.push_back(i);
    if (meas_->rowCount() != static_cast<int>(rows.size())) {
        meas_->setRowCount(static_cast<int>(rows.size()));
        // shrink the panel to its content so the free space goes to the displays
        const int h = meas_->horizontalHeader()->height() + std::max<int>(1, static_cast<int>(rows.size())) * 22 + 6;
        meas_->setFixedHeight(h);
        measPanel_->setMaximumHeight(h + 26);
    }
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        const int i = rows[r];
        const auto& tr = f.tr[i];
        const QColor c = toQColor(i < kNumChannels ? s_.ch[i].color : s_.math.color);
        auto* hi = meas_->verticalHeaderItem(r);
        if (!hi) {
            hi = new QTableWidgetItem;
            meas_->setVerticalHeaderItem(r, hi);
        }
        hi->setText(kTraceNames[i]);
        hi->setForeground(c);
        for (int col = 0; col < static_cast<int>(std::size(kTableMeas)); ++col) {
            const MeasId id = kTableMeas[col];
            const auto& mv = tr.meas[id];
            auto* it = meas_->item(r, col);
            if (!it) {
                it = new QTableWidgetItem;
                it->setTextAlignment(Qt::AlignCenter);
                meas_->setItem(r, col, it);
            }
            const std::string unit = measUnit(id, tr.unit);
            if (mv.valid) {
                double val = mv.v;
                // values below the float resolution of the signal are shown as 0 (e.g. mean of a symmetric wave)
                const double scale = std::max({std::fabs(tr.meas[MeasId::Max].v), std::fabs(tr.meas[MeasId::Min].v), 1e-30});
                if (unit == tr.unit && std::fabs(val) < 1e-6 * scale) val = 0.0;
                it->setText((mv.upperBound ? "< " : "") + fmt(val, unit.c_str(), 4));
                it->setForeground(c.lighter(105));
            } else {
                it->setText("N/A");
                it->setForeground(QColor(0x5d, 0x66, 0x72));
            }
            if (col == 0)
                it->setToolTip(QStringLiteral("Top %1   Base %2   Amplitude %3\nAC RMS %4   rising edges %5   falling edges %6\n%7 samples on screen")
                                   .arg(fmt(tr.meas[MeasId::Top].v, tr.unit.c_str()), fmt(tr.meas[MeasId::Base].v, tr.unit.c_str()),
                                        fmt(tr.meas[MeasId::Amplitude].v, tr.unit.c_str()), fmt(tr.meas[MeasId::AcRms].v, tr.unit.c_str()))
                                   .arg(tr.meas.risingEdges)
                                   .arg(tr.meas.fallingEdges)
                                   .arg(tr.meas.samples));
        }
    }
}

void MainWindow::updateMathInfo(const Frame& f) {
    if (!mathInfo_) return;
    const auto& mt = f.tr[kMathTrace];
    if (!mt.present) return;
    mathInfo_->setText(qs(mt.info) + (mt.warning ? tr("\n\xE2\x9A\xA0 cut-off at/above Nyquist was clamped") : QString()));
    mathScale_->update();
}

// =========================================================================== menus
void MainWindow::buildMenus() {
    auto* file = menuBar()->addMenu(tr("&File"));
    auto add = [this](QMenu* m, const QString& t, const QKeySequence& k, auto fn) {
        auto* a = m->addAction(t);
        if (!k.isEmpty()) a->setShortcut(k);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    add(file, tr("Open Signal File (CSV/WAV)..."), QKeySequence::Open, [this] { openSignalFile(); });
    file->addSeparator();
    add(file, tr("Save Waveform as CSV..."), QKeySequence(Qt::CTRL | Qt::Key_E), [this] { saveWaveformCsv(); });
    add(file, tr("Save Waveform as WAV..."), QKeySequence(), [this] { saveWaveformWav(); });
    add(file, tr("Save Screen Image (PNG)..."), QKeySequence(Qt::CTRL | Qt::Key_P), [this] { saveImage(); });
    add(file, tr("Export Measurements (CSV)..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { exportMeasurements(); });
    file->addSeparator();
    add(file, tr("Load Setup..."), QKeySequence(Qt::CTRL | Qt::Key_L), [this] { loadSetup(); });
    add(file, tr("Save Setup..."), QKeySequence::Save, [this] { saveSetup(); });
    add(file, tr("Default Setup"), QKeySequence(), [this] { defaultSetup(); });
    file->addSeparator();
    add(file, tr("Quit"), QKeySequence::Quit, [this] { close(); });

    auto* acq = menuBar()->addMenu(tr("&Acquire"));
    add(acq, tr("Run / Stop"), QKeySequence(Qt::Key_Space), [this] { runStop(); });
    add(acq, tr("Single"), QKeySequence(Qt::Key_S), [this] { single(); });
    add(acq, tr("Auto Scale"), QKeySequence(Qt::Key_A), [this] { autoscale(); });
    add(acq, tr("Force Trigger"), QKeySequence(Qt::Key_F), [this] { engine_->forceTrigger(); });
    add(acq, tr("Clear Spectrum Averaging"), QKeySequence(), [this] { engine_->resetSpectrum(); });

    auto* view = menuBar()->addMenu(tr("&View"));
    add(view, tr("Full Screen"), QKeySequence::FullScreen, [this] { toggleFullScreen(); });
    add(view, tr("Full Screen (F11)"), QKeySequence(Qt::Key_F11), [this] { toggleFullScreen(); });
    add(view, tr("Show / Hide Control Panel"), QKeySequence(Qt::CTRL | Qt::Key_1),
        [this] { edit([](ScopeSettings& s) { s.display.showPanel = !s.display.showPanel; }); });
    add(view, tr("Show / Hide Spectrum"), QKeySequence(Qt::CTRL | Qt::Key_2),
        [this] { edit([](ScopeSettings& s) { s.display.showSpectrum = !s.display.showSpectrum; }); });
    add(view, tr("Show / Hide Measurements"), QKeySequence(Qt::CTRL | Qt::Key_3),
        [this] { edit([](ScopeSettings& s) { s.display.showMeasurements = !s.display.showMeasurements; }); });

    auto* help = menuBar()->addMenu(tr("&Help"));
    add(help, tr("Quick Guide"), QKeySequence::HelpContents, [this] { showGuide(); });
    add(help, tr("About SIGNAL LAB PRO"), QKeySequence(), [this] { about(); });
}

// =========================================================================== state
void MainWindow::edit(const std::function<void(ScopeSettings&)>& fn) {
    fn(s_);
    syncControls();
    pushSettings();
}

void MainWindow::adopt(const ScopeSettings& s) {
    s_ = s;
    syncControls();
    pushSettings();
}

void MainWindow::pushSettings() {
    engine_->setSettings(s_);
    scope_->setSettings(s_);
    spectrum_->setSettings(s_);
    applyLayoutVisibility();
    saveTimer_->start(1000);
}

void MainWindow::applyLayoutVisibility() {
    spectrum_->setVisible(s_.display.showSpectrum);
    measPanel_->setVisible(s_.display.showMeasurements);
    tabs_->setVisible(s_.display.showPanel);
}

void MainWindow::refreshInputLists() {
    const bool was = syncing_;
    syncing_ = true;
    QStringList items;
    switch (s_.acq.source) {
    case SourceKind::Generator:
        for (int g = 0; g < kNumGenerators; ++g) items << QStringLiteral("GEN %1").arg(g + 1);
        break;
    case SourceKind::File:
        if (file_)
            for (size_t i = 0; i < file_->names.size(); ++i)
                items << QStringLiteral("%1 (%2)").arg(qs(file_->names[i]), qs(file_->units[i]));
        if (items.isEmpty()) items << tr("(no file)");
        break;
    case SourceKind::Audio: items << tr("Audio In 1 (L)") << tr("Audio In 2 (R)"); break;
    case SourceKind::Serial: for (int i = 0; i < 4; ++i) items << QStringLiteral("ADC %1").arg(i + 1); break;
    }
    for (int c = 0; c < kNumChannels; ++c) {
        auto* cb = chUi_[c].input;
        QStringList cur;
        for (int i = 0; i < cb->count(); ++i) cur << cb->itemText(i);
        if (cur != items) {
            cb->clear();
            cb->addItems(items);
        }
        cb->setCurrentIndex(std::clamp(s_.ch[c].input, 0, static_cast<int>(items.size()) - 1));
    }
    syncing_ = was;
}

void MainWindow::syncControls() {
    syncing_ = true;
    // channels
    for (int c = 0; c < kNumChannels; ++c) {
        auto& u = chUi_[c];
        const auto& ch = s_.ch[c];
        const QColor col = toQColor(ch.color);
        u.card->setAccent(col);
        u.enable->setChecked(ch.enabled);
        u.enable->setText(ch.enabled ? "ON" : "OFF");
        u.enable->setStyleSheet(ch.enabled ? QStringLiteral("QPushButton { background: rgba(%1,%2,%3,60); border-color: %4; color: white; font-weight:700; }")
                                                 .arg(col.red()).arg(col.green()).arg(col.blue()).arg(col.name())
                                           : QString());
        u.vdiv->setAccent(col);
        u.pos->setAccent(col);
        u.vdiv->setValue(ch.voltsPerDiv);
        u.pos->setValue(ch.position);
        u.coupling->setCurrent(static_cast<int>(ch.coupling));
        u.coupling->setAccent(col);
        u.invert->setChecked(ch.invert);
    }
    refreshInputLists();
    // horizontal / trigger
    tdiv_->setValue(s_.timebase.secPerDiv);
    delay_->setValue(s_.timebase.delay);
    trigPos_->setValue(s_.timebase.triggerPos);
    autoRoll_->setChecked(s_.timebase.autoRoll);
    refresh_->setCurrentIndex(std::max(0, refresh_->findData(s_.acq.refreshRate)));
    trigMode_->setCurrent(static_cast<int>(s_.trigger.mode));
    slope_->setCurrent(static_cast<int>(s_.trigger.slope));
    trigSrc_->setCurrentIndex(std::clamp(s_.trigger.source, 0, 3));
    level_->setValue(s_.trigger.level);
    level_->setAccent(toQColor(s_.ch[std::clamp(s_.trigger.source, 0, 3)].color));
    holdoff_->setValue(s_.trigger.holdoff);
    noiseRej_->setChecked(s_.trigger.hysteresisDiv >= 0.3);
    // generator
    const auto& g = s_.gen[genSel_];
    genSelect_->setCurrent(genSel_);
    genOutput_->setChecked(g.output);
    genOutput_->setText(g.output ? "OUTPUT ON" : "OUTPUT OFF");
    QStringList routed;
    for (int c = 0; c < kNumChannels; ++c)
        if (s_.ch[c].input == genSel_) routed << QStringLiteral("CH%1").arg(c + 1);
    genRoute_->setText(s_.acq.source != SourceKind::Generator ? tr("source is not GEN (see INPUT)")
                       : routed.isEmpty() ? tr("not connected to a channel")
                                          : tr("connected to ") + routed.join(", "));
    if (auto* b = waveGroup_->button(static_cast<int>(g.shape))) b->setChecked(true);
    gFreq_->setValue(g.frequency);
    gAmp_->setValue(g.vpp);
    gOff_->setValue(g.offset);
    gPhase_->setValue(g.phaseDeg);
    gDuty_->setValue(g.duty);
    gEdge_->setValue(g.edgeTime);
    gNoise_->setValue(g.noiseRms);
    const bool hasDuty = g.shape == Waveform::Square || g.shape == Waveform::Pulse;
    gDuty_->setEnabled(hasDuty);
    gEdge_->setEnabled(hasDuty);
    gFreq_->setEnabled(g.shape != Waveform::DC && g.shape != Waveform::Noise);
    gAmp_->setEnabled(g.shape != Waveform::DC && g.shape != Waveform::Custom);
    customBtn_->setEnabled(g.shape == Waveform::Custom);
    QString warn;
    if (g.shape != Waveform::DC && g.shape != Waveform::Noise && g.frequency > s_.acq.sampleRate / 2)
        warn = tr("\xE2\x9A\xA0 Frequency above Nyquist (fs/2 = %1): the sampled signal aliases.").arg(fmt(s_.acq.sampleRate / 2, "Hz"));
    else if (g.shape != Waveform::DC && g.shape != Waveform::Noise && g.frequency > s_.acq.sampleRate / 10)
        warn = tr("Less than 10 samples per period: use SIN(x)/x display or raise the sample rate.");
    genWarn_->setText(warn);
    int sr = sampleRate_->findData(s_.acq.sampleRate);
    if (sr < 0) {
        sampleRate_->addItem(fmt(s_.acq.sampleRate, "Sa/s", 5), s_.acq.sampleRate);
        sr = sampleRate_->count() - 1;
    }
    sampleRate_->setCurrentIndex(sr);
    // math
    const auto& m = s_.math;
    mathEnable_->setChecked(m.enabled);
    mathEnable_->setText(m.enabled ? "MATH ON" : "MATH OFF");
    mathOp_->setCurrentIndex(static_cast<int>(m.op));
    mathA_->setCurrentIndex(m.srcA);
    mathB_->setCurrentIndex(m.srcB);
    mathOrder_->setValue(m.order);
    mathMa_->setValue(m.maLength);
    mathFc_->setValue(m.cutoff);
    mathFc2_->setValue(m.cutoffHigh);
    mathFc_->setTitle(m.op == MathOp::BandPass ? "Lower edge" : "Cut-off");
    mathScale_->setValue(m.unitsPerDiv);
    mathPos_->setValue(m.position);
    const QColor mc = toQColor(m.color);
    for (auto* k : {mathFc_, mathFc2_, mathScale_, mathPos_}) k->setAccent(mc);
    const bool binary = m.op == MathOp::Add || m.op == MathOp::Subtract || m.op == MathOp::Multiply;
    const bool filt = m.op == MathOp::LowPass || m.op == MathOp::HighPass || m.op == MathOp::BandPass;
    mathBRow_->setVisible(binary);
    mathFilterBox_->setVisible(filt);
    mathFc2_->setVisible(m.op == MathOp::BandPass);
    mathMaRow_->setVisible(m.op == MathOp::MovingAverage);
    if (!m.enabled) mathInfo_->setText(tr("MATH is off"));
    // spectrum
    const auto& sp = s_.spectrum;
    specEnable_->setChecked(sp.enabled);
    specEnable_->setText(sp.enabled ? "FFT ON" : "FFT OFF");
    specSrc_->setCurrentIndex(sp.source);
    specSize_->setCurrentIndex(std::max(0, specSize_->findData(sp.fftSize)));
    specWin_->setCurrentIndex(static_cast<int>(sp.window));
    specScale_->setCurrent(sp.logScale ? 0 : 1);
    specRef_->setValue(sp.refLevel);
    specStart_->setValue(sp.fStart);
    specStop_->setValue(sp.fStop);
    specDbDiv_->setCurrentIndex(std::max(0, specDbDiv_->findData(static_cast<int>(sp.dbPerDiv))));
    specAvg_->setValue(sp.averages);
    specGram_->setChecked(sp.spectrogram);
    // display
    const auto& d = s_.display;
    persist_->setCurrent(static_cast<int>(d.persistence));
    interp_->setCurrent(static_cast<int>(d.interpolation));
    glow_->setChecked(d.glow);
    gridInt_->setValue(static_cast<int>(std::lround(d.gridIntensity * 100)));
    lineWidth_->setValue(static_cast<int>(std::lround(s_.ch[0].lineWidth * 10)));
    for (int i = 0; i < kNumTraces; ++i) colorBtns_[i]->setColor(toQColor(i < kNumChannels ? s_.ch[i].color : s_.math.color));
    cursorMode_->setCurrent(static_cast<int>(d.cursorMode));
    cursorSrc_->setCurrentIndex(d.cursorSource);
    showSpec_->setChecked(d.showSpectrum);
    showMeas_->setChecked(d.showMeasurements);
    // input
    source_->setCurrent(static_cast<int>(s_.acq.source));
    syncing_ = false;
}

// =========================================================================== refresh loop
void MainWindow::refresh() {
    const EngineStatus st = engine_->status();
    running_ = st.running;
    updateHeader(st);
    scope_->setStatus(st);
    if (auto f = engine_->latestFrame(); f && f.get() != lastFramePtr_) {
        lastFramePtr_ = f.get();
        scope_->setFrame(f);
        const qint64 now = clock_.elapsed();
        if (now - lastMeasUpdate_ >= 120) {
            lastMeasUpdate_ = now;
            updateMeasurements(*f);
            updateMathInfo(*f);
        }
    }
    if (auto sp = engine_->latestSpectrum(); sp && sp.get() != lastSpecPtr_) {
        lastSpecPtr_ = sp.get();
        spectrum_->setSpectrum(sp);
        if (sp->valid)
            specInfo_->setText(QString::fromUtf8("fs %1   \xCE\x94" "f %2\nRBW %3   ENBW %4 bins")
                                   .arg(fmt(sp->fs, "Sa/s", 4), fmt(sp->binHz, "Hz", 4), fmt(sp->binHz * sp->enbwBins, "Hz", 4))
                                   .arg(sp->enbwBins, 0, 'f', 3));
    }
    if (auto r = engine_->takeAutoscaleResult()) {
        adopt(*r);
        engine_->run();
        statusBar()->showMessage(tr("Auto scale applied"), 3000);
    }
}

void MainWindow::updateHeader(const EngineStatus& st) {
    QColor c = theme::green;
    switch (st.state) {
    case TrigState::Stopped: c = theme::red; break;
    case TrigState::Auto: c = QColor(0xFF, 0xD6, 0x0A); break;
    case TrigState::Waiting:
    case TrigState::Armed: c = theme::amber; break;
    case TrigState::Roll: c = QColor(0x22, 0xD3, 0xEE); break;
    default: break;
    }
    const QString pillCss = QStringLiteral("QLabel { color:%1; background: rgba(%2,%3,%4,40); border:1px solid %1; border-radius:6px; }")
                                .arg(c.name()).arg(c.red()).arg(c.green()).arg(c.blue());
    if (statePill_->text() != trigStateLabel(st.state)) {
        statePill_->setText(trigStateLabel(st.state));
        statePill_->setStyleSheet(pillCss);
    }
    const QString runCss = st.running
        ? QStringLiteral("QPushButton { background:#123d24; border:1px solid #3ddc84; color:#d8ffe7; font-weight:800; }")
        : QStringLiteral("QPushButton { background:#4a1518; border:1px solid #ff4d4f; color:#ffe1e1; font-weight:800; }");
    if (runBtn_->property("css").toString() != runCss) {
        runBtn_->setStyleSheet(runCss);
        runBtn_->setProperty("css", runCss);
        runBtn_->setText(st.running ? "\xE2\x96\xB6  RUN" : "\xE2\x96\xA0  STOP");
    }
    const bool armed = st.state == TrigState::Armed;
    const QString sCss = armed ? QStringLiteral("QPushButton { background:#4a3510; border:1px solid #ffb020; color:#fff1d6; font-weight:800; }")
                               : QString();
    if (singleBtn_->property("css").toString() != sCss) {
        singleBtn_->setStyleSheet(sCss);
        singleBtn_->setProperty("css", sCss);
    }
    QString rate = fmt(st.fs, "Sa/s", 4);
    if (st.decim > 1) rate += QString::fromUtf8(" (\xC3\xB7%1)").arg(st.decim);
    rateLabel_->setText(rate);
    fpsLabel_->setText(QStringLiteral("%1 wfm/s").arg(st.frameRate, 0, 'f', 0));
    QString src = sourceName(s_.acq.source);
    if (s_.acq.source == SourceKind::File && file_) src += ": " + QFileInfo(qs(file_->sourcePath)).fileName();
    if (st.segmented) src += "  \xC2\xB7 SEG";
    srcLabel_->setText(src);
    if (acqInfo_)
        acqInfo_->setText(QString::fromUtf8("Native %1   Effective %2\nDecimation \xC3\x97%3   Acquired %4 Sa\nMode %5")
                              .arg(fmt(st.nativeRate, "Sa/s", 4), fmt(st.fs, "Sa/s", 4))
                              .arg(st.decim)
                              .arg(static_cast<qulonglong>(st.acquired))
                              .arg(st.segmented ? tr("segmented (dead time between records)") : tr("continuous streaming")));
}

// =========================================================================== actions
void MainWindow::runStop() {
    if (running_) engine_->stop();
    else engine_->run();
    running_ = !running_;
}

void MainWindow::single() {
    if (s_.trigger.mode != TriggerMode::Single) edit([](ScopeSettings& s) { s.trigger.mode = TriggerMode::Single; });
    engine_->single();
}

void MainWindow::autoscale() {
    engine_->requestAutoscale();
    statusBar()->showMessage(tr("Auto scale..."), 1500);
}

void MainWindow::setTriggerMode(TriggerMode m) {
    edit([m](ScopeSettings& s) { s.trigger.mode = m; });
    if (m == TriggerMode::Single) engine_->single();
    else engine_->run();
}

void MainWindow::setSource(SourceKind k) {
    if (k == SourceKind::File && !file_) {
        openSignalFile();
        if (!file_) { syncControls(); return; }
    }
    edit([k](ScopeSettings& s) {
        s.acq.source = k;
        for (auto& c : s.ch) c.input = std::clamp(c.input, 0, 3);
    });
    if (k == SourceKind::Audio && !audio_->running()) statusBar()->showMessage(tr("Press 'Start audio input' to capture"), 5000);
    engine_->run();
}

bool MainWindow::loadSignalFile(const QString& path, QString* error) {
    auto d = std::make_shared<SignalData>();
    std::string err;
    if (!readSignalFile(path.toStdString(), *d, err, s_.acq.sampleRate)) {
        if (error) *error = qs(err);
        return false;
    }
    file_ = d;
    engine_->setFileData(file_);
    s_.filePath = path.toStdString();
    QString units;
    for (size_t i = 0; i < d->units.size(); ++i) units += (i ? ", " : "") + qs(d->names[i]) + " [" + qs(d->units[i]) + "]";
    fileInfo_->setText(QString::fromUtf8("%1\n%2  \xC2\xB7  %3 samples  \xC2\xB7  %4\n%5\n%6")
                           .arg(QFileInfo(path).fileName(), fmt(d->sampleRate, "Sa/s", 5))
                           .arg(static_cast<qulonglong>(d->length()))
                           .arg(fmt(d->length() / d->sampleRate, "s", 4), units, qs(d->note)));
    return true;
}

void MainWindow::openSignalFile(QString path) {
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(this, tr("Open signal file"), QString(), tr("Signal files (*.csv *.txt *.wav);;All files (*)"));
    if (path.isEmpty()) return;
    QString err;
    if (!loadSignalFile(path, &err)) {
        QMessageBox::warning(this, tr("Open file"), err);
        return;
    }
    edit([](ScopeSettings& s) {
        s.acq.source = SourceKind::File;
        for (int c = 0; c < kNumChannels; ++c) s.ch[c].input = c;
    });
    for (int c = 0; c < kNumChannels; ++c)
        if (c >= static_cast<int>(file_->channels.size()) && s_.ch[c].enabled) edit([c](ScopeSettings& s) { s.ch[c].enabled = false; });
    engine_->run();
    autoscale();
    statusBar()->showMessage(tr("Loaded ") + path + (file_->note.empty() ? QString() : "  -  " + qs(file_->note)), 6000);
}

void MainWindow::saveWaveformCsv() {
    auto f = scope_->frame();
    if (!f) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save waveform"), "waveform.csv", tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    std::vector<ExportColumn> cols;
    size_t n = SIZE_MAX;
    double t0 = 0, dt = 0;
    for (int i = 0; i < kNumTraces; ++i) {
        const auto& tr = f->tr[i];
        if (!tr.present) continue;
        cols.push_back({kTraceNames[i], tr.unit, tr.y.data()});
        n = std::min(n, tr.y.size());
        t0 = tr.t0;
        dt = tr.dt;
    }
    if (cols.empty()) return;
    std::vector<std::string> comments = {
        "SIGNAL LAB PRO waveform export",
        "date=" + QDateTime::currentDateTime().toString(Qt::ISODate).toStdString(),
        "source=" + sourceName(s_.acq.source).toStdString(),
        "time is relative to the trigger point; state=" + std::string(trigStateLabel(f->state)),
        "timebase_s_per_div=" + std::to_string(f->settings.timebase.secPerDiv),
        "trigger=CH" + std::to_string(f->settings.trigger.source + 1) + " level " + std::to_string(f->settings.trigger.level)};
    std::string err;
    if (!writeCsv(path.toStdString(), cols, n, t0, dt, comments, err)) QMessageBox::warning(this, tr("Save"), qs(err));
    else statusBar()->showMessage(tr("Saved %1 samples to %2").arg(n).arg(path), 5000);
}

void MainWindow::saveWaveformWav() {
    auto f = scope_->frame();
    if (!f) return;
    std::vector<ExportColumn> cols;
    size_t n = SIZE_MAX;
    for (int i = 0; i < kNumTraces && cols.size() < 4; ++i) {
        const auto& tr = f->tr[i];
        if (!tr.present) continue;
        cols.push_back({kTraceNames[i], tr.unit, tr.y.data()});
        n = std::min(n, tr.y.size());
    }
    if (cols.empty()) return;
    const double rate = 1.0 / f->tr[0].dt > 0 ? f->fs : f->fs;
    if (std::fabs(rate - std::round(rate)) > 1e-6 * rate) {
        QMessageBox::warning(this, tr("WAV export"), tr("The effective sample rate %1 is not an integer number of Hz and "
                                                         "cannot be stored exactly in a WAV header. Use CSV instead.")
                                                          .arg(fmt(rate, "Sa/s", 6)));
        return;
    }
    bool ok = false;
    const QString fmtSel = QInputDialog::getItem(this, tr("WAV format"), tr("Sample format"),
                                                 {tr("32-bit float (exact)"), tr("16-bit PCM")}, 0, false, &ok);
    if (!ok) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save waveform"), "waveform.wav", tr("WAV (*.wav)"));
    if (path.isEmpty()) return;
    std::string err;
    double vfs = 0;
    if (!writeWav(path.toStdString(), cols, n, rate, fmtSel.startsWith("16") ? 16 : 32, 0.0, err, &vfs))
        QMessageBox::warning(this, tr("Save"), qs(err));
    else
        statusBar()->showMessage(tr("Saved WAV (%1 ch, full scale = %2) to %3").arg(cols.size()).arg(fmt(vfs, "V", 3), path), 6000);
}

void MainWindow::saveImage() {
    const QString def = QStringLiteral("signallab_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save screen image"), def, tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    const QPixmap pm = vsplit_->grab(); // device-pixel-ratio aware (Retina)
    if (!pm.save(path)) QMessageBox::warning(this, tr("Save"), tr("Could not write ") + path);
    else statusBar()->showMessage(tr("Image saved: ") + path, 5000);
}

void MainWindow::exportMeasurements() {
    auto f = scope_->frame();
    if (!f) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export measurements"), "measurements.csv", tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export"), out.errorString());
        return;
    }
    QTextStream ts(&out);
    ts << "# SIGNAL LAB PRO measurements, " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "# computed on the displayed record: " << f->tr[0].y.size() << " samples at " << QString::number(f->fs, 'g', 12)
       << " Sa/s, window " << QString::number(f->tEnd - f->tStart, 'g', 9) << " s\n";
    ts << "trace,measurement,value,unit,valid,upper_bound\n";
    for (int i = 0; i < kNumTraces; ++i) {
        const auto& tr = f->tr[i];
        if (!tr.present) continue;
        for (int m = 0; m < static_cast<int>(MeasId::Count); ++m) {
            const auto id = static_cast<MeasId>(m);
            const auto& v = tr.meas[id];
            ts << kTraceNames[i] << ',' << measName(id) << ',' << (v.valid ? QString::number(v.v, 'g', 10) : QString()) << ','
               << qs(measUnit(id, tr.unit)) << ',' << (v.valid ? 1 : 0) << ',' << (v.upperBound ? 1 : 0) << "\n";
        }
    }
    if (auto sp = engine_->latestSpectrum(); sp && sp->valid) {
        ts << "# spectrum: source " << kTraceNames[sp->source] << ", N=" << sp->fftSize << ", window " << windowName(sp->window) << "\n";
        for (const auto& p : sp->peaks)
            ts << "FFT,peak," << QString::number(p.freq, 'g', 10) << ",Hz,1,0\nFFT,peak_level," << QString::number(10 * std::log10(p.power), 'g', 6)
               << ",dB(" << qs(sp->unit) << "rms),1,0\n";
        if (sp->harmonics.valid) ts << "FFT,THD," << QString::number(100.0 * sp->harmonics.thdRatio, 'g', 6) << ",%,1,0\n";
    }
    statusBar()->showMessage(tr("Measurements exported: ") + path, 5000);
}

bool MainWindow::applySetupFile(const QString& path, QString* error) {
    ScopeSettings s;
    if (!loadSettingsFile(s, path, error)) return false;
    s_ = s;
    if (s_.acq.source == SourceKind::File && !s_.filePath.empty()) {
        QString e;
        if (!loadSignalFile(qs(s_.filePath), &e)) {
            s_.acq.source = SourceKind::Generator;
            if (error) *error = tr("setup loaded, but its file could not be opened: ") + e;
        }
    }
    return true;
}

void MainWindow::loadSetup() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load setup"), QString(), tr("Setup (*.json)"));
    if (path.isEmpty()) return;
    QString err;
    if (!applySetupFile(path, &err)) {
        QMessageBox::warning(this, tr("Load setup"), err);
        return;
    }
    adopt(s_);
    if (s_.trigger.mode == TriggerMode::Single) engine_->single();
    else engine_->run();
    statusBar()->showMessage(tr("Setup loaded: ") + path + (err.isEmpty() ? QString() : "  (" + err + ")"), 5000);
}

void MainWindow::saveSetup() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save setup"), "setup.json", tr("Setup (*.json)"));
    if (path.isEmpty()) return;
    QString err;
    if (!saveSettingsFile(s_, path, &err)) QMessageBox::warning(this, tr("Save setup"), err);
    else statusBar()->showMessage(tr("Setup saved: ") + path, 5000);
}

void MainWindow::defaultSetup() {
    adopt(ScopeSettings::defaults());
    engine_->run();
}

void MainWindow::saveSession() { saveSettingsFile(s_, opt_.settingsPath); }

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) showNormal();
    else showFullScreen();
}

void MainWindow::toggleAudio() {
    if (audio_->running()) {
        audio_->stop();
        return;
    }
    startAudio();
}

void MainWindow::startAudio() {
#if defined(SLP_HAVE_PERMISSIONS)
    QMicrophonePermission perm;
    switch (qApp->checkPermission(perm)) {
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(perm, this, [this](const QPermission& p) {
            if (p.status() == Qt::PermissionStatus::Granted) startAudio();
            else statusBar()->showMessage(tr("Microphone access denied"), 6000);
        });
        return;
    case Qt::PermissionStatus::Denied:
        QMessageBox::warning(this, tr("Microphone"), tr("Microphone access is denied. Enable it in System Settings > "
                                                        "Privacy & Security > Microphone."));
        return;
    case Qt::PermissionStatus::Granted: break;
    }
#endif
    QString err;
    if (!audio_->start(audioDev_->currentIndex(), &err)) {
        QMessageBox::warning(this, tr("Audio input"), err);
        return;
    }
    if (s_.acq.source != SourceKind::Audio) setSource(SourceKind::Audio);
    else engine_->run();
}

void MainWindow::editCustomWaveform() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Custom waveform - GEN %1").arg(genSel_ + 1));
    dlg.resize(560, 420);
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(dimLabel(tr("The output is offset + the sum of the partials below. Frequencies are multiples of the "
                             "generator frequency (%1). Partial shapes: Sine, Square, Triangle, Sawtooth.")
                              .arg(fmt(s_.gen[genSel_].frequency, "Hz"))));
    auto* table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({tr("Shape"), tr("Multiple of f"), tr("Vpp"), tr("Phase (\xC2\xB0)")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    auto addRow = [table](const HarmonicComponent& c) {
        const int r = table->rowCount();
        table->insertRow(r);
        auto* cb = new QComboBox;
        cb->addItems({"Sine", "Square", "Triangle", "Sawtooth"});
        cb->setCurrentIndex(std::clamp(static_cast<int>(c.shape), 0, 3));
        table->setCellWidget(r, 0, cb);
        auto mkSpin = [](double v, double lo, double hi, int dec) {
            auto* s = new QDoubleSpinBox;
            s->setRange(lo, hi);
            s->setDecimals(dec);
            s->setValue(v);
            return s;
        };
        table->setCellWidget(r, 1, mkSpin(c.multiple, 0.0, 10000.0, 4));
        table->setCellWidget(r, 2, mkSpin(c.vpp, 0.0, 100.0, 4));
        table->setCellWidget(r, 3, mkSpin(c.phaseDeg, -360.0, 360.0, 1));
    };
    for (const auto& c : s_.gen[genSel_].components) addRow(c);
    v->addWidget(table, 1);
    auto* presets = new QComboBox;
    presets->addItems({tr("Presets..."), tr("Square approximation (odd harmonics 1-9)"), tr("Sawtooth approximation (1-8)"),
                       tr("Fundamental + 10 % 3rd harmonic"), tr("Two-tone (f and 1.1 f)"),
                       tr("AM, m = 0.5 (carrier f, sidebands f \xC2\xB1 0.1 f)")});
    auto* add = new QPushButton(tr("Add partial"));
    auto* rem = new QPushButton(tr("Remove selected"));
    v->addLayout(row({presets, add, rem}));
    connect(add, &QPushButton::clicked, &dlg, [addRow] { addRow({Waveform::Sine, 2.0, 0.5, 0.0}); });
    connect(rem, &QPushButton::clicked, &dlg, [table] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    connect(presets, &QComboBox::currentIndexChanged, &dlg, [table, addRow, presets](int i) {
        if (i <= 0) return;
        table->setRowCount(0);
        const double pi = 3.14159265358979323846;
        switch (i) {
        case 1: for (int k = 1; k <= 9; k += 2) addRow({Waveform::Sine, double(k), 8.0 / (pi * k), 0.0}); break;
        case 2: for (int k = 1; k <= 8; ++k) addRow({Waveform::Sine, double(k), 4.0 / (pi * k), k % 2 ? 0.0 : 180.0}); break;
        case 3: addRow({Waveform::Sine, 1, 2.0, 0}); addRow({Waveform::Sine, 3, 0.2, 0}); break;
        case 4: addRow({Waveform::Sine, 1, 1.0, 0}); addRow({Waveform::Sine, 1.1, 1.0, 0}); break;
        case 5: addRow({Waveform::Sine, 1, 2.0, 0}); addRow({Waveform::Sine, 0.9, 0.5, 0}); addRow({Waveform::Sine, 1.1, 0.5, 0}); break;
        }
        presets->setCurrentIndex(0);
    });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<HarmonicComponent> comps;
    for (int r = 0; r < table->rowCount(); ++r) {
        HarmonicComponent c;
        c.shape = static_cast<Waveform>(qobject_cast<QComboBox*>(table->cellWidget(r, 0))->currentIndex());
        c.multiple = qobject_cast<QDoubleSpinBox*>(table->cellWidget(r, 1))->value();
        c.vpp = qobject_cast<QDoubleSpinBox*>(table->cellWidget(r, 2))->value();
        c.phaseDeg = qobject_cast<QDoubleSpinBox*>(table->cellWidget(r, 3))->value();
        comps.push_back(c);
    }
    edit([&](ScopeSettings& s) {
        s.gen[genSel_].components = comps;
        s.gen[genSel_].shape = Waveform::Custom;
    });
}

void MainWindow::showGuide() {
    QMessageBox::information(this, tr("Quick guide"),
        tr("<b>Keyboard</b><br>Space = Run/Stop &nbsp; S = Single &nbsp; A = Auto Scale &nbsp; F = Force trigger<br>"
           "Ctrl+1/2/3 = toggle panel / spectrum / measurements &nbsp; F11 = full screen<br><br>"
           "<b>Scope screen</b><br>Wheel = time/div &nbsp; Shift+wheel = V/div (selected trace) &nbsp; Alt+wheel = position<br>"
           "Left-drag = pan (delay) &nbsp; Right-drag = zoom box<br>Drag left markers = trace position, right marker = "
           "trigger level, top marker = trigger position<br>Click a channel badge to select it<br><br>"
           "<b>Spectrum</b><br>Wheel = zoom &nbsp; drag = pan &nbsp; Shift+drag = reference level &nbsp; double-click = full span<br><br>"
           "<b>Knobs</b><br>Wheel or drag (Shift = fine), double-click to type (1.5k, 200m, 10u), right-click = default"));
}

void MainWindow::about() {
    QMessageBox::about(this, tr("About SIGNAL LAB PRO"),
        tr("<h3>SIGNAL LAB PRO %1</h3>Digital oscilloscope &amp; signal analyzer<br>C++20 \xC2\xB7 Qt %2 \xC2\xB7 FFT: %3<br><br>"
           "4 channels, function generator, edge trigger (Auto/Normal/Single), automatic measurements, "
           "FFT spectrum &amp; spectrogram, digital filters, CSV/WAV I/O.")
            .arg(QApplication::applicationVersion(), QT_VERSION_STR, RealFFT::backendName()));
}

} // namespace slp
