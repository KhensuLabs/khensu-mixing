// shared-core/src/session/AudioFileProber.cpp
#include <khensu/session/AudioFileProber.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace khensu {

AudioFileProber::AudioFileProber() : m_formatManager(std::make_unique<juce::AudioFormatManager>())
{
    m_formatManager->registerBasicFormats();
}

AudioFileProber::~AudioFileProber() = default;

std::unique_ptr<juce::AudioFormatReader> AudioFileProber::openReader(juce::AudioFormatManager& formats,
                                                                     const juce::File& file)
{
    if (std::unique_ptr<juce::AudioFormatReader> byExtension{formats.createReaderFor(file)}) return byExtension;
    // The stream overload tries every registered format and owns the stream.
    std::unique_ptr<juce::InputStream> stream = file.createInputStream();
    if (stream == nullptr) return nullptr;
    return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(std::move(stream)));
}

std::string AudioFileProber::generateId() { return juce::Uuid().toDashedString().toStdString(); }

Result<AudioFileRef> AudioFileProber::probe(const std::string& absolutePath)
{
    juce::String jucePathStr(absolutePath);
    juce::File file(jucePathStr);
    if (!file.existsAsFile()) return Result<AudioFileRef>::err("File does not exist: " + absolutePath);

    std::unique_ptr<juce::AudioFormatReader> reader = openReader(*m_formatManager, file);
    if (reader == nullptr) return Result<AudioFileRef>::err("No audio format could read: " + absolutePath);

    AudioFileRef ref;
    ref.id = generateId();
    ref.path = absolutePath;
    ref.format = file.getFileExtension().removeCharacters(".").toLowerCase().toStdString();
    ref.sample_rate = (int)reader->sampleRate;
    ref.channels = (int)reader->numChannels;
    ref.bit_depth = (int)reader->bitsPerSample;
    ref.file_size_bytes = (int64_t)file.getSize();
    ref.duration_seconds = (reader->sampleRate > 0.0) ? (double)reader->lengthInSamples / reader->sampleRate : 0.0;

    // Best-effort metadata (WAV INFO / AIFF / ID3 via format-specific keys).
    const auto& md = reader->metadataValues;
    ref.title = md.getValue("title", md.getValue("INAM", "")).toStdString();
    ref.artist = md.getValue("artist", md.getValue("IART", "")).toStdString();
    ref.album = md.getValue("album", "").toStdString();

    return Result<AudioFileRef>::ok(std::move(ref));
}

} // namespace khensu
