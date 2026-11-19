// tests/shared-core/test_bad_audio_files.cpp
// Bad-/unusual-input-file sweep for the C++ load paths (2026-10-16): every
// file of a hostile/edge corpus goes through AudioFileProber::probe (session
// import), AudioAnalyzer::analyze (the fast in-process analysis) and a plain
// AudioFormatManager reader pass (what the desktop waveform/peak loaders do).
// The contract: never crash or hang; a file that cannot be decoded is an
// error Result, never a "successful" analysis; and a successful analysis
// contains only finite numbers (a NaN/Inf would reach the UI and the JSON
// the app persists). The Python half of the sweep is
// local-services/tests/test_bad_audio_inputs.py.
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/session/AudioFileProber.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace khensu;

namespace {

constexpr double kPi = 3.14159265358979323846;

enum class Expect {
    Error,   // not decodable: probe and/or analyze must report an error
    Either,  // degenerate but decodable: error or a finite result
    Success, // ordinary audio that must analyse (finite) - e.g. odd paths
};

struct WavSpec {
    int channels = 2;
    double rate = 44100.0;
    int samples = 44100;
    int bits = 16;
    bool floating = false;
    std::function<float(int ch, int i)> sample = [](int, int i) {
        return static_cast<float>(0.25 * std::sin(2.0 * kPi * 440.0 * i / 44100.0));
    };
};

bool writeWav(const juce::File& file, const WavSpec& spec)
{
    file.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> out = file.createOutputStream();
    if (out == nullptr) return false;
    auto options = juce::AudioFormatWriterOptions{}
                       .withSampleRate(spec.rate)
                       .withNumChannels(spec.channels)
                       .withBitsPerSample(spec.bits);
    if (spec.floating) options = options.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer = fmt.createWriterFor(out, options);
    if (writer == nullptr) return false;
    if (spec.samples > 0) {
        juce::AudioBuffer<float> buffer(spec.channels, spec.samples);
        for (int c = 0; c < spec.channels; ++c)
            for (int i = 0; i < spec.samples; ++i)
                buffer.setSample(c, i, spec.sample(c, i));
        writer->writeFromAudioSampleBuffer(buffer, 0, spec.samples);
    }
    writer.reset();
    return file.existsAsFile();
}

bool writeBytes(const juce::File& file, const std::vector<unsigned char>& bytes)
{
    file.deleteFile();
    return file.replaceWithData(bytes.data(), bytes.size());
}

std::vector<unsigned char> randomBytes(size_t n)
{
    juce::Random rng(5);
    std::vector<unsigned char> b(n);
    for (auto& v : b)
        v = static_cast<unsigned char>(rng.nextInt(256));
    return b;
}

struct Case {
    std::string name;
    Expect expect;
    std::function<juce::File(const juce::File& dir)> build; // returns {} when unbuildable
};

juce::File wavCase(const juce::File& dir, const juce::String& name, const WavSpec& spec)
{
    const auto f = dir.getChildFile(name);
    return writeWav(f, spec) ? f : juce::File();
}

std::vector<Case> corpus()
{
    std::vector<Case> c;
    c.push_back({"zero_byte", Expect::Error, [](const juce::File& d) {
                     auto f = d.getChildFile("zero.wav");
                     return writeBytes(f, {}) ? f : juce::File();
                 }});
    c.push_back({"header_only", Expect::Error, [](const juce::File& d) {
                     WavSpec s;
                     s.samples = 0;
                     return wavCase(d, "header_only.wav", s);
                 }});
    c.push_back({"truncated", Expect::Either, [](const juce::File& d) {
                     // Header claims 1 s; only the first ~quarter of the data survives.
                     auto f = wavCase(d, "truncated.wav", WavSpec{});
                     if (f == juce::File()) return f;
                     juce::MemoryBlock mb;
                     f.loadFileAsData(mb);
                     return writeBytes(f, std::vector<unsigned char>(static_cast<const unsigned char*>(mb.getData()),
                                                                     static_cast<const unsigned char*>(mb.getData()) +
                                                                         mb.getSize() / 4))
                                ? f
                                : juce::File();
                 }});
    for (const char* ext : {"wav", "mp3", "flac", "ogg", "aiff"})
        c.push_back({std::string("random_") + ext, Expect::Error, [ext](const juce::File& d) {
                         auto f = d.getChildFile(juce::String("random.") + ext);
                         return writeBytes(f, randomBytes(64 * 1024)) ? f : juce::File();
                     }});
    c.push_back({"text_as_wav", Expect::Error, [](const juce::File& d) {
                     auto f = d.getChildFile("notes.wav");
                     return f.replaceWithText(juce::String::repeatedString("just some text\n", 200)) ? f : juce::File();
                 }});
    c.push_back({"wav_named_mp3", Expect::Success,
                 [](const juce::File& d) { return wavCase(d, "really_wav.mp3", WavSpec{}); }});
    c.push_back({"one_sample", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.samples = 1;
                     return wavCase(d, "one_sample.wav", s);
                 }});
    c.push_back({"short_50ms", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.samples = 2205;
                     return wavCase(d, "short_50ms.wav", s);
                 }});
    c.push_back({"short_350ms", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.samples = 15435;
                     return wavCase(d, "short_350ms.wav", s);
                 }});
    c.push_back({"digital_silence", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.sample = [](int, int) { return 0.0f; };
                     return wavCase(d, "silence.wav", s);
                 }});
    c.push_back({"dc_only", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.sample = [](int, int) { return 0.5f; };
                     return wavCase(d, "dc.wav", s);
                 }});
    c.push_back({"square_full_scale", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.sample = [](int, int i) { return (i / 50) % 2 == 0 ? 1.0f : -1.0f; };
                     return wavCase(d, "square.wav", s);
                 }});
    c.push_back({"pcm_8bit", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.bits = 8;
                     return wavCase(d, "u8.wav", s);
                 }});
    c.push_back({"pcm_24bit", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.bits = 24;
                     return wavCase(d, "pcm24.wav", s);
                 }});
    c.push_back({"float_over_full_scale", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.bits = 32;
                     s.floating = true;
                     s.sample = [](int, int i) { return static_cast<float>(3.5 * std::sin(2.0 * kPi * i / 100.0)); };
                     return wavCase(d, "float_hot.wav", s);
                 }});
    c.push_back({"float_inf", Expect::Error, [](const juce::File& d) {
                     WavSpec s;
                     s.bits = 32;
                     s.floating = true;
                     s.sample = [](int ch, int i) {
                         if (ch == 1 && i == 200) return std::numeric_limits<float>::infinity();
                         if (ch == 0 && i == 300) return -std::numeric_limits<float>::infinity();
                         return 0.1f;
                     };
                     return wavCase(d, "float_inf.wav", s);
                 }});
    c.push_back({"sr_8k_mono", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.channels = 1;
                     s.rate = 8000.0;
                     s.samples = 16000;
                     return wavCase(d, "sr8k_mono.wav", s);
                 }});
    c.push_back({"sr_192k", Expect::Success, [](const juce::File& d) {
                     WavSpec s;
                     s.rate = 192000.0;
                     s.samples = 96000;
                     return wavCase(d, "sr192k.wav", s);
                 }});
    c.push_back({"sr_384k", Expect::Either, [](const juce::File& d) {
                     WavSpec s;
                     s.rate = 384000.0;
                     s.samples = 96000;
                     return wavCase(d, "sr384k.wav", s);
                 }});
    c.push_back({"sr_768k", Expect::Error, [](const juce::File& d) {
                     WavSpec s;
                     s.rate = 768000.0;
                     s.samples = 96000;
                     return wavCase(d, "sr768k.wav", s);
                 }});
    for (int ch : {1, 3, 6, 8, 12})
        c.push_back({"channels_" + std::to_string(ch), Expect::Either, [ch](const juce::File& d) {
                         WavSpec s;
                         s.channels = ch;
                         return wavCase(d, juce::String("ch") + juce::String(ch) + ".wav", s);
                     }});
    // Non-ASCII, emoji and space-containing names (UTF-8 source, built with /utf-8).
    c.push_back({"path_unicode", Expect::Success, [](const juce::File& d) {
                     return wavCase(d,
                                    juce::String(juce::CharPointer_UTF8("M\xc3\xa9lodie \xc3\xbcn\xc3\xaf "
                                                                        "\xe6\x97\xa5\xe6\x9c\xac.wav")),
                                    WavSpec{});
                 }});
    c.push_back({"path_emoji_space", Expect::Success, [](const juce::File& d) {
                     return wavCase(d, juce::String(juce::CharPointer_UTF8("my mix \xf0\x9f\x8e\xb5 final v2.wav")),
                                    WavSpec{});
                 }});
    c.push_back({"path_over_260", Expect::Either, [](const juce::File& d) {
                     auto deep = d;
                     while (deep.getFullPathName().length() < 280)
                         deep = deep.getChildFile("long_segment_" + juce::String::repeatedString("x", 30));
                     if (!deep.createDirectory().wasOk()) return juce::File();
                     return wavCase(deep, "long path tone.wav", WavSpec{});
                 }});
    return c;
}

void requireFinite(const AnalysisResult& r, const std::string& name)
{
    std::vector<std::pair<const char*, float>> scalars = {
        {"lufs_integrated", r.loudness.lufs_integrated},
        {"lufs_short_term_max", r.loudness.lufs_short_term_max},
        {"lra", r.loudness.lra},
        {"plr", r.loudness.plr},
        {"true_peak_dbtp", r.loudness.true_peak_dbtp},
        {"centroid_hz", r.spectral.centroid_hz},
        {"balance_low", r.spectral.balance_low},
        {"balance_mid", r.spectral.balance_mid},
        {"balance_high", r.spectral.balance_high},
        {"tilt_db_per_octave", r.spectral.tilt_db_per_octave},
        {"crest_factor_db", r.dynamics.crest_factor_db},
        {"dynamic_range_db", r.dynamics.dynamic_range_db},
        {"peak_to_rms_db", r.dynamics.peak_to_rms_db},
        {"width", r.stereo.width},
        {"correlation", r.stereo.correlation},
        {"ms_balance", r.stereo.ms_balance},
        {"overall_correlation", r.phase.overall_correlation},
        {"noise_floor_db", r.artifacts.noise_floor_db},
        {"sample_reversal_rate", r.artifacts.sample_reversal_rate},
    };
    for (const auto& [field, v] : scalars) {
        INFO(name << ": " << field << " = " << v);
        CHECK(std::isfinite(v));
    }
    for (const auto* vec :
         {&r.spectral.rms_per_octave_db, &r.spectral.rms_mid_per_octave_db, &r.spectral.rms_side_per_octave_db,
          &r.spectral.fft_mid_magnitudes, &r.spectral.fft_side_magnitudes}) {
        size_t bad = 0;
        for (float v : *vec)
            bad += std::isfinite(v) ? 0u : 1u;
        INFO(name << ": spectral vector has " << bad << " non-finite values");
        CHECK(bad == 0u);
    }
    CHECK(std::isfinite(r.sample_rate));
}

/// What the desktop peak/waveform loaders do: open via a fresh
/// AudioFormatManager and walk the file with readMaxLevels.
void readerPass(const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
    if (reader == nullptr || reader->lengthInSamples <= 0) return;
    const juce::int64 total = reader->lengthInSamples;
    const juce::int64 step = juce::jmax<juce::int64>(1, total / 64);
    for (juce::int64 pos = 0; pos < total; pos += step) {
        juce::Range<float> levels[2];
        reader->readMaxLevels(pos, (int)juce::jmin(step, total - pos), levels, juce::jmin(2, (int)reader->numChannels));
    }
}

} // namespace

TEST_CASE("Bad-audio corpus: probe, analyze and reader passes never crash and never report NaN",
          "[audio_analyzer][prober][bad_input]")
{
    juce::TemporaryFile tmpDir;
    const juce::File root = tmpDir.getFile();
    REQUIRE(root.createDirectory().wasOk());

    for (const auto& entry : corpus()) {
        DYNAMIC_SECTION(entry.name)
        {
            const auto dir = root.getChildFile(entry.name);
            REQUIRE(dir.createDirectory().wasOk());
            const juce::File file = entry.build(dir);
            if (file == juce::File()) {
                WARN(entry.name << ": could not be created on this machine - skipped");
                continue;
            }
            const std::string path = file.getFullPathName().toStdString();
            INFO(entry.name << " -> " << path);

            AudioFileProber prober;
            auto probed = prober.probe(path);
            if (probed.is_ok()) {
                CHECK(std::isfinite(probed.value().duration_seconds));
                CHECK(probed.value().duration_seconds >= 0.0);
            }

            AudioFileRef ref;
            ref.id = entry.name;
            ref.path = path;
            AudioAnalyzer analyzer;
            auto analyzed = analyzer.analyze(ref, "bad-input-sweep");

            readerPass(file);

            switch (entry.expect) {
            case Expect::Error:
                CHECK(analyzed.is_err());
                break;
            case Expect::Success:
                CHECK(probed.is_ok());
                REQUIRE(analyzed.is_ok());
                break;
            case Expect::Either:
                break;
            }
            if (analyzed.is_ok()) requireFinite(analyzed.value(), entry.name);
        }
    }
    root.deleteRecursively();
}

#if JUCE_WINDOWS
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t*);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void*, const char*);

namespace {
/// ntdll's RtlAreLongPathsEnabled (Windows 10 1607+; not in every SDK's
/// import library, so looked up at runtime). False when it doesn't exist.
bool longPathsEnabledForProcess()
{
    using Fn = unsigned char(__stdcall*)();
    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll != nullptr ? reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")) : nullptr;
    return fn != nullptr && fn() != 0;
}
} // namespace
#endif

TEST_CASE("Audio over a path longer than 260 characters probes and analyzes", "[prober][bad_input][long_path]")
{
#if JUCE_WINDOWS
    // True only when this exe's manifest opts in (cmake/KhensuLongPathAware
    // .manifest) AND the machine policy LongPathsEnabled is on.
    if (!longPathsEnabledForProcess()) {
        WARN("long paths are not enabled for this process/machine - skipped");
        return;
    }
#endif
    juce::TemporaryFile tmpDir;
    const juce::File root = tmpDir.getFile();
    auto deep = root;
    while (deep.getFullPathName().length() < 300)
        deep = deep.getChildFile("long_segment_" + juce::String::repeatedString("x", 30));
    REQUIRE(deep.createDirectory().wasOk());
    const juce::File file = wavCase(deep, "long path tone.wav", WavSpec{});
    REQUIRE(file.existsAsFile());
    const std::string path = file.getFullPathName().toStdString();
    REQUIRE(path.size() > 260);

    AudioFileProber prober;
    auto probed = prober.probe(path);
    REQUIRE(probed.is_ok());
    CHECK(probed.value().duration_seconds > 0.0);

    AudioFileRef ref;
    ref.id = "long_path";
    ref.path = path;
    AudioAnalyzer analyzer;
    CHECK(analyzer.analyze(ref, "long-path").is_ok());
    root.deleteRecursively();
}
