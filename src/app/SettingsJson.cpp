#include "SettingsJson.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace slp {

namespace {

constexpr int kFormatVersion = 1;

QString hexColor(uint32_t c) { return QStringLiteral("#%1").arg(c & 0xFFFFFF, 6, 16, QLatin1Char('0')); }
uint32_t parseColor(const QJsonValue& v, uint32_t fallback) {
    QString s = v.toString();
    if (s.startsWith('#')) s.remove(0, 1);
    bool ok = false;
    const uint32_t c = s.toUInt(&ok, 16);
    return ok && s.size() == 6 ? c : fallback;
}

template <class E> QString es(E e) { return QString::fromLatin1(toString(e)); }
template <class E> E ev(const QJsonObject& o, const char* key, E fallback) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isString()) return fallback;
    return enumFromString<E>(v.toString().toStdString(), fallback);
}
double num(const QJsonObject& o, const char* key, double fallback, double lo = -1e300, double hi = 1e300) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isDouble()) return fallback;
    const double d = v.toDouble();
    return std::isfinite(d) ? std::clamp(d, lo, hi) : fallback;
}
int inum(const QJsonObject& o, const char* key, int fallback, int lo, int hi) {
    return static_cast<int>(std::lround(num(o, key, fallback, lo, hi)));
}
bool bval(const QJsonObject& o, const char* key, bool fallback) {
    const auto v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

} // namespace

QJsonObject settingsToJson(const ScopeSettings& s) {
    QJsonObject root;
    root["format"] = "SIGNAL LAB PRO setup";
    root["version"] = kFormatVersion;

    QJsonArray chs;
    for (const auto& c : s.ch) {
        QJsonObject o;
        o["enabled"] = c.enabled;
        o["voltsPerDiv"] = c.voltsPerDiv;
        o["position"] = c.position;
        o["coupling"] = es(c.coupling);
        o["invert"] = c.invert;
        o["input"] = c.input;
        o["color"] = hexColor(c.color);
        o["lineWidth"] = c.lineWidth;
        chs.append(o);
    }
    root["channels"] = chs;

    QJsonArray gens;
    for (const auto& g : s.gen) {
        QJsonObject o;
        o["output"] = g.output;
        o["shape"] = es(g.shape);
        o["frequency"] = g.frequency;
        o["vpp"] = g.vpp;
        o["offset"] = g.offset;
        o["phaseDeg"] = g.phaseDeg;
        o["duty"] = g.duty;
        o["edgeTime"] = g.edgeTime;
        o["noiseRms"] = g.noiseRms;
        QJsonArray comps;
        for (const auto& c : g.components) {
            QJsonObject co;
            co["shape"] = es(c.shape);
            co["multiple"] = c.multiple;
            co["vpp"] = c.vpp;
            co["phaseDeg"] = c.phaseDeg;
            comps.append(co);
        }
        o["components"] = comps;
        gens.append(o);
    }
    root["generators"] = gens;

    QJsonObject tb;
    tb["secPerDiv"] = s.timebase.secPerDiv;
    tb["delay"] = s.timebase.delay;
    tb["triggerPos"] = s.timebase.triggerPos;
    tb["autoRoll"] = s.timebase.autoRoll;
    root["timebase"] = tb;

    QJsonObject tr;
    tr["mode"] = es(s.trigger.mode);
    tr["slope"] = es(s.trigger.slope);
    tr["source"] = s.trigger.source;
    tr["level"] = s.trigger.level;
    tr["holdoff"] = s.trigger.holdoff;
    tr["hysteresisDiv"] = s.trigger.hysteresisDiv;
    root["trigger"] = tr;

    QJsonObject acq;
    acq["source"] = es(s.acq.source);
    acq["sampleRate"] = s.acq.sampleRate;
    acq["refreshRate"] = s.acq.refreshRate;
    root["acquisition"] = acq;

    QJsonObject m;
    m["enabled"] = s.math.enabled;
    m["op"] = es(s.math.op);
    m["srcA"] = s.math.srcA;
    m["srcB"] = s.math.srcB;
    m["order"] = s.math.order;
    m["cutoff"] = s.math.cutoff;
    m["cutoffHigh"] = s.math.cutoffHigh;
    m["maLength"] = s.math.maLength;
    m["unitsPerDiv"] = s.math.unitsPerDiv;
    m["position"] = s.math.position;
    m["color"] = hexColor(s.math.color);
    root["math"] = m;

    QJsonObject sp;
    sp["enabled"] = s.spectrum.enabled;
    sp["source"] = s.spectrum.source;
    sp["fftSize"] = s.spectrum.fftSize;
    sp["window"] = es(s.spectrum.window);
    sp["logScale"] = s.spectrum.logScale;
    sp["refLevel"] = s.spectrum.refLevel;
    sp["dbPerDiv"] = s.spectrum.dbPerDiv;
    sp["fStart"] = s.spectrum.fStart;
    sp["fStop"] = s.spectrum.fStop;
    sp["averages"] = s.spectrum.averages;
    sp["spectrogram"] = s.spectrum.spectrogram;
    root["spectrum"] = sp;

    QJsonObject d;
    d["persistence"] = es(s.display.persistence);
    d["interpolation"] = es(s.display.interpolation);
    d["glow"] = s.display.glow;
    d["gridIntensity"] = s.display.gridIntensity;
    d["cursorMode"] = es(s.display.cursorMode);
    d["cursorSource"] = s.display.cursorSource;
    d["cursorX1"] = s.display.cursorX1;
    d["cursorX2"] = s.display.cursorX2;
    d["cursorY1"] = s.display.cursorY1;
    d["cursorY2"] = s.display.cursorY2;
    d["selectedTrace"] = s.display.selectedTrace;
    d["showSpectrum"] = s.display.showSpectrum;
    d["showMeasurements"] = s.display.showMeasurements;
    d["showPanel"] = s.display.showPanel;
    root["display"] = d;

    root["filePath"] = QString::fromStdString(s.filePath);
    return root;
}

ScopeSettings settingsFromJson(const QJsonObject& root) {
    const ScopeSettings def = ScopeSettings::defaults();
    ScopeSettings s = def;

    const QJsonArray chs = root.value("channels").toArray();
    for (int i = 0; i < kNumChannels && i < chs.size(); ++i) {
        const QJsonObject o = chs[i].toObject();
        auto& c = s.ch[i];
        const auto& d = def.ch[i];
        c.enabled = bval(o, "enabled", d.enabled);
        c.voltsPerDiv = num(o, "voltsPerDiv", d.voltsPerDiv, 1e-6, 1e6);
        c.position = num(o, "position", d.position, -100, 100);
        c.coupling = ev(o, "coupling", d.coupling);
        c.invert = bval(o, "invert", d.invert);
        c.input = inum(o, "input", d.input, 0, 3);
        c.color = parseColor(o.value("color"), d.color);
        c.lineWidth = num(o, "lineWidth", d.lineWidth, 0.5, 6.0);
    }

    const QJsonArray gens = root.value("generators").toArray();
    for (int i = 0; i < kNumGenerators && i < gens.size(); ++i) {
        const QJsonObject o = gens[i].toObject();
        auto& g = s.gen[i];
        const auto& d = def.gen[i];
        g.output = bval(o, "output", d.output);
        g.shape = ev(o, "shape", d.shape);
        g.frequency = num(o, "frequency", d.frequency, 0.0, 1e9);
        g.vpp = num(o, "vpp", d.vpp, 0.0, 1e4);
        g.offset = num(o, "offset", d.offset, -1e4, 1e4);
        g.phaseDeg = num(o, "phaseDeg", d.phaseDeg, -3600, 3600);
        g.duty = num(o, "duty", d.duty, 0.1, 99.9);
        g.edgeTime = num(o, "edgeTime", d.edgeTime, 0.0, 1.0);
        g.noiseRms = num(o, "noiseRms", d.noiseRms, 0.0, 1e4);
        if (o.value("components").isArray()) {
            g.components.clear();
            for (const auto& cv : o.value("components").toArray()) {
                const QJsonObject co = cv.toObject();
                HarmonicComponent c;
                c.shape = ev(co, "shape", Waveform::Sine);
                c.multiple = num(co, "multiple", 1.0, 0.0, 1e6);
                c.vpp = num(co, "vpp", 1.0, 0.0, 1e4);
                c.phaseDeg = num(co, "phaseDeg", 0.0, -3600, 3600);
                g.components.push_back(c);
                if (g.components.size() >= 32) break;
            }
        }
    }

    const QJsonObject tb = root.value("timebase").toObject();
    s.timebase.secPerDiv = num(tb, "secPerDiv", def.timebase.secPerDiv, 1e-9, 100);
    s.timebase.delay = num(tb, "delay", def.timebase.delay, -1e3, 1e3);
    s.timebase.triggerPos = num(tb, "triggerPos", def.timebase.triggerPos, 0, 100);
    s.timebase.autoRoll = bval(tb, "autoRoll", def.timebase.autoRoll);

    const QJsonObject tr = root.value("trigger").toObject();
    s.trigger.mode = ev(tr, "mode", def.trigger.mode);
    s.trigger.slope = ev(tr, "slope", def.trigger.slope);
    s.trigger.source = inum(tr, "source", def.trigger.source, 0, 3);
    s.trigger.level = num(tr, "level", def.trigger.level, -1e6, 1e6);
    s.trigger.holdoff = num(tr, "holdoff", def.trigger.holdoff, 0, 10);
    s.trigger.hysteresisDiv = num(tr, "hysteresisDiv", def.trigger.hysteresisDiv, 0, 4);

    const QJsonObject acq = root.value("acquisition").toObject();
    s.acq.source = ev(acq, "source", def.acq.source);
    s.acq.sampleRate = num(acq, "sampleRate", def.acq.sampleRate, 10, 1e9);
    s.acq.refreshRate = inum(acq, "refreshRate", def.acq.refreshRate, 1, 240);

    const QJsonObject m = root.value("math").toObject();
    s.math.enabled = bval(m, "enabled", def.math.enabled);
    s.math.op = ev(m, "op", def.math.op);
    s.math.srcA = inum(m, "srcA", def.math.srcA, 0, 3);
    s.math.srcB = inum(m, "srcB", def.math.srcB, 0, 3);
    s.math.order = inum(m, "order", def.math.order, 1, 8);
    s.math.cutoff = num(m, "cutoff", def.math.cutoff, 1e-3, 1e9);
    s.math.cutoffHigh = num(m, "cutoffHigh", def.math.cutoffHigh, 1e-3, 1e9);
    s.math.maLength = inum(m, "maLength", def.math.maLength, 1, 1 << 20);
    s.math.unitsPerDiv = num(m, "unitsPerDiv", def.math.unitsPerDiv, 1e-12, 1e12);
    s.math.position = num(m, "position", def.math.position, -100, 100);
    s.math.color = parseColor(m.value("color"), def.math.color);

    const QJsonObject sp = root.value("spectrum").toObject();
    s.spectrum.enabled = bval(sp, "enabled", def.spectrum.enabled);
    s.spectrum.source = inum(sp, "source", def.spectrum.source, 0, kNumTraces - 1);
    s.spectrum.fftSize = inum(sp, "fftSize", def.spectrum.fftSize, 1024, 1 << 18);
    s.spectrum.window = ev(sp, "window", def.spectrum.window);
    s.spectrum.logScale = bval(sp, "logScale", def.spectrum.logScale);
    s.spectrum.refLevel = num(sp, "refLevel", def.spectrum.refLevel, -300, 300);
    s.spectrum.dbPerDiv = num(sp, "dbPerDiv", def.spectrum.dbPerDiv, 0.1, 50);
    s.spectrum.fStart = num(sp, "fStart", def.spectrum.fStart, 0, 1e10);
    s.spectrum.fStop = num(sp, "fStop", def.spectrum.fStop, 0, 1e10);
    s.spectrum.averages = inum(sp, "averages", def.spectrum.averages, 1, 256);
    s.spectrum.spectrogram = bval(sp, "spectrogram", def.spectrum.spectrogram);

    const QJsonObject d = root.value("display").toObject();
    s.display.persistence = ev(d, "persistence", def.display.persistence);
    s.display.interpolation = ev(d, "interpolation", def.display.interpolation);
    s.display.glow = bval(d, "glow", def.display.glow);
    s.display.gridIntensity = num(d, "gridIntensity", def.display.gridIntensity, 0, 1);
    s.display.cursorMode = ev(d, "cursorMode", def.display.cursorMode);
    s.display.cursorSource = inum(d, "cursorSource", def.display.cursorSource, 0, kNumTraces - 1);
    s.display.cursorX1 = num(d, "cursorX1", def.display.cursorX1);
    s.display.cursorX2 = num(d, "cursorX2", def.display.cursorX2);
    s.display.cursorY1 = num(d, "cursorY1", def.display.cursorY1);
    s.display.cursorY2 = num(d, "cursorY2", def.display.cursorY2);
    s.display.selectedTrace = inum(d, "selectedTrace", def.display.selectedTrace, 0, kNumTraces - 1);
    s.display.showSpectrum = bval(d, "showSpectrum", def.display.showSpectrum);
    s.display.showMeasurements = bval(d, "showMeasurements", def.display.showMeasurements);
    s.display.showPanel = bval(d, "showPanel", def.display.showPanel);

    s.filePath = root.value("filePath").toString().toStdString();
    return s;
}

bool saveSettingsFile(const ScopeSettings& s, const QString& path, QString* error) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(settingsToJson(s)).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool loadSettingsFile(ScopeSettings& s, const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = pe.errorString();
        return false;
    }
    s = settingsFromJson(doc.object());
    return true;
}

} // namespace slp
