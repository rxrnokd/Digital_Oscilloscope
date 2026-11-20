// SIGNAL LAB PRO - application entry point.
#include "Controls.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFont>

#include <clocale>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    // Qt sets the C locale from the environment on Unix; numeric I/O (CSV export,
    // printf-style formatting) must always use '.' as the decimal separator.
    std::setlocale(LC_NUMERIC, "C");
    QApplication::setApplicationName("SignalLabPro");
    QApplication::setOrganizationName("SignalLabPro");
    QApplication::setApplicationDisplayName("SIGNAL LAB PRO");
    QApplication::setApplicationVersion("1.0.0");
    app.setStyle("Fusion");
    app.setStyleSheet(slp::ui::styleSheet());
    app.setWindowIcon(slp::ui::appIcon());

    QCommandLineParser p;
    p.setApplicationDescription("SIGNAL LAB PRO - digital oscilloscope and signal analyzer");
    p.addHelpOption();
    p.addVersionOption();
    QCommandLineOption settings("settings", "Session file to restore / save.", "file");
    QCommandLineOption setup("load-setup", "Apply a setup (JSON) after start.", "file");
    QCommandLineOption dump("dump-settings", "Write the live settings to <file> at exit.", "file");
    QCommandLineOption shot("screenshot", "Save a window screenshot (PNG) before an automatic quit.", "file");
    QCommandLineOption open("open", "Open a CSV/WAV signal file.", "file");
    QCommandLineOption quit("quit-after", "Quit automatically after <ms> milliseconds.", "ms");
    QCommandLineOption size("size", "Initial window size, e.g. 1600x1000.", "WxH");
    p.addOptions({settings, setup, dump, shot, open, quit, size});
    p.process(app);

    slp::AppOptions opt;
    opt.settingsPath = p.value(settings);
    opt.loadSetupPath = p.value(setup);
    opt.dumpSettingsPath = p.value(dump);
    opt.screenshotPath = p.value(shot);
    opt.openFile = p.value(open);
    opt.quitAfterMs = p.value(quit).toInt();
    if (p.isSet(size)) {
        const auto parts = p.value(size).split('x');
        if (parts.size() == 2) opt.size = QSize(parts[0].toInt(), parts[1].toInt());
    }

    slp::MainWindow w(opt);
    w.show();
    return app.exec();
}
