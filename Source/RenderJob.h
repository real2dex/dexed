// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../JuceLibraryCode/JuceHeader.h"
#include "RenderSpec.h"

class DexedAudioProcessor;

/**
 * One clip to render: which patch, which note, where to write it.
 *
 * Parsed from a line of the job file. Everything that is not given falls back
 * to the run-wide defaults, so a minimal job is just {"output": "..."}.
 */
struct RenderJob
{
    juce::File   output;

    juce::File   syx;             // empty = keep whatever patch is loaded
    int          program = -1;    // -1 = leave the current program alone

    // Normalised 0..1 parameter overrides, applied after the patch is loaded.
    juce::NamedValueSet params;

    RenderSpec   spec;

    /** Parses one JSON object. Returns an error string, empty on success. */
    static juce::String fromJson (const juce::var& json,
                                  const RenderSpec& defaults,
                                  RenderJob& out);
};

struct RenderResult
{
    int          index = 0;
    bool         ok    = false;
    juce::String error;
};

/**
 * Renders every job across `numThreads` worker threads and writes the WAVs.
 *
 * Each worker owns a private DexedAudioProcessor, so no locking is needed on
 * the render path; the msfa lookup tables they share are read-only by then.
 * `onResult` is called from the worker threads and must be thread safe.
 */
class BatchRenderer
{
public:
    BatchRenderer (int numThreads, double sampleRate, int blockSize, int engineType);
    ~BatchRenderer();

    void run (const juce::Array<RenderJob>& jobs,
              std::function<void (const RenderResult&)> onResult);

    /** Renders a single job on the calling thread, no workers involved. */
    static RenderResult renderOne (DexedAudioProcessor& proc, const RenderJob& job, int index);

    static juce::StringArray parameterNames();

private:
    int    numThreads;
    double sampleRate;
    int    blockSize;
    int    engineType;
};
