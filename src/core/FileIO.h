// SIGNAL LAB PRO - data import / export (CSV, WAV).
//
// CSV import : comma / semicolon / tab separated, optional header row, '#' comment lines.
//              A time column is detected by its header name (t, time, time_s, seconds ...)
//              or, without header, by a strictly increasing first column. The sample rate
//              comes from the time column, from a "# sample_rate_hz=" comment, or from the
//              caller supplied default. Up to 4 value columns are used.
// WAV import : PCM 8/16/24/32-bit, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE. Samples are
//              normalised to full scale (unit "FS"). Files written by SIGNAL LAB PRO carry
//              a full-scale voltage tag (INFO/ICMT "SLP_VFS=...") and are restored in volts.
// WAV export : 16-bit PCM or 32-bit float, <= 4 channels; volts are divided by the
//              full-scale voltage Vfs and the tag is stored so the round trip is lossless.
#pragma once

#include <string>
#include <vector>

namespace slp {

struct SignalData {
    double sampleRate = 0.0;
    std::vector<std::string> names;
    std::vector<std::string> units;
    std::vector<std::vector<float>> channels;
    std::string sourcePath;
    std::string note; // warnings produced while importing

    size_t length() const { return channels.empty() ? 0 : channels.front().size(); }
};

bool readCsv(const std::string& path, SignalData& out, std::string& error, double defaultRate = 1e6);
bool readWav(const std::string& path, SignalData& out, std::string& error);
bool readSignalFile(const std::string& path, SignalData& out, std::string& error, double defaultRate = 1e6);

struct ExportColumn {
    std::string name; // e.g. "CH1"
    std::string unit; // e.g. "V"
    const float* data = nullptr;
};

// Writes "time_s, <name>_<unit>, ..." with header comments (prefixed by '#').
bool writeCsv(const std::string& path, const std::vector<ExportColumn>& cols, size_t n, double t0, double dt,
              const std::vector<std::string>& comments, std::string& error);

// bits = 16 (PCM) or 32 (float). fullScale <= 0 selects the smallest 1-2-5 value above the peak.
bool writeWav(const std::string& path, const std::vector<ExportColumn>& cols, size_t n, double sampleRate, int bits,
              double fullScale, std::string& error, double* usedFullScale = nullptr);

// Locale independent number parsing (used for CSV). Returns false on garbage.
bool parseNumber(const char* begin, const char* end, double& out);

} // namespace slp
