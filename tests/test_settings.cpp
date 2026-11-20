// SIGNAL LAB PRO - setup persistence tests (Qt Core JSON).
#include "TestFramework.h"

#include "SettingsJson.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace slp;

namespace {
ScopeSettings modified() {
    ScopeSettings s = ScopeSettings::defaults();
    s.ch[2].enabled = true;
    s.ch[2].voltsPerDiv = 0.02;
    s.ch[2].position = -1.25;
    s.ch[2].coupling = Coupling::AC;
    s.ch[2].invert = true;
    s.ch[2].input = 3;
    s.ch[2].color = 0x123456;
    s.gen[1].shape = Waveform::Pulse;
    s.gen[1].frequency = 12345.678;
    s.gen[1].duty = 12.5;
    s.gen[1].edgeTime = 3e-7;
    s.gen[3].components = {{Waveform::Triangle, 2.5, 0.3, 45.0}};
    s.timebase.secPerDiv = 5e-6;
    s.timebase.delay = -1.5e-6;
    s.timebase.triggerPos = 20;
    s.trigger.mode = TriggerMode::Normal;
    s.trigger.slope = TriggerSlope::Falling;
    s.trigger.source = 2;
    s.trigger.level = -0.125;
    s.trigger.holdoff = 1e-3;
    s.acq.sampleRate = 25e6;
    s.acq.source = SourceKind::File;
    s.math.enabled = true;
    s.math.op = MathOp::BandPass;
    s.math.cutoff = 100;
    s.math.cutoffHigh = 2000;
    s.spectrum.fftSize = 65536;
    s.spectrum.window = WindowType::FlatTop;
    s.spectrum.fStop = 20000;
    s.display.cursorMode = CursorMode::Track;
    s.display.persistence = Persistence::Long;
    s.display.cursorX1 = -3e-6;
    s.filePath = "/tmp/some file.csv";
    return s;
}
} // namespace

TEST(settings_json_roundtrip) {
    const ScopeSettings s = modified();
    const ScopeSettings r = settingsFromJson(settingsToJson(s));
    CHECK(r == s);
    // and through the serialized text form
    const QByteArray text = QJsonDocument(settingsToJson(s)).toJson();
    const ScopeSettings r2 = settingsFromJson(QJsonDocument::fromJson(text).object());
    CHECK(r2 == s);
    CHECK(settingsFromJson(settingsToJson(ScopeSettings::defaults())) == ScopeSettings::defaults());
}

TEST(settings_defaults_and_robustness) {
    // empty object -> defaults
    CHECK(settingsFromJson(QJsonObject{}) == ScopeSettings::defaults());
    // invalid values are rejected / clamped instead of corrupting the instrument state
    QJsonObject o = settingsToJson(ScopeSettings::defaults());
    QJsonObject tb = o["timebase"].toObject();
    tb["secPerDiv"] = -5.0;      // clamped to the minimum
    tb["triggerPos"] = "oops";   // wrong type -> default
    o["timebase"] = tb;
    QJsonObject tr = o["trigger"].toObject();
    tr["mode"] = "Bogus";        // unknown enum -> default
    tr["source"] = 17;           // clamped to CH4
    o["trigger"] = tr;
    const ScopeSettings s = settingsFromJson(o);
    CHECK(s.timebase.secPerDiv > 0);
    CHECK(s.timebase.triggerPos == ScopeSettings::defaults().timebase.triggerPos);
    CHECK(s.trigger.mode == ScopeSettings::defaults().trigger.mode);
    CHECK(s.trigger.source == 3);
}

TEST(settings_file_save_load) {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("setup.json");
    const ScopeSettings s = modified();
    QString err;
    CHECK(saveSettingsFile(s, path, &err));
    ScopeSettings r;
    CHECK(loadSettingsFile(r, path, &err));
    CHECK(r == s);
    CHECK(!loadSettingsFile(r, dir.filePath("missing.json"), &err));
    QFile bad(dir.filePath("bad.json"));
    CHECK(bad.open(QIODevice::WriteOnly));
    bad.write("{ not json");
    bad.close();
    CHECK(!loadSettingsFile(r, bad.fileName(), &err));
    CHECK(r == s); // unchanged on failure
}

int main(int argc, char** argv) { return slptest::runAll(argc, argv); }
