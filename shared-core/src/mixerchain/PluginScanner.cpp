#include <khensu/mixerchain/PluginScanner.h>
#include <khensu/common/AppPaths.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <mutex>
#include <thread>

namespace khensu::mixerchain {

class PluginScanner::ScanThread : public juce::Thread {
  public:
    ScanThread(std::function<void(const std::vector<ScannedPlugin>&)> onComplete,
               std::function<void(const std::string&)> onProgress, juce::File deadMansPedal,
               juce::String workerExe)
        : Thread("KhensuPluginScanner"), onComplete_(std::move(onComplete)), onProgress_(std::move(onProgress)),
          deadMansPedal_(std::move(deadMansPedal)), workerExe_(std::move(workerExe))
    {}

    ~ScanThread() override { stopThread(2000); }

    /// A retry (startRetryQuarantined): probe only `files`, one at a time,
    /// allowing `timeoutMs` each. Call before startThread().
    void setRetry(juce::StringArray files, int timeoutMs)
    {
        retryFiles_ = std::move(files);
        timeoutMs_ = timeoutMs;
    }

    void run() override
    {
        juce::AudioPluginFormatManager formatManager;
        juce::addDefaultFormatsToManager(formatManager);

        juce::KnownPluginList pluginList;
        std::vector<ScannedPlugin> results;

        // Files a previous scan crashed on (still listed in the pedal file).
        const juce::StringArray quarantined = readPedal(deadMansPedal_);
        for (const auto& f : quarantined)
            pluginList.addToBlacklist(f);

        for (int i = 0; i < formatManager.getNumFormats(); ++i) {
            if (threadShouldExit()) break;

            auto* format = formatManager.getFormat(i);
            if (format == nullptr) continue;

            juce::String formatName = format->getName();
            // We only care about VST3 and AudioUnit (on macOS)
            if (formatName != "VST3" && formatName != "AudioUnit") continue;

            juce::FileSearchPath searchPaths = format->getDefaultLocationsToSearch();

#if JUCE_WINDOWS
            if (formatName == "VST3" && searchPaths.getNumPaths() == 0) {
                // Common Windows search directories
                searchPaths.add(juce::File("C:\\Program Files\\Common Files\\VST3"));
                searchPaths.add(juce::File("C:\\Program Files (x86)\\Common Files\\VST3"));
            }
#endif

            // The pedal file (CORE-07): JUCE records each file before loading
            // it and removes it after a clean load, so a crash leaves the
            // culprit listed. JUCE itself only moves such files to the END of
            // the queue - they would still be loaded, and crash the app on
            // every rescan - so quarantined files are dropped from the queue.
            juce::PluginDirectoryScanner scanner(pluginList, *format, searchPaths, true, deadMansPedal_);
            juce::StringArray toScan;
            if (retryFiles_.isEmpty()) {
                toScan = format->searchPathsForPlugins(searchPaths, true, false);
            } else {
                for (const auto& f : retryFiles_)
                    if (format->fileMightContainThisPluginType(f)) toScan.add(f);
            }
            for (const auto& f : quarantined) {
                if (!toScan.contains(f)) continue;
                toScan.removeString(f);
                if (onProgress_)
                    onProgress_(("Skipped " + f +
                                 " - it crashed during a previous scan (clear the quarantine to retry it)")
                                    .toStdString());
            }
            if (workerExe_.isNotEmpty()) {
                // CORE-07: probe in worker processes, not in the app.
                // Several workers side by side, the files dealt out in turn, so
                // plugins that wait out the hang timeout (an authorisation
                // window nobody answers) do not queue behind one another.
                // A retry runs one worker, so licence windows come one at a time.
                const int processes = retryFiles_.isEmpty() ? kWorkerProcesses : 1;
                std::vector<juce::StringArray> shards(static_cast<size_t>(processes));
                for (int n = 0; n < toScan.size(); ++n)
                    shards[static_cast<size_t>(n % processes)].add(toScan[n]);
                std::vector<std::thread> workers;
                for (const auto& shard : shards)
                    if (!shard.isEmpty())
                        workers.emplace_back([this, &shard, &formatName, &pluginList] {
                            scanInWorkers(shard, formatName, pluginList);
                        });
                for (auto& w : workers)
                    w.join();
                continue;
            }
            scanner.setFilesOrIdentifiersToScan(toScan);

            juce::String pluginName;
            while (!threadShouldExit()) {
                if (onProgress_ && !pluginName.isEmpty()) {
                    juce::String progressMsg = "Scanning " + formatName + ": " + pluginName;
                    onProgress_(progressMsg.toStdString());
                }

                if (!scanner.scanNextFile(true, pluginName)) {
                    break;
                }
            }
        }

        if (threadShouldExit()) return;

        // Collect all successfully scanned plugin descriptions
        for (const auto& desc : pluginList.getTypes()) {
            ScannedPlugin sp;
            sp.id = desc.createIdentifierString().toStdString();
            sp.name = desc.name.toStdString();
            sp.manufacturer = desc.manufacturerName.toStdString();
            sp.category = desc.category.toStdString();
            sp.format = desc.pluginFormatName.toStdString();
            sp.file_path = desc.fileOrIdentifier.toStdString();
            results.push_back(sp);
        }

        if (!threadShouldExit() && onComplete_) {
            onComplete_(results);
        }
    }

    static juce::StringArray readPedal(const juce::File& pedal)
    {
        juce::StringArray lines;
        if (pedal.existsAsFile()) pedal.readLines(lines);
        lines.trim();
        lines.removeEmptyStrings();
        lines.removeDuplicates(true);
        return lines;
    }

  private:
    /// The files in `toScan` through worker processes (CORE-07). One worker
    /// probes the files in order, writing `current.txt` (the index it is on)
    /// before each and `<index>.xml` after it. When a worker crashes or the
    /// index stops moving for timeoutMs_, the file it was on is
    /// quarantined and a new worker carries on after it - so a library costs
    /// one process start per crash, not one per plugin.
    void scanInWorkers(const juce::StringArray& toScan, const juce::String& formatName, juce::KnownPluginList& list)
    {
        int next = 0;
        while (next < toScan.size() && !threadShouldExit()) {
            const auto dir = juce::File::createTempFile("khensu-scan");
            dir.createDirectory();
            juce::StringArray batch;
            for (int i = next; i < toScan.size(); ++i)
                batch.add(toScan[i]);
            const auto listFile = dir.getChildFile("list.txt");
            const auto currentFile = dir.getChildFile("current.txt");
            listFile.replaceWithText(batch.joinIntoString("\n"), false, false, "\n");

            juce::ChildProcess child;
            const juce::StringArray args{workerExe_, juce::String(kWorkerListArg) + listFile.getFullPathName(),
                                         juce::String(kWorkerFormatArg) + formatName,
                                         juce::String(kWorkerOutArg) + dir.getFullPathName()};
            if (!child.start(args, 0)) {
                if (onProgress_) onProgress_("Plugin scan stopped: the scan worker did not start");
                dir.deleteRecursively();
                return;
            }
            int current = -1;
            auto lastMove = juce::Time::getMillisecondCounter();
            bool hung = false;
            while (!child.waitForProcessToFinish(250)) {
                if (threadShouldExit()) {
                    child.kill();
                    dir.deleteRecursively();
                    return;
                }
                const int now = currentFile.existsAsFile() ? currentFile.loadFileAsString().trim().getIntValue() : -1;
                if (now != current) {
                    current = now;
                    lastMove = juce::Time::getMillisecondCounter();
                    if (onProgress_ && current >= 0 && current < batch.size())
                        onProgress_(("Scanning " + formatName + ": " + juce::File(batch[current]).getFileName())
                                        .toStdString());
                } else if (juce::Time::getMillisecondCounter() - lastMove >
                           static_cast<juce::uint32>(timeoutMs_)) {
                    child.kill();
                    hung = true;
                    break;
                }
            }
            const bool finished = !hung && child.getExitCode() == 0 && dir.getChildFile("done").existsAsFile();
            if (currentFile.existsAsFile()) current = currentFile.loadFileAsString().trim().getIntValue();

            int lastDone = -1;
            for (int i = 0; i < batch.size(); ++i) {
                const auto xmlFile = dir.getChildFile(juce::String(i) + ".xml");
                if (!xmlFile.existsAsFile()) continue;
                lastDone = i;
                if (auto xml = juce::XmlDocument::parse(xmlFile))
                    for (auto* e : xml->getChildIterator()) {
                        juce::PluginDescription desc;
                        if (desc.loadFromXml(*e)) list.addType(desc);
                    }
            }
            dir.deleteRecursively();
            if (finished) return;

            // The file it was on, if that file never finished, is the culprit.
            const int culprit = current > lastDone ? current : -1;
            if (culprit >= 0) {
                const juce::String file = batch[culprit];
                // Quarantined like a pedal entry: never loaded again until cleared.
                if (deadMansPedal_ != juce::File()) {
                    const std::lock_guard<std::mutex> lock(pedalMutex_);
                    deadMansPedal_.appendText(file + "\n", false, false, "\n");
                }
                const juce::String why = hung ? "it did not finish loading in " + juce::String(timeoutMs_ / 1000) +
                                                    " s - it may be waiting on a licence or sign-in window"
                                              : juce::String("it crashed while being probed");
                if (onProgress_)
                    onProgress_(("Skipped " + file + " - " + why +
                                 " (it ran in a separate process, so the app kept running; clear the quarantine to "
                                 "retry it)")
                                    .toStdString());
            }
            // Always move forward: past the culprit, or past the last file finished.
            next += juce::jmax(culprit, lastDone, current) + 1;
        }
    }

    std::function<void(const std::vector<ScannedPlugin>&)> onComplete_;
    std::function<void(const std::string&)> onProgress_;
    juce::File deadMansPedal_;
    juce::String workerExe_;
    juce::StringArray retryFiles_; ///< empty: a full scan
    int timeoutMs_ = kWorkerTimeoutMs;
    std::mutex pedalMutex_; ///< the side-by-side workers share the pedal file
};

PluginScanner::PluginScanner() : PluginScanner(defaultDeadMansPedalFile()) {}

PluginScanner::PluginScanner(std::string deadMansPedalPath) : deadMansPedalPath_(std::move(deadMansPedalPath)) {}

std::string PluginScanner::defaultDeadMansPedalFile()
{
    return (juce::File(juce::String::fromUTF8(AppPaths::dataDirectory().c_str()))
                .getChildFile("plugin_scan_deadmans_pedal.txt"))
        .getFullPathName()
        .toStdString();
}

std::vector<std::string> PluginScanner::quarantinedPlugins() const
{
    std::vector<std::string> out;
    if (deadMansPedalPath_.empty()) return out;
    for (const auto& f : ScanThread::readPedal(juce::File(juce::String::fromUTF8(deadMansPedalPath_.c_str()))))
        out.push_back(f.toStdString());
    return out;
}

bool PluginScanner::clearQuarantine() const
{
    if (deadMansPedalPath_.empty()) return true;
    const juce::File pedal(juce::String::fromUTF8(deadMansPedalPath_.c_str()));
    return !pedal.existsAsFile() || pedal.deleteFile();
}

PluginScanner::~PluginScanner() { cancelScan(); }

void PluginScanner::startScan(std::function<void(const std::vector<ScannedPlugin>&)> onComplete,
                              std::function<void(const std::string&)> onProgress)
{
    cancelScan();

    const juce::File pedal = deadMansPedalPath_.empty() ? juce::File()
                                                        : juce::File(juce::String::fromUTF8(deadMansPedalPath_.c_str()));
    if (pedal != juce::File()) pedal.getParentDirectory().createDirectory();
    scanThread_ = std::make_unique<ScanThread>(std::move(onComplete), std::move(onProgress), pedal,
                                               juce::String::fromUTF8(workerExe_.c_str()));
    scanThread_->startThread();
}

bool PluginScanner::startRetryQuarantined(std::function<void(const std::vector<ScannedPlugin>&)> onComplete,
                                          std::function<void(const std::string&)> onProgress)
{
    cancelScan();
    juce::StringArray files;
    for (const auto& f : quarantinedPlugins())
        files.add(juce::String::fromUTF8(f.c_str()));
    if (files.isEmpty() || !clearQuarantine()) return false;
    const juce::File pedal = deadMansPedalPath_.empty() ? juce::File()
                                                        : juce::File(juce::String::fromUTF8(deadMansPedalPath_.c_str()));
    if (pedal != juce::File()) pedal.getParentDirectory().createDirectory();
    scanThread_ = std::make_unique<ScanThread>(std::move(onComplete), std::move(onProgress), pedal,
                                               juce::String::fromUTF8(workerExe_.c_str()));
    scanThread_->setRetry(files, kRetryTimeoutMs);
    scanThread_->startThread();
    return true;
}

void PluginScanner::cancelScan()
{
    if (scanThread_ != nullptr) {
        scanThread_->stopThread(2000);
        scanThread_.reset();
    }
}

bool PluginScanner::isScanWorkerRequest(const std::vector<std::string>& args)
{
    for (const auto& a : args)
        if (a.rfind(kWorkerListArg, 0) == 0) return true;
    return false;
}

int PluginScanner::runScanWorker(const std::vector<std::string>& args)
{
    juce::String listPath, formatName, outDir;
    for (const auto& a : args) {
        const juce::String s = juce::String::fromUTF8(a.c_str());
        if (s.startsWith(kWorkerListArg)) listPath = s.fromFirstOccurrenceOf(kWorkerListArg, false, false);
        else if (s.startsWith(kWorkerFormatArg)) formatName = s.fromFirstOccurrenceOf(kWorkerFormatArg, false, false);
        else if (s.startsWith(kWorkerOutArg)) outDir = s.fromFirstOccurrenceOf(kWorkerOutArg, false, false);
    }
    if (listPath.isEmpty() || outDir.isEmpty()) return 2;
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (formats.getFormat(i)->getName() == formatName) format = formats.getFormat(i);
    if (format == nullptr) return 3;
    juce::StringArray files;
    juce::File(listPath).readLines(files);
    const juce::File dir(outDir);
    for (int i = 0; i < files.size(); ++i) {
        const auto file = files[i].trim();
        if (file.isEmpty()) continue;
        // Written before the probe, so a crash or hang names this file.
        if (!dir.getChildFile("current.txt").replaceWithText(juce::String(i))) return 4;
        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile(found, file);
        juce::XmlElement root("PLUGINS");
        for (auto* d : found)
            if (auto x = d->createXml()) root.addChildElement(x.release());
        if (!root.writeTo(dir.getChildFile(juce::String(i) + ".xml"))) return 4;
    }
    return dir.getChildFile("done").replaceWithText("ok") ? 0 : 4;
}

bool PluginScanner::isScanning() const { return scanThread_ != nullptr && scanThread_->isThreadRunning(); }

} // namespace khensu::mixerchain
