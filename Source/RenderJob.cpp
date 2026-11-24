// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderJob.h"
#include "CrashHandler.h"
#include "PluginProcessor.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
    /**
     * Parsed cartridges, keyed by absolute path.
     *
     * A dataset run typically points thousands of jobs at a handful of .syx
     * files. Reading and unpacking each one once turns that into a map lookup.
     * Cartridge is copied out under the lock, so workers never share one.
     */
    class CartridgeCache
    {
    public:
        bool get (const juce::File& file, Cartridge& out, juce::String& error)
        {
            const juce::String key = file.getFullPathName();
            const std::string  mapKey = key.toStdString();

            {
                const std::lock_guard<std::mutex> lock (mutex);
                auto it = entries.find (mapKey);
                if (it != entries.end())
                {
                    if (! it->second.ok)
                    {
                        error = it->second.error;
                        return false;
                    }
                    out = it->second.cart;
                    return true;
                }
            }

            Entry entry;
            if (! file.existsAsFile())
            {
                entry.ok = false;
                entry.error = "syx not found: " + key;
            }
            else if (entry.cart.load (file) == -1)
            {
                entry.ok = false;
                entry.error = "syx failed to load: " + key;
            }
            else
            {
                entry.ok = true;
            }

            const std::lock_guard<std::mutex> lock (mutex);
            auto& stored = entries.emplace (mapKey, std::move (entry)).first->second;
            if (! stored.ok)
            {
                error = stored.error;
                return false;
            }
            out = stored.cart;
            return true;
        }

    private:
        struct Entry
        {
            Cartridge     cart;
            bool          ok = false;
            juce::String  error;
        };

        std::mutex mutex;
        std::unordered_map<std::string, Entry> entries;
    };

    CartridgeCache& cartridgeCache()
    {
        static CartridgeCache cache;
        return cache;
    }

    /** Parameter name -> index, built once from a throwaway processor. */
    const std::unordered_map<std::string, int>& parameterIndex()
    {
        static const std::unordered_map<std::string, int> map = []
        {
            std::unordered_map<std::string, int> m;
            DexedAudioProcessor probe;
            for (int i = 0; i < probe.getNumParameters(); ++i)
                m[probe.getParameterName (i).toStdString()] = i;
            return m;
        }();
        return map;
    }

    /**
     * Creates each output directory once instead of once per clip.
     *
     * With short clips the run is bound by per-file syscalls rather than by
     * synthesis, so the redundant createDirectory() per job was costing real
     * throughput.
     */
    void ensureDirectory (const juce::File& dir)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, bool> seen;

        const std::string key = dir.getFullPathName().toStdString();

        const std::lock_guard<std::mutex> lock (mutex);
        if (seen.emplace (key, true).second)
            dir.createDirectory();
    }

    /**
     * Runs `attempt` until it succeeds, for up to about half a second.
     *
     * On Windows a file that was just written is often held open for a moment
     * by a scanner (Defender, the search indexer). Deleting or replacing it then
     * fails with a sharing violation, or leaves it delete-pending so a new file
     * of the same name cannot be created. Both clear up on their own.
     */
    template <typename Attempt>
    bool retryTransient (Attempt&& attempt)
    {
        for (int i = 0; i < 20; ++i)
        {
            if (attempt())
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        }
        return attempt();
    }

    std::filesystem::path toPath (const juce::File& file)
    {
       #if JUCE_WINDOWS
        return std::filesystem::path (file.getFullPathName().toWideCharPointer());
       #else
        return std::filesystem::path (file.getFullPathName().toRawUTF8());
       #endif
    }

    /**
     * Writes to <name>.tmp next to the target and renames it into place only
     * once it is complete, so a run that dies part way never leaves a
     * truncated file under the real name.
     */
    bool writeWav (const juce::File& file,
                   const juce::AudioSampleBuffer& mono,
                   const RenderSpec& spec,
                   juce::String& error)
    {
        ensureDirectory (file.getParentDirectory());

        const juce::File tmp = file.getSiblingFile (file.getFileName() + ".tmp");

        // FileOutputStream opens an existing file at its end, so a leftover
        // temp file has to go first. Measured on Windows, deleting beats
        // opening and truncating by roughly 2x.
        std::unique_ptr<juce::FileOutputStream> stream;
        const bool opened = retryTransient ([&]
        {
            tmp.deleteFile();
            stream = std::make_unique<juce::FileOutputStream> (tmp);
            return stream->openedOk();
        });

        if (! opened)
        {
            error = "cannot open for writing: " + tmp.getFullPathName()
                  + " (" + stream->getStatus().getErrorMessage() + ")";
            return false;
        }

        juce::WavAudioFormat wav;
        auto* raw = stream.get();
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (raw, spec.sampleRate, (unsigned int) spec.channels,
                                 spec.bitDepth, {}, 0));

        if (writer == nullptr)
        {
            error = "unsupported wav format: " + juce::String (spec.bitDepth) + " bit / "
                  + juce::String (spec.channels) + " ch";
            stream.reset();
            tmp.deleteFile();
            return false;
        }

        stream.release();   // the writer owns it now

        const int numSamples = mono.getNumSamples();
        bool ok;

        if (spec.channels == 1)
        {
            ok = writer->writeFromAudioSampleBuffer (mono, 0, numSamples);
        }
        else
        {
            // The DX7 is mono; a stereo request duplicates the channel.
            juce::AudioSampleBuffer wide (spec.channels, numSamples);
            for (int ch = 0; ch < spec.channels; ++ch)
                wide.copyFrom (ch, 0, mono, 0, 0, numSamples);
            ok = writer->writeFromAudioSampleBuffer (wide, 0, numSamples);
        }

        // Header and buffered samples out now, while the stream can still
        // report a failure (e.g. disk full); the destructor would swallow it.
        ok = ok && writer->flush();
        raw->flush();
        ok = ok && raw->getStatus().wasOk();
        writer.reset();

        if (! ok)
        {
            error = "write failed: " + tmp.getFullPathName();
            tmp.deleteFile();
            return false;
        }

        // Replaces an existing file of the same name (MoveFileExW with
        // MOVEFILE_REPLACE_EXISTING on Windows, rename(2) elsewhere).
        std::error_code ec;
        const auto from = toPath (tmp), to = toPath (file);
        if (! retryTransient ([&] { std::filesystem::rename (from, to, ec); return ! ec; }))
        {
            error = "cannot rename " + tmp.getFullPathName() + " to " + file.getFileName()
                  + " (error " + juce::String (ec.value()) + ")";
            tmp.deleteFile();
            return false;
        }

        return true;
    }
}

juce::StringArray BatchRenderer::parameterNames()
{
    juce::StringArray names;
    DexedAudioProcessor probe;
    for (int i = 0; i < probe.getNumParameters(); ++i)
        names.add (probe.getParameterName (i));
    return names;
}

juce::String RenderJob::fromJson (const juce::var& json,
                                  const RenderSpec& defaults,
                                  RenderJob& out)
{
    auto* obj = json.getDynamicObject();
    if (obj == nullptr)
        return "not a JSON object";

    out.spec = defaults;

    const juce::String outPath = obj->getProperty ("output").toString();
    if (outPath.isEmpty())
        return "missing 'output'";
    out.output = juce::File::getCurrentWorkingDirectory().getChildFile (outPath);

    if (obj->hasProperty ("syx"))
    {
        const juce::String syxPath = obj->getProperty ("syx").toString();
        if (syxPath.isNotEmpty())
            out.syx = juce::File::getCurrentWorkingDirectory().getChildFile (syxPath);
    }

    if (obj->hasProperty ("program"))
        out.program = juce::jlimit (0, 31, (int) obj->getProperty ("program"));

    if (obj->hasProperty ("note"))
        out.spec.midiNote = juce::jlimit (0, 127, (int) obj->getProperty ("note"));

    if (obj->hasProperty ("velocity"))
        out.spec.velocity = juce::jlimit (0.0f, 1.0f, (float) obj->getProperty ("velocity"));

    if (obj->hasProperty ("sound_beats"))
        out.spec.soundBeats = (double) obj->getProperty ("sound_beats");

    if (obj->hasProperty ("tail_beats"))
        out.spec.tailBeats = (double) obj->getProperty ("tail_beats");

    if (obj->hasProperty ("bpm"))
        out.spec.bpm = (double) obj->getProperty ("bpm");

    if (out.spec.bpm <= 0.0)
        return "'bpm' must be positive";
    if (out.spec.soundBeats < 0.0 || out.spec.tailBeats < 0.0)
        return "beat counts must not be negative";
    if (out.spec.totalSamples() <= 0)
        return "clip length is zero";

    if (auto* paramsObj = obj->getProperty ("params").getDynamicObject())
    {
        const auto& index = parameterIndex();
        for (auto& prop : paramsObj->getProperties())
        {
            const juce::String name = prop.name.toString();
            if (index.find (name.toStdString()) == index.end())
                return "unknown parameter: " + name;

            out.params.set (prop.name, juce::jlimit (0.0f, 1.0f, (float) prop.value));
        }
    }

    return {};
}

RenderResult BatchRenderer::renderOne (DexedAudioProcessor& proc, const RenderJob& job, int index)
{
    RenderResult result;
    result.index = index;

    // Reset the patch first so a job never inherits parameter overrides from
    // whichever job happened to run before it on this worker.
    if (job.syx != juce::File())
    {
        Cartridge cart;
        if (! cartridgeCache().get (job.syx, cart, result.error))
            return result;

        proc.loadCartridge (cart);
        proc.setCurrentProgram (job.program >= 0 ? job.program : 0);
    }
    else
    {
        proc.setCurrentProgram (job.program >= 0 ? job.program : proc.getCurrentProgram());
    }

    const auto& index_ = parameterIndex();
    for (int i = 0; i < job.params.size(); ++i)
    {
        auto it = index_.find (job.params.getName (i).toString().toStdString());
        if (it != index_.end())
            proc.setParameter (it->second, (float) job.params.getValueAt (i));
    }

    juce::AudioSampleBuffer buffer;
    proc.renderClip (job.spec, buffer);

    result.ok = writeWav (job.output, buffer, job.spec, result.error);
    return result;
}

BatchRenderer::BatchRenderer (int numThreads, double sampleRate, int blockSize, int engineType)
    : numThreads (juce::jmax (1, numThreads)),
      sampleRate (sampleRate),
      blockSize (blockSize),
      engineType (engineType)
{
    // Must happen before any worker exists: these tables are process-global and
    // read-only for the rest of the run.
    DexedAudioProcessor::initSharedTables (sampleRate);

    // renderOne() consults this; build it here rather than lazily on a worker,
    // where its probe processor would be constructed mid-render.
    parameterIndex();
}

BatchRenderer::~BatchRenderer() = default;

void BatchRenderer::run (const juce::Array<RenderJob>& jobs,
                         std::function<void (const RenderResult&)> onResult)
{
    const int total = jobs.size();
    if (total == 0)
        return;

    const int workerCount = juce::jmin (numThreads, total);

    // Every processor is built here, one after another, before any worker
    // starts, and destroyed after they have all joined. Construction is not
    // thread safe: it runs AudioProcessor's constructor and Dexed's own
    // setup, which writes process-wide statics (the app/cartridge
    // directories, the startup cartridge). Doing it concurrently on the
    // workers raced on those juce::File statics.
    std::vector<std::unique_ptr<DexedAudioProcessor>> procs;
    procs.reserve ((size_t) workerCount);
    for (int w = 0; w < workerCount; ++w)
    {
        auto proc = std::make_unique<DexedAudioProcessor>();
        proc->setEngineType (engineType);
        proc->prepareForOfflineRender (sampleRate, blockSize);
        procs.push_back (std::move (proc));
    }

    std::atomic<int> next { 0 };
    std::mutex resultMutex;

    auto worker = [&] (DexedAudioProcessor& proc)
    {
        CrashHandler::installForThread();

        for (;;)
        {
            const int i = next.fetch_add (1);
            if (i >= total)
                break;

            RenderResult result;
            result.index = i;

            // A job that throws (bad_alloc, a filesystem error, ...) fails on
            // its own instead of taking the whole batch down with terminate().
            try
            {
                result = renderOne (proc, jobs.getReference (i), i);
            }
            catch (const std::exception& e)
            {
                result.ok = false;
                result.error = juce::String ("exception: ") + e.what();
            }
            catch (...)
            {
                result.ok = false;
                result.error = "unknown exception";
            }

            // onResult failing is fatal, but end the process from inside the
            // catch so the crash report can still name the exception. Letting
            // it unwind would destroy still-joinable threads (on the calling
            // thread) and terminate without it.
            try
            {
                const std::lock_guard<std::mutex> lock (resultMutex);
                onResult (result);
            }
            catch (...)
            {
                std::terminate();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve ((size_t) workerCount - 1);
    for (int w = 1; w < workerCount; ++w)
        threads.emplace_back (worker, std::ref (*procs[(size_t) w]));

    worker (*procs[0]);   // the calling thread pulls its share too

    for (auto& t : threads)
        t.join();
}
