// desktop-app/Source/diagnostics/DiagnosticsBundle.cpp - see the header.
#include "DiagnosticsBundle.h"

#include <algorithm>
#include <cstring>

namespace khensu::diagnostics {

namespace {

constexpr const char* kTokenFileName = "token.txt";
constexpr const char* kRedacted = "<redacted>";

bool isTokenFile(const juce::File& f)
{
    return f.getFileName().equalsIgnoreCase(kTokenFileName);
}

bool isTextLike(const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".txt" || ext == ".log" || ext == ".json" || ext == ".csv";
}

bool findBytes(const juce::MemoryBlock& hay, const void* needle, size_t n)
{
    if (n == 0 || hay.getSize() < n) return false;
    const auto* h = static_cast<const char*>(hay.getData());
    const auto* e = h + hay.getSize();
    return std::search(h, e, static_cast<const char*>(needle), static_cast<const char*>(needle) + n) != e;
}

/// The token value, trimmed; empty when there is none (nothing to scrub).
juce::String readToken(const juce::File& configDir)
{
    const auto f = configDir.getChildFile(kTokenFileName);
    return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
}

struct Collector {
    juce::ZipFile::Builder zip;
    juce::StringArray manifest;
    juce::StringArray skipped;
    juce::String token;
    juce::int64 maxBytes = 0;
    int added = 0;
    // MemoryBlocks the builder streams from must outlive writeToStream().
    juce::OwnedArray<juce::MemoryBlock> keepAlive;

    void addBytes(const juce::String& path, const juce::MemoryBlock& data, const juce::String& note = {})
    {
        auto* block = keepAlive.add(new juce::MemoryBlock(data));
        zip.addEntry(std::make_unique<juce::MemoryInputStream>(*block, false), 9, path,
                     juce::Time::getCurrentTime());
        manifest.add(path + " (" + juce::String(static_cast<juce::int64>(data.getSize())) + " bytes)" +
                     (note.isNotEmpty() ? " - " + note : juce::String()));
        ++added;
    }

    void addText(const juce::String& path, const juce::String& text)
    {
        juce::MemoryBlock mb(text.toRawUTF8(), text.getNumBytesAsUTF8());
        if (token.isNotEmpty() && containsSecret(mb, token)) {
            const auto scrubbed = text.replace(token, kRedacted);
            addBytes(path, juce::MemoryBlock(scrubbed.toRawUTF8(), scrubbed.getNumBytesAsUTF8()),
                     "service token redacted");
            return;
        }
        addBytes(path, mb);
    }

    void addFile(const juce::File& f, const juce::String& path)
    {
        if (isTokenFile(f)) {
            skipped.add(path + ": the service token is never exported");
            return;
        }
        if (f.getSize() > maxBytes) {
            skipped.add(path + ": larger than the bundle's per-file limit");
            return;
        }
        juce::MemoryBlock data;
        if (!f.loadFileAsData(data)) {
            skipped.add(path + ": could not be read");
            return;
        }
        if (token.isNotEmpty() && containsSecret(data, token)) {
            if (isTextLike(f)) {
                addText(path, data.toString());
            } else {
                // A binary (a minidump of process memory) can't be scrubbed
                // safely - leave it out rather than risk the token.
                skipped.add(path + ": contains the service token and is binary, so it was left out");
            }
            return;
        }
        addBytes(path, data);
    }

    void addFolder(const juce::File& dir, const juce::String& prefix, const juce::String& pattern)
    {
        if (!dir.isDirectory()) return;
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, pattern))
            addFile(f, prefix + "/" + f.getFileName());
    }
};

} // namespace

bool containsSecret(const juce::MemoryBlock& data, const juce::String& secret)
{
    if (secret.isEmpty()) return false;
    const char* utf8 = secret.toRawUTF8();
    if (findBytes(data, utf8, std::strlen(utf8))) return true;
    // UTF-16LE (Windows wide strings in a dump).
    juce::MemoryBlock wide;
    for (auto p = secret.getCharPointer(); !p.isEmpty();) {
        const auto c = p.getAndAdvance();
        const auto lo = static_cast<char>(c & 0xff);
        const auto hi = static_cast<char>((c >> 8) & 0xff);
        wide.append(&lo, 1);
        wide.append(&hi, 1);
    }
    return findBytes(data, wide.getData(), wide.getSize());
}

juce::String describeVersions(const juce::String& appVersion, const juce::String& serviceVersion)
{
    juce::String s;
    s << "Khensu Mixing " << appVersion << "\n"
      << "Expected analysis service: " << serviceVersion << "\n"
      << "JUCE: " << juce::SystemStats::getJUCEVersion() << "\n"
      << "OS: " << juce::SystemStats::getOperatingSystemName() << (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "")
      << "\n"
      << "CPU: " << juce::SystemStats::getCpuVendor() << " " << juce::SystemStats::getCpuModel() << ", "
      << juce::SystemStats::getNumCpus() << " logical cores\n"
      << "Memory: " << juce::SystemStats::getMemorySizeInMegabytes() << " MB\n"
      << "Created: " << juce::Time::getCurrentTime().toISO8601(true) << "\n";
    return s;
}

BundleResult writeBundle(const BundleInputs& in, const juce::File& destination)
{
    BundleResult result;
    Collector c;
    c.token = readToken(in.configDir);
    c.maxBytes = in.maxFileBytes;

    c.addText("versions.txt", in.versionsText);
    if (!in.redactedSettings.isVoid())
        c.addText("settings.redacted.json", juce::JSON::toString(in.redactedSettings));
    for (const auto& [name, json] : in.serviceSnapshots) {
        if (json.isEmpty())
            c.skipped.add("service/" + name + ": the service did not answer");
        else
            c.addText("service/" + name, json);
    }

    // Crash records: every text record / log, plus the newest few dumps.
    if (in.crashesDir.isDirectory()) {
        for (const auto& f : in.crashesDir.findChildFiles(juce::File::findFiles, false, "*"))
            if (f.getFileExtension().toLowerCase() != ".dmp") c.addFile(f, "crashes/" + f.getFileName());
        auto dumps = in.crashesDir.findChildFiles(juce::File::findFiles, false, "*.dmp");
        std::sort(dumps.begin(), dumps.end(), [](const juce::File& a, const juce::File& b) {
            return a.getLastModificationTime() > b.getLastModificationTime();
        });
        for (int i = 0; i < dumps.size(); ++i) {
            if (i < in.maxDumps)
                c.addFile(dumps[i], "crashes/" + dumps[i].getFileName());
            else
                c.skipped.add("crashes/" + dumps[i].getFileName() + ": older than the newest " +
                              juce::String(in.maxDumps) + " dumps");
        }
    }
    c.addFolder(in.tracesDir, "traces", "*.json");
    c.addFolder(in.logsDir, "logs", "*");

    juce::String manifest;
    manifest << "Khensu diagnostics bundle - built locally, never uploaded.\n"
             << "The analysis service's token is never included (see DiagnosticsBundle.h).\n\nIncluded:\n";
    for (const auto& line : c.manifest) manifest << "  " << line << "\n";
    manifest << "\nLeft out:\n";
    for (const auto& line : c.skipped) manifest << "  " << line << "\n";
    c.addText("MANIFEST.txt", manifest);

    destination.getParentDirectory().createDirectory();
    const juce::TemporaryFile temp(destination);
    {
        juce::FileOutputStream out(temp.getFile());
        if (!out.openedOk()) {
            result.error = "could not create " + destination.getFullPathName();
            return result;
        }
        double progress = 0.0;
        if (!c.zip.writeToStream(out, &progress)) {
            result.error = "could not write the zip";
            return result;
        }
    }
    if (!temp.overwriteTargetFileWithTemporary()) {
        result.error = "could not move the zip into place";
        return result;
    }
    result.ok = true;
    result.file = destination;
    result.filesAdded = c.added;
    result.skipped = c.skipped;
    return result;
}

} // namespace khensu::diagnostics
