// SIGNAL LAB PRO - GUI controller. Owns the instrument settings (single source of truth),
// pushes them to the acquisition engine and pulls frames / spectra at the display rate.
#pragma once

#include "Acquisition.h"
#include "AudioInput.h"
#include "Controls.h"
#include "ScopeSettings.h"
#include "ScopeView.h"
#include "SpectrumView.h"

#include <QElapsedTimer>
#include <QMainWindow>

#include <array>
#include <functional>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTimer;
class QToolButton;

namespace slp {

struct AppOptions {
    QString settingsPath;     // session file (auto saved / restored)
    QString loadSetupPath;    // apply a setup file after start
    QString dumpSettingsPath; // write the live settings at exit (tests)
    QString screenshotPath;   // save a window screenshot before quitting (docs/tests)
    QString openFile;         // CSV / WAV to load at start
    int quitAfterMs = 0;      // auto quit (tests)
    QSize size;               // initial window size
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const AppOptions& opt, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    struct ChannelUi {
        ui::Card* card = nullptr;
        QPushButton* enable = nullptr;
        ui::Knob* vdiv = nullptr;
        ui::Knob* pos = nullptr;
        ui::Segmented* coupling = nullptr;
        QCheckBox* invert = nullptr;
        QComboBox* input = nullptr;
    };

    // ---- construction
    QWidget* buildHeader();
    QWidget* buildChannelsTab();
    QWidget* buildHorizontalTab();
    QWidget* buildGeneratorTab();
    QWidget* buildMathTab();
    QWidget* buildSpectrumTab();
    QWidget* buildDisplayTab();
    QWidget* buildInputTab();
    QWidget* buildMeasurementPanel();
    void buildMenus();
    QWidget* scrollPage(QWidget* content);
    ui::Knob* knob(const QString& title, ui::Knob::Stepper st, ui::Knob::Formatter f, double lo, double hi, double def,
                   std::function<void(double)> onEdit);

    // ---- state flow
    void edit(const std::function<void(ScopeSettings&)>& fn);
    void adopt(const ScopeSettings& s);
    void pushSettings();
    void syncControls();
    void applyLayoutVisibility();
    void refreshInputLists();
    void refresh();
    void updateHeader(const EngineStatus& st);
    void updateMeasurements(const Frame& f);
    void updateMathInfo(const Frame& f);

    // ---- actions
    void runStop();
    void single();
    void autoscale();
    void setTriggerMode(TriggerMode m);
    void setSource(SourceKind k);
    void openSignalFile(QString path = {});
    bool loadSignalFile(const QString& path, QString* error);
    void saveWaveformCsv();
    void saveWaveformWav();
    void saveImage();
    void exportMeasurements();
    void loadSetup();
    void saveSetup();
    void defaultSetup();
    bool applySetupFile(const QString& path, QString* error);
    void toggleFullScreen();
    void editCustomWaveform();
    void toggleAudio();
    void startAudio();
    void saveSession();
    void showGuide();
    void about();

    AppOptions opt_;
    ScopeSettings s_;
    std::unique_ptr<AcquisitionEngine> engine_;
    std::shared_ptr<PushSource> push_;
    std::shared_ptr<const SignalData> file_;
    AudioInput* audio_ = nullptr;

    ScopeView* scope_ = nullptr;
    SpectrumView* spectrum_ = nullptr;
    QTableWidget* meas_ = nullptr;
    QWidget* measPanel_ = nullptr;
    QSplitter* vsplit_ = nullptr;
    QSplitter* hsplit_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QElapsedTimer clock_;
    const Frame* lastFramePtr_ = nullptr;
    const SpectrumData* lastSpecPtr_ = nullptr;
    qint64 lastMeasUpdate_ = 0;
    bool syncing_ = false;
    bool running_ = true;

    // header
    QPushButton* runBtn_ = nullptr;
    QPushButton* singleBtn_ = nullptr;
    QLabel* statePill_ = nullptr;
    QLabel* rateLabel_ = nullptr;
    QLabel* srcLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;

    // channels
    std::array<ChannelUi, kNumChannels> chUi_;

    // horizontal / trigger
    ui::Knob* tdiv_ = nullptr;
    ui::Knob* delay_ = nullptr;
    ui::Knob* trigPos_ = nullptr;
    ui::Knob* level_ = nullptr;
    ui::Knob* holdoff_ = nullptr;
    QCheckBox* autoRoll_ = nullptr;
    QCheckBox* noiseRej_ = nullptr;
    ui::Segmented* trigMode_ = nullptr;
    ui::Segmented* slope_ = nullptr;
    QComboBox* trigSrc_ = nullptr;
    QComboBox* refresh_ = nullptr;
    QLabel* acqInfo_ = nullptr;

    // generator
    int genSel_ = 0;
    ui::Segmented* genSelect_ = nullptr;
    QPushButton* genOutput_ = nullptr;
    QButtonGroup* waveGroup_ = nullptr;
    std::vector<QToolButton*> waveBtns_;
    ui::Knob* gFreq_ = nullptr;
    ui::Knob* gAmp_ = nullptr;
    ui::Knob* gOff_ = nullptr;
    ui::Knob* gPhase_ = nullptr;
    ui::Knob* gDuty_ = nullptr;
    ui::Knob* gEdge_ = nullptr;
    ui::Knob* gNoise_ = nullptr;
    QComboBox* sampleRate_ = nullptr;
    QLabel* genRoute_ = nullptr;
    QLabel* genWarn_ = nullptr;
    QPushButton* customBtn_ = nullptr;

    // math
    QPushButton* mathEnable_ = nullptr;
    QComboBox* mathOp_ = nullptr;
    QComboBox* mathA_ = nullptr;
    QComboBox* mathB_ = nullptr;
    QSpinBox* mathOrder_ = nullptr;
    QSpinBox* mathMa_ = nullptr;
    ui::Knob* mathFc_ = nullptr;
    ui::Knob* mathFc2_ = nullptr;
    ui::Knob* mathScale_ = nullptr;
    ui::Knob* mathPos_ = nullptr;
    QWidget* mathBRow_ = nullptr;
    QWidget* mathFilterBox_ = nullptr;
    QWidget* mathMaRow_ = nullptr;
    QLabel* mathInfo_ = nullptr;

    // spectrum
    QPushButton* specEnable_ = nullptr;
    QComboBox* specSrc_ = nullptr;
    QComboBox* specSize_ = nullptr;
    QComboBox* specWin_ = nullptr;
    QComboBox* specDbDiv_ = nullptr;
    ui::Segmented* specScale_ = nullptr;
    ui::Knob* specRef_ = nullptr;
    ui::Knob* specStart_ = nullptr;
    ui::Knob* specStop_ = nullptr;
    QSpinBox* specAvg_ = nullptr;
    QCheckBox* specGram_ = nullptr;
    QLabel* specInfo_ = nullptr;

    // display
    ui::Segmented* persist_ = nullptr;
    ui::Segmented* interp_ = nullptr;
    ui::Segmented* cursorMode_ = nullptr;
    QComboBox* cursorSrc_ = nullptr;
    QCheckBox* glow_ = nullptr;
    QCheckBox* showSpec_ = nullptr;
    QCheckBox* showMeas_ = nullptr;
    QSlider* gridInt_ = nullptr;
    QSlider* lineWidth_ = nullptr;
    std::array<ui::ColorButton*, kNumTraces> colorBtns_{};

    // input
    ui::Segmented* source_ = nullptr;
    QLabel* fileInfo_ = nullptr;
    QLabel* audioInfo_ = nullptr;
    QComboBox* audioDev_ = nullptr;
    QPushButton* audioBtn_ = nullptr;
};

} // namespace slp
