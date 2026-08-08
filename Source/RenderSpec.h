// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../JuceLibraryCode/JuceHeader.h"

/**
 * Geometry and format of one rendered clip.
 *
 * The defaults reproduce the original renderClipToFile() exactly: 120 BPM,
 * 4 beats of sound followed by a 4 beat tail, 48 kHz / 32-bit float / mono,
 * which is 192000 samples of 4.0 s. Existing datasets stay bit-compatible as
 * long as nothing here is overridden.
 */
struct RenderSpec
{
    double sampleRate  = 48000.0;
    int    blockSize   = 512;

    double bpm         = 120.0;
    double soundBeats  = 4.0;
    double tailBeats   = 4.0;

    int    bitDepth    = 32;   // 32 writes float samples, 16/24 write ints
    int    channels    = 1;

    int    midiNote    = 60;
    float  velocity    = 0.8f;

    /**
     * Stop running the FM voices once every carrier envelope has died and
     * feed silence to the filter instead. The filter still runs, so its
     * ringing is preserved and the output is bit-identical to a full render.
     */
    bool   skipDeadVoices = true;

    int soundSamples() const
    {
        return juce::roundToInt (soundBeats / (bpm / 60.0) * sampleRate);
    }

    int tailSamples() const
    {
        return juce::roundToInt (tailBeats / (bpm / 60.0) * sampleRate);
    }

    int totalSamples() const  { return soundSamples() + tailSamples(); }
};
