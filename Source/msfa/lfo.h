/*
 * Copyright 2013 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Low frequency oscillator, compatible with DX7

class Lfo {
public:
    static void init(double sample_rate);

    void reset(const uint8_t params[6]);

    // result is 0..1 in Q24
    int32_t getsample();

    // result is 0..1 in Q24
    int32_t getdelay();

    void keydown();

    // Rewinds the free-running state that reset() deliberately leaves alone.
    // The offline renderer calls this between clips so clip N+1 cannot inherit
    // the LFO phase clip N happened to stop at.
    void resetState() {
        phase_ = 0;
        randstate_ = 0;
        delaystate_ = 0;
        delaysaturated_ = false;
    }

    // --- offline renderer support ---------------------------------------
    // The offline renderer may stop computing the FM voices once it can prove
    // the output has become permanently silent. That proof needs to know how
    // long the LFO takes to repeat itself, since the LFO is the only thing
    // still moving after every envelope has frozen.

    // Length of one full LFO cycle in samples, or 0 when the phase alone does
    // not bound the cycle (sample & hold carries randstate_ across cycles, so
    // no finite observation window is conclusive).
    uint64_t periodSamples() const {
        if (waveform_ == 5 || delta_ == 0)
            return 0;
        const uint64_t chunks = (0x100000000ULL + delta_ - 1) / delta_;
        return chunks * N;
    }

    // The delay ramp only ever increases modulation depth, so a silent stretch
    // observed before it tops out proves nothing about later blocks.
    bool delayIsSaturated() const { return delaysaturated_; }

private:
    static uint32_t lforatio_;
    static uint32_t unit_;

    // reset() does not touch phase_/randstate_/delaystate_, and keydown() only
    // rewinds the phase when LFO KEY SYNC is on. In the plugin they therefore
    // carried whatever the free-running LFO had reached, which made a patch
    // with key sync off render differently every time. Offline rendering has to
    // be reproducible, so they start from a defined state.
    uint32_t phase_ = 0; // Q32
    uint32_t delta_ = 0;
    uint8_t waveform_ = 0;
    uint8_t randstate_ = 0;
    bool sync_ = false;

    uint32_t delaystate_ = 0;
    uint32_t delayinc_ = 0;
    uint32_t delayinc2_ = 0;
    bool delaysaturated_ = false;
};
