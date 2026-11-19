// shared-core/include/khensu/session/AudioFileProber.h
#pragma once
#include <khensu/session/AudioFileRef.h>
#include <khensu/common/Result.h>
#include <memory>
#include <string>

namespace juce {
class AudioFormatManager;
class AudioFormatReader;
class File;
} // namespace juce

namespace khensu {

/// Reads audio file metadata into an AudioFileRef.
/// Does NOT decode audio sample data - only headers/metadata.
/// THREAD_CONTEXT(WorkerThread) - performs disk I/O. Do NOT call from the AudioThread.
class AudioFileProber {
  public:
    AudioFileProber();
    ~AudioFileProber();

    /// Probe the file at absolutePath. Returns an error Result if the file
    /// does not exist or no registered format can read it.
    Result<AudioFileRef> probe(const std::string& absolutePath);

    /// Opens a reader for `file`, trying the format its extension names
    /// first and then every registered format by content. JUCE's own
    /// createReaderFor(File) only tries extension matches, so a WAV saved as
    /// ".mp3" (or an MP3 named ".wav") would be unreadable here while the
    /// Python service, which sniffs content, analyses it. Every desktop load
    /// path uses this so import, analysis and playback agree. nullptr when
    /// nothing can read it.
    static std::unique_ptr<juce::AudioFormatReader> openReader(juce::AudioFormatManager& formats,
                                                               const juce::File& file);

  private:
    std::unique_ptr<juce::AudioFormatManager> m_formatManager;
    static std::string generateId();
};

} // namespace khensu
