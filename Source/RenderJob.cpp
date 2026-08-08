// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderJob.h"
#include "PluginProcessor.h"

#include <atomic>
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

    bool writeWav (const juce::File& file,
                   const juce::AudioSampleBuffer& mono,
                   const RenderSpec& spec,
                   juce::String& error)
    {
        ensureDirectory (file.getParentDirectory());

        // FileOutputStream opens an existing file at its end, so the old
        // contents have to go first. Measured on Windows, deleting beats
        // opening and truncating by roughly 2x.
        file.deleteFile();

        auto stream = file.createOutputStream();
        if (stream == nullptr)
        {
            error = "cannot open for writing: " + file.getFullPathName();
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

        if (! ok)
            error = "write failed: " + file.getFullPathName();

        return ok;
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
}

BatchRenderer::~BatchRenderer() = default;

void BatchRenderer::run (const juce::Array<RenderJob>& jobs,
                         std::function<void (const RenderResult&)> onResult)
{
    const int total = jobs.size();
    if (total == 0)
        return;

    std::atomic<int> next { 0 };
    std::mutex resultMutex;

    auto worker = [&]
    {
        DexedAudioProcessor proc;
        proc.setEngineType (engineType);
        proc.prepareForOfflineRender (sampleRate, blockSize);

        for (;;)
        {
            const int i = next.fetch_add (1);
            if (i >= total)
                break;

            RenderResult result = renderOne (proc, jobs.getReference (i), i);

            const std::lock_guard<std::mutex> lock (resultMutex);
            onResult (result);
        }
    };

    const int workerCount = juce::jmin (numThreads, total);

    std::vector<std::thread> threads;
    threads.reserve ((size_t) workerCount - 1);
    for (int i = 1; i < workerCount; ++i)
        threads.emplace_back (worker);

    worker();   // the calling thread pulls its share too

    for (auto& t : threads)
        t.join();
}
