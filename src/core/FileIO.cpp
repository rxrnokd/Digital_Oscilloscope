#include "FileIO.h"

#include "ScopeSettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace slp {

// ================================================================ helpers
bool parseNumber(const char* b, const char* e, double& out) {
    while (b < e && (std::isspace(static_cast<unsigned char>(*b)) || *b == '"')) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(e[-1])) || e[-1] == '"')) --e;
    if (b >= e) return false;
    if (e - b == 3 && (std::tolower(b[0]) == 'n' && std::tolower(b[1]) == 'a' && std::tolower(b[2]) == 'n')) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    const char* p = b;
    bool neg = false;
    if (*p == '+' || *p == '-') { neg = *p == '-'; ++p; }
    uint64_t mant = 0;
    int exp10 = 0, digits = 0, sig = 0;
    while (p < e && std::isdigit(static_cast<unsigned char>(*p))) {
        if (sig < 19) { mant = mant * 10 + static_cast<uint64_t>(*p - '0'); if (mant) ++sig; }
        else ++exp10;
        ++p; ++digits;
    }
    if (p < e && (*p == '.' || *p == ',')) {
        ++p;
        while (p < e && std::isdigit(static_cast<unsigned char>(*p))) {
            if (sig < 19) { mant = mant * 10 + static_cast<uint64_t>(*p - '0'); --exp10; if (mant) ++sig; }
            ++p; ++digits;
        }
    }
    if (digits == 0) return false;
    if (p < e && (*p == 'e' || *p == 'E')) {
        ++p;
        bool eneg = false;
        if (p < e && (*p == '+' || *p == '-')) { eneg = *p == '-'; ++p; }
        if (p >= e || !std::isdigit(static_cast<unsigned char>(*p))) return false;
        int ex = 0;
        while (p < e && std::isdigit(static_cast<unsigned char>(*p))) { ex = std::min(ex * 10 + (*p - '0'), 9999); ++p; }
        exp10 += eneg ? -ex : ex;
    }
    if (p != e) return false;
    double v = static_cast<double>(mant);
    if (exp10 != 0) v *= std::pow(10.0, exp10);
    out = neg ? -v : v;
    return true;
}

namespace {

std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) || c == '"'; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& line, char delim) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c == delim && !quoted) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// "CH1 (V)" -> name CH1, unit V ;  "CH1 [mV]" ;  "CH1_V"
void splitNameUnit(const std::string& header, std::string& name, std::string& unit) {
    std::string h = trim(header);
    unit.clear();
    for (char open : {'(', '['}) {
        const char close = open == '(' ? ')' : ']';
        auto a = h.rfind(open), b = h.rfind(close);
        if (a != std::string::npos && b != std::string::npos && b > a) {
            unit = trim(h.substr(a + 1, b - a - 1));
            name = trim(h.substr(0, a));
            return;
        }
    }
    name = h;
}

bool isTimeHeader(const std::string& h) {
    std::string n, u;
    splitNameUnit(h, n, u);
    n = lower(n);
    return n == "t" || n == "time" || n == "time_s" || n == "seconds" || n == "sec" || n == "s" || n == "x-axis" ||
           n.rfind("time", 0) == 0;
}

double timeUnitScale(const std::string& unit) {
    const std::string u = lower(unit);
    if (u == "ms") return 1e-3;
    if (u == "us" || u == "\xC2\xB5s") return 1e-6;
    if (u == "ns") return 1e-9;
    return 1.0;
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
void wrTag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

bool readAll(const std::string& path, std::string& data, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open file: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    data = ss.str();
    return true;
}

} // namespace

// ================================================================ CSV
bool readCsv(const std::string& path, SignalData& out, std::string& error, double defaultRate) {
    std::string text;
    if (!readAll(path, text, error)) return false;
    out = SignalData{};
    out.sourcePath = path;

    double metaRate = 0.0;
    std::vector<std::string> metaUnits;
    std::vector<std::string> header;
    std::vector<std::vector<double>> cols;
    char delim = 0;
    size_t skipped = 0;
    bool decimalComma = false;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3); // UTF-8 BOM
        const std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '#') {
            const std::string l = lower(t);
            auto eq = l.find('=');
            if (eq != std::string::npos) {
                const std::string key = trim(l.substr(1, eq - 1));
                const std::string val = trim(t.substr(eq + 1));
                if (key == "sample_rate_hz" || key == "sample_rate" || key == "samplerate") {
                    double v;
                    if (parseNumber(val.data(), val.data() + val.size(), v) && v > 0) metaRate = v;
                }
            }
            continue;
        }
        if (!delim) {
            const auto nc = std::count(t.begin(), t.end(), ',');
            const auto ns = std::count(t.begin(), t.end(), ';');
            const auto nt = std::count(t.begin(), t.end(), '\t');
            if (ns > 0 && ns >= nt) { delim = ';'; decimalComma = nc > 0; }
            else if (nt > 0 && nt >= nc) delim = '\t';
            else delim = ',';
        }
        auto tok = split(t, delim);
        std::vector<double> vals(tok.size());
        bool numeric = true;
        for (size_t i = 0; i < tok.size(); ++i) {
            std::string s = trim(tok[i]);
            if (decimalComma) std::replace(s.begin(), s.end(), ',', '.');
            if (!parseNumber(s.data(), s.data() + s.size(), vals[i])) { numeric = false; break; }
        }
        if (!numeric) {
            if (cols.empty() && header.empty()) { header = tok; continue; }
            ++skipped;
            continue;
        }
        if (cols.empty()) cols.resize(tok.size());
        if (tok.size() != cols.size()) { ++skipped; continue; }
        for (size_t i = 0; i < tok.size(); ++i) cols[i].push_back(vals[i]);
    }
    if (cols.empty() || cols[0].size() < 2) { error = "no numeric data found in CSV"; return false; }
    const size_t n = cols[0].size();

    // ---- time column detection
    bool hasTime = false;
    double tScale = 1.0;
    if (!header.empty() && header.size() == cols.size() && isTimeHeader(header[0]) && cols.size() >= 2) {
        hasTime = true;
        std::string nm, un;
        splitNameUnit(header[0], nm, un);
        tScale = timeUnitScale(un);
    } else if (header.empty() && cols.size() >= 2) {
        bool inc = true;
        const size_t check = std::min<size_t>(n, 200);
        for (size_t i = 1; i < check && inc; ++i) inc = cols[0][i] > cols[0][i - 1];
        if (inc) {
            const double d0 = cols[0][1] - cols[0][0];
            const double dl = cols[0][check - 1] - cols[0][check - 2];
            hasTime = std::fabs(dl - d0) <= 0.01 * std::fabs(d0);
        }
    }

    std::vector<size_t> valueCols;
    for (size_t i = hasTime ? 1 : 0; i < cols.size() && valueCols.size() < 4; ++i) valueCols.push_back(i);
    if (cols.size() - (hasTime ? 1 : 0) > 4) out.note += "more than 4 value columns: only the first 4 are used. ";

    double rate = metaRate > 0 ? metaRate : defaultRate;
    std::vector<double> tcol;
    if (hasTime) {
        tcol = cols[0];
        for (auto& v : tcol) v *= tScale;
        const double span = tcol.back() - tcol.front();
        if (!(span > 0)) { error = "time column is not increasing"; return false; }
        rate = static_cast<double>(n - 1) / span;
        const double dtm = span / static_cast<double>(n - 1);
        double maxDev = 0;
        for (size_t i = 1; i < n; ++i) maxDev = std::max(maxDev, std::fabs((tcol[i] - tcol[i - 1]) - dtm));
        if (maxDev > 0.01 * dtm) out.note += "non-uniform time stamps: linearly resampled to a uniform grid. ";
        else tcol.clear(); // uniform: no resampling needed
    }
    out.sampleRate = rate;

    for (size_t vi = 0; vi < valueCols.size(); ++vi) {
        const size_t c = valueCols[vi];
        std::string name = "Col" + std::to_string(vi + 1), unit = "V";
        if (!header.empty() && c < header.size()) {
            std::string nm, un;
            splitNameUnit(header[c], nm, un);
            if (!nm.empty()) name = nm;
            if (!un.empty()) unit = un;
        }
        std::vector<float> data(n);
        if (tcol.empty()) {
            for (size_t i = 0; i < n; ++i) data[i] = static_cast<float>(cols[c][i]);
        } else { // resample
            const double t0 = tcol.front(), dt = 1.0 / rate;
            size_t j = 0;
            for (size_t i = 0; i < n; ++i) {
                const double t = t0 + static_cast<double>(i) * dt;
                while (j + 2 < n && tcol[j + 1] < t) ++j;
                const double a = (t - tcol[j]) / (tcol[j + 1] - tcol[j]);
                data[i] = static_cast<float>(cols[c][j] + std::clamp(a, 0.0, 1.0) * (cols[c][j + 1] - cols[c][j]));
            }
        }
        out.channels.push_back(std::move(data));
        out.names.push_back(name);
        out.units.push_back(unit);
    }
    if (skipped) out.note += std::to_string(skipped) + " malformed rows skipped. ";
    if (!hasTime && metaRate <= 0) out.note += "no time column: sample rate set to " + formatSI(rate, "Sa/s") + ". ";
    return true;
}

bool writeCsv(const std::string& path, const std::vector<ExportColumn>& cols, size_t n, double t0, double dt,
              const std::vector<std::string>& comments, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "cannot write file: " + path; return false; }
    for (const auto& c : comments) std::fprintf(f, "# %s\n", c.c_str());
    if (dt > 0) std::fprintf(f, "# sample_rate_hz=%.12g\n", 1.0 / dt);
    std::fprintf(f, "time (s)");
    for (const auto& c : cols) std::fprintf(f, ",%s (%s)", c.name.c_str(), c.unit.c_str());
    std::fprintf(f, "\n");
    for (size_t i = 0; i < n; ++i) {
        std::fprintf(f, "%.12g", t0 + static_cast<double>(i) * dt);
        for (const auto& c : cols) {
            const float v = c.data[i];
            if (std::isfinite(v)) std::fprintf(f, ",%.9g", static_cast<double>(v));
            else std::fprintf(f, ",nan");
        }
        std::fprintf(f, "\n");
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) error = "write error: " + path;
    return ok;
}

// ================================================================ WAV
bool readWav(const std::string& path, SignalData& out, std::string& error) {
    std::string raw;
    if (!readAll(path, raw, error)) return false;
    const auto* d = reinterpret_cast<const uint8_t*>(raw.data());
    const size_t size = raw.size();
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    out = SignalData{};
    out.sourcePath = path;
    uint16_t fmt = 0, channels = 0, blockAlign = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    std::string comment;
    size_t p = 12;
    while (p + 8 <= size) {
        const uint8_t* ck = d + p;
        const uint32_t ckSize = rd32(ck + 4);
        const size_t bodyEnd = std::min(size, p + 8 + static_cast<size_t>(ckSize));
        const uint8_t* body = ck + 8;
        const size_t bodyLen = bodyEnd - (p + 8);
        if (std::memcmp(ck, "fmt ", 4) == 0 && bodyLen >= 16) {
            fmt = rd16(body);
            channels = rd16(body + 2);
            rate = rd32(body + 4);
            blockAlign = rd16(body + 12);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && bodyLen >= 40) fmt = rd16(body + 24); // sub-format GUID
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = body;
            dataSize = bodyLen;
        } else if (std::memcmp(ck, "LIST", 4) == 0 && bodyLen >= 4 && std::memcmp(body, "INFO", 4) == 0) {
            size_t q = 4;
            while (q + 8 <= bodyLen) {
                const uint32_t sz = rd32(body + q + 4);
                if (std::memcmp(body + q, "ICMT", 4) == 0 && q + 8 + sz <= bodyLen)
                    comment.assign(reinterpret_cast<const char*>(body + q + 8), strnlen(reinterpret_cast<const char*>(body + q + 8), sz));
                q += 8 + sz + (sz & 1);
            }
        }
        p += 8 + static_cast<size_t>(ckSize) + (ckSize & 1);
    }
    if (!data || channels == 0 || rate == 0 || blockAlign == 0) { error = "WAV file without fmt/data chunk"; return false; }
    if (!(fmt == 1 || fmt == 3)) { error = "unsupported WAV encoding (format " + std::to_string(fmt) + ")"; return false; }
    const int bytes = bits / 8;
    if ((fmt == 1 && (bits != 8 && bits != 16 && bits != 24 && bits != 32)) || (fmt == 3 && bits != 32 && bits != 64)) {
        error = "unsupported WAV bit depth " + std::to_string(bits);
        return false;
    }
    if (blockAlign < channels * bytes) { error = "corrupt WAV block alignment"; return false; }

    double vfs = 0.0;
    std::vector<std::string> tagUnits;
    if (auto a = comment.find("SLP_VFS="); a != std::string::npos) {
        auto e = comment.find(';', a);
        const std::string v = comment.substr(a + 8, e == std::string::npos ? std::string::npos : e - a - 8);
        parseNumber(v.data(), v.data() + v.size(), vfs);
    }
    if (auto a = comment.find("SLP_UNITS="); a != std::string::npos) {
        auto e = comment.find(';', a);
        tagUnits = split(comment.substr(a + 10, e == std::string::npos ? std::string::npos : e - a - 10), ',');
    }

    const size_t frames = dataSize / blockAlign;
    const int useCh = std::min<int>(channels, 4);
    if (channels > 4) out.note += "more than 4 WAV channels: only the first 4 are used. ";
    out.sampleRate = rate;
    out.channels.assign(useCh, std::vector<float>(frames));
    const double intScale16 = vfs > 0 ? 1.0 / 32767.0 : 1.0 / 32768.0;
    for (size_t f = 0; f < frames; ++f) {
        const uint8_t* fr = data + f * blockAlign;
        for (int c = 0; c < useCh; ++c) {
            const uint8_t* s = fr + c * bytes;
            double v = 0;
            if (fmt == 1) {
                switch (bits) {
                case 8: v = (static_cast<int>(s[0]) - 128) / 128.0; break;
                case 16: v = static_cast<int16_t>(rd16(s)) * intScale16; break;
                case 24: {
                    int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
                    if (x & 0x800000) x |= ~0xFFFFFF;
                    v = x / 8388608.0;
                    break;
                }
                case 32: v = static_cast<int32_t>(rd32(s)) / 2147483648.0; break;
                }
            } else if (bits == 32) {
                uint32_t u = rd32(s);
                float x;
                std::memcpy(&x, &u, 4);
                v = x;
            } else {
                uint64_t u = static_cast<uint64_t>(rd32(s)) | (static_cast<uint64_t>(rd32(s + 4)) << 32);
                double x;
                std::memcpy(&x, &u, 8);
                v = x;
            }
            if (vfs > 0) v *= vfs;
            out.channels[c][f] = static_cast<float>(v);
        }
    }
    for (int c = 0; c < useCh; ++c) {
        out.names.push_back("WAV" + std::to_string(c + 1));
        if (vfs > 0) out.units.push_back(c < static_cast<int>(tagUnits.size()) && !tagUnits[c].empty() ? tagUnits[c] : "V");
        else out.units.push_back("FS");
    }
    if (vfs <= 0) out.note += "WAV samples are shown in full-scale units (FS), not volts. ";
    return true;
}

bool writeWav(const std::string& path, const std::vector<ExportColumn>& cols, size_t n, double sampleRate, int bits,
              double fullScale, std::string& error, double* usedFullScale) {
    if (cols.empty() || cols.size() > 4) { error = "WAV export supports 1 to 4 channels"; return false; }
    if (!(sampleRate >= 1.0) || sampleRate > 4294967295.0) { error = "sample rate not representable in WAV"; return false; }
    if (bits != 16 && bits != 32) bits = 32;
    if (fullScale <= 0) {
        double peak = 0;
        for (const auto& c : cols)
            for (size_t i = 0; i < n; ++i)
                if (std::isfinite(c.data[i])) peak = std::max(peak, static_cast<double>(std::fabs(c.data[i])));
        fullScale = ceil125(std::max(peak * 1.0001, 1e-6));
    }
    if (usedFullScale) *usedFullScale = fullScale;
    const uint16_t ch = static_cast<uint16_t>(cols.size());
    const uint16_t format = bits == 16 ? 1 : 3;
    const bool extensible = ch > 2;
    const uint16_t blockAlign = static_cast<uint16_t>(ch * bits / 8);
    const uint32_t rate = static_cast<uint32_t>(std::llround(sampleRate));

    std::string cmt = "SLP_VFS=";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", fullScale);
    cmt += buf;
    cmt += ";SLP_UNITS=";
    for (size_t i = 0; i < cols.size(); ++i) cmt += (i ? "," : "") + cols[i].unit;
    cmt.push_back('\0');
    if (cmt.size() & 1) cmt.push_back('\0');

    std::vector<uint8_t> v;
    v.reserve(64 + n * blockAlign);
    wrTag(v, "RIFF");
    wr32(v, 0); // patched below
    wrTag(v, "WAVE");
    wrTag(v, "fmt ");
    wr32(v, extensible ? 40 : (format == 3 ? 18 : 16));
    wr16(v, extensible ? 0xFFFE : format);
    wr16(v, ch);
    wr32(v, rate);
    wr32(v, rate * blockAlign);
    wr16(v, blockAlign);
    wr16(v, static_cast<uint16_t>(bits));
    if (extensible) {
        wr16(v, 22);
        wr16(v, static_cast<uint16_t>(bits));
        wr32(v, 0); // no speaker mapping (measurement channels)
        wr16(v, format);
        static const uint8_t guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        v.insert(v.end(), guidTail, guidTail + 14);
    } else if (format == 3) {
        wr16(v, 0);
    }
    if (format == 3) {
        wrTag(v, "fact");
        wr32(v, 4);
        wr32(v, static_cast<uint32_t>(n));
    }
    wrTag(v, "LIST");
    wr32(v, static_cast<uint32_t>(4 + 8 + cmt.size()));
    wrTag(v, "INFO");
    wrTag(v, "ICMT");
    wr32(v, static_cast<uint32_t>(cmt.size()));
    v.insert(v.end(), cmt.begin(), cmt.end());
    wrTag(v, "data");
    wr32(v, static_cast<uint32_t>(n * blockAlign));
    const double inv = 1.0 / fullScale;
    for (size_t i = 0; i < n; ++i) {
        for (const auto& c : cols) {
            double x = std::isfinite(c.data[i]) ? c.data[i] * inv : 0.0;
            if (bits == 16) {
                x = std::clamp(x, -1.0, 1.0);
                wr16(v, static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 32767.0))));
            } else {
                const float fx = static_cast<float>(x);
                uint32_t u;
                std::memcpy(&u, &fx, 4);
                wr32(v, u);
            }
        }
    }
    const uint32_t riff = static_cast<uint32_t>(v.size() - 8);
    for (int i = 0; i < 4; ++i) v[4 + i] = (riff >> (8 * i)) & 0xFF;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "cannot write file: " + path; return false; }
    const bool ok = std::fwrite(v.data(), 1, v.size(), f) == v.size();
    std::fclose(f);
    if (!ok) error = "write error: " + path;
    return ok;
}

bool readSignalFile(const std::string& path, SignalData& out, std::string& error, double defaultRate) {
    std::string ext = path.size() >= 4 ? lower(path.substr(path.size() - 4)) : "";
    if (ext == ".wav") return readWav(path, out, error);
    return readCsv(path, out, error, defaultRate);
}

} // namespace slp
