#pragma once
#include <string>

namespace khensu {

/// A reference to an audio file on disk. Does NOT own or embed the audio data.
/// THREAD_CONTEXT(AnyThread) - plain data struct, safe everywhere.
struct AudioFileRef {
    std::string id;     // UUID v4
    std::string path;   // Absolute path on disk
    std::string format; // "wav" | "mp3" | "flac" | "aiff" | "ogg"
    double duration_seconds = 0.0;
    int sample_rate = 0;
    int channels = 0;
    int bit_depth = 0;
    int64_t file_size_bytes = 0;
    std::string title; // From ID3/RIFF metadata
    std::string artist;
    std::string album;
    std::string metadata_json; // Raw extra metadata as JSON blob
    // Multitrack sessions (a folder of bounced stems): the track name derived
    // from the file name and its stem-taxonomy role ("kick", "vocals", ...).
    // The session's summed mixdown has role "mix". Empty for ordinary files.
    std::string track_name;
    std::string track_role;
};

} // namespace khensu
