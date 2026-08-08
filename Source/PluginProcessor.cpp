/**
 *
 * Copyright (c) 2013-2025 Pascal Gauthier.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 */

#include <stdarg.h>
#include <bitset>

#include "PluginProcessor.h"

#include "Dexed.h"
#include "msfa/synth.h"
#include "msfa/freqlut.h"
#include "msfa/sin.h"
#include "msfa/exp2.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include "msfa/aligned_buf.h"
#include "msfa/fm_op_kernel.h"

#if JUCE_MSVC
    #pragma comment (lib, "kernel32.lib")
    #pragma comment (lib, "user32.lib")
    #pragma comment (lib, "wininet.lib")
    #pragma comment (lib, "advapi32.lib")
    #pragma comment (lib, "ws2_32.lib")
    #pragma comment (lib, "version.lib")
    #pragma comment (lib, "shlwapi.lib")
    #pragma comment (lib, "winmm.lib")
	#pragma comment (lib, "DbgHelp.lib")
	#pragma comment (lib, "Imm32.lib")

	#ifdef _NATIVE_WCHAR_T_DEFINED
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppwd.lib")
		#else
			#pragma comment (lib, "comsuppw.lib")
		#endif
	#else
		#ifdef _DEBUG
			#pragma comment (lib, "comsuppd.lib")
		#else
			#pragma comment (lib, "comsupp.lib")
		#endif
	#endif

#endif

//==============================================================================
DexedAudioProcessor::DexedAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("output", AudioChannelSet::stereo(), true)) {
    // Exp2/Tanh/Sin fill process-global tables that do not depend on the sample
    // rate. They are idempotent, but writing them while another thread renders
    // would be a data race, so initSharedTables() must have run first. Calling
    // it here as well keeps a single-instance use (tests, one-shot render)
    // correct without any ceremony.
    Exp2::init();
    Tanh::init();
    Sin::init();

    synthTuningState = createStandardTuning();
    synthTuningStateLast = createStandardTuning();

    lastStateSave = 0;
    currentNote = -1;
    engineType = -1;

    monoMode = 0;

    resolvAppDir();

    initCtrl();
    normalizeDxVelocity = false;

    memset(&voiceStatus, 0, sizeof(VoiceStatus));
    setEngineType(DEXED_ENGINE_MARKI);

    controllers.values_[kControllerPitchRangeUp] = 3;
    controllers.values_[kControllerPitchRangeDn] = 3;
    controllers.values_[kControllerPitchStep] = 0;
    controllers.masterTune = 0;

    // Deliberately no loadPreference() here. A renderer that silently changes
    // its output based on a Dexed.xml left over in the user's home directory is
    // useless for building datasets. The CLI sets everything it cares about.

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        voices[note].dx7_note = NULL;
    }
    setCurrentProgram(0);
    nextMidi = NULL;
    midiMsg = NULL;

    // MTS-ESP is a live retuning IPC service; there is no master to talk to in
    // a batch render and it would make output depend on the host environment.
    // Every MTS_* call is null-safe and falls back to standard tuning.
    mtsClient = NULL;
}

DexedAudioProcessor::~DexedAudioProcessor() {
    Logger *tmp = Logger::getCurrentLogger();
	if ( tmp != NULL ) {
		Logger::setCurrentLogger(NULL);
		delete tmp;
	}
    TRACE("Bye");
    if (mtsClient) MTS_DeregisterClient(mtsClient);
}

//==============================================================================
// Freqlut/Lfo/PitchEnv/Env/Porta keep their tables in process-global statics
// that are only a function of the sample rate. Rebuilding them per instance or
// per clip is pure waste, and doing it while another worker renders is a data
// race, so the CLI calls this exactly once up front and never again.
void DexedAudioProcessor::initSharedTables(double sampleRate) {
    Exp2::init();
    Tanh::init();
    Sin::init();
    Freqlut::init(sampleRate);
    Lfo::init(sampleRate);
    PitchEnv::init(sampleRate);
    Env::init_sr(sampleRate);
    Porta::init_sr(sampleRate);
}

// Per-instance state only. Safe to call from a worker thread once
// initSharedTables() has run.
void DexedAudioProcessor::prepareForOfflineRender(double sampleRate, int blockSize) {
    setRateAndBufferSizeDetails(sampleRate, blockSize);

    fx.init(sampleRate);

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        // The original prepareToPlay() overwrote this pointer without freeing
        // it, leaking 16 Dx7Note on every call.
        delete voices[note].dx7_note;
        voices[note].dx7_note = new Dx7Note(synthTuningState, mtsClient);
        voices[note].midi_note = -1;
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
        voices[note].keydown_seq = -1;
    }

    currentNote = 0;
    nextKeydownSeq = 0;
    controllers.values_[kControllerPitch] = 0x2000;
    controllers.modwheel_cc = 0;
    controllers.foot_cc = 0;
    controllers.breath_cc = 0;
    controllers.aftertouch_cc = 0;
    controllers.portamento_enable_cc = false;
    controllers.portamento_cc = 0;
	controllers.refresh();

    sustain = false;
    extra_buf_size = 0;

    lfo.reset(data + 137);

    delete nextMidi;
    delete midiMsg;
    nextMidi = new MidiMessage(0xF0);
	midiMsg = new MidiMessage(0xF0);
}

void DexedAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    initSharedTables(sampleRate);
    prepareForOfflineRender(sampleRate, samplesPerBlock);
}

void DexedAudioProcessor::releaseResources() {
    currentNote = -1;

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        if ( voices[note].dx7_note != NULL ) {
            delete voices[note].dx7_note;
            voices[note].dx7_note = NULL;
        }
        voices[note].keydown = false;
        voices[note].sustained = false;
        voices[note].live = false;
    }

    if ( nextMidi != NULL ) {
        delete nextMidi;
        nextMidi = NULL;
    }
    if ( midiMsg != NULL ) {
        delete midiMsg;
        midiMsg = NULL;
    }
}

void DexedAudioProcessor::processBlock(AudioSampleBuffer& buffer, MidiBuffer& midiMessages) {

    juce::ScopedNoDenormals noDenormals;

    int numSamples = buffer.getNumSamples();
    int i;

    if ( refreshVoice ) {
        for(i=0;i < MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].live )
                voices[i].dx7_note->update(data, voices[i].midi_note, voices[i].velocity, voices[i].channel);
        }
        lfo.reset(data + 137);
        refreshVoice = false;
        fmPermanentlySilent = false;
        silentChunkSamples = 0;
    }

    MidiBuffer::Iterator it(midiMessages);
    hasMidiMessage = it.getNextEvent(*nextMidi,midiEventPos);

    float *channelData = buffer.getWritePointer(0);
  
    // flush first events
    for (i=0; i < numSamples && i < extra_buf_size; i++) {
        channelData[i] = extra_buf[i];
    }
    
    // remaining buffer is still to be processed
    if (extra_buf_size > numSamples) {
        for (int j = 0; j < extra_buf_size - numSamples; j++) {
            extra_buf[j] = extra_buf[j + numSamples];
        }
        extra_buf_size -= numSamples;
        
        // flush the events, they will be process in the next cycle
        while(getNextEvent(&it, numSamples)) {
            processMidiMessage(midiMsg);
        }
    } else {
        for (; i < numSamples; i += N) {
            AlignedBuf<int32_t, N> audiobuf;
            float sumbuf[N];

            while(getNextEvent(&it, i)) {
                processMidiMessage(midiMsg);
            }

            for (int j = 0; j < N; ++j) {
                audiobuf.get()[j] = 0;
                sumbuf[j] = 0;
            }

            if (fmPermanentlySilent) {
                // Proven below to stay exactly zero, so sumbuf stays zeroed and
                // the whole FM path is skipped. The filter below still runs on
                // these zeros, which is what keeps the output bit-identical.
            } else {
            int32_t lfovalue = lfo.getsample();
            int32_t lfodelay = lfo.getdelay();

            bool checkMTSESPRetuning = synthTuningState->is_standard_tuning() &&
                                        MTS_HasMaster(mtsClient);

            for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
                if (voices[note].live) {

                    if (checkMTSESPRetuning)
                        voices[note].dx7_note->updateBasePitches();

                    voices[note].dx7_note->compute(audiobuf.get(), lfovalue, lfodelay, &controllers);

                    for (int j=0; j < N; ++j) {
                        int32_t val = audiobuf.get()[j];

                        val = val >> 4;
                        int clip_val = val < -(1 << 24) ? 0x8000 : val >= (1 << 24) ? 0x7fff : val >> 9;
                        float f = ((float) clip_val) / (float) 0x8000;
                        if( f > 1 ) f = 1;
                        if( f < -1 ) f = -1;
                        sumbuf[j] += f;
                        audiobuf.get()[j] = 0;
                    }
                }
            }

            if (skipDeadVoices)
                trackFmSilence(sumbuf);
            }

            int jmax = numSamples - i;
            for (int j = 0; j < N; ++j) {
                if (j < jmax) {
                    channelData[i + j] = sumbuf[j];
                } else {
                    extra_buf[j - jmax] = sumbuf[j];
                }
            }
        }
        extra_buf_size = i - numSamples;
    }

    while(getNextEvent(&it, numSamples)) {
        processMidiMessage(midiMsg);
    }

    fx.process(channelData, numSamples);

    // DX7 is a mono synth, but copy it to the right channel is available
    if ( buffer.getNumChannels() > 1 )
        buffer.copyFrom(1, 0, channelData, numSamples, 1);
}

/**
 * Decide whether the FM engine can be switched off for the rest of the clip.
 *
 * Once every carrier envelope has gone inactive the envelopes are frozen: for
 * ix_ >= 4 Env::getsample() stops advancing, so operator gains only still move
 * because of the LFO. The LFO repeats every periodSamples(), so an observed run
 * of exactly-zero output that covers a full LFO cycle (plus one chunk, so the
 * previous-gain term in fm_op_kernel has settled too) means every subsequent
 * cycle produces exactly the same zeros.
 *
 * The conservative cases -- sample & hold, an LFO delay ramp that has not
 * topped out yet -- simply never arm the flag and render in full.
 */
void DexedAudioProcessor::trackFmSilence(const float* sumbuf) {
    for (int j = 0; j < N; ++j) {
        if (sumbuf[j] != 0.0f) {
            silentChunkSamples = 0;
            return;
        }
    }

    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note) {
        if (voices[note].live && voices[note].dx7_note->isPlaying()) {
            silentChunkSamples = 0;
            return;
        }
    }

    const uint64_t period = lfo.periodSamples();
    if (period == 0 || !lfo.delayIsSaturated()) {
        silentChunkSamples = 0;
        return;
    }

    silentChunkSamples += N;
    if (silentChunkSamples >= period + N)
        fmPermanentlySilent = true;
}

bool DexedAudioProcessor::getNextEvent(MidiBuffer::Iterator* iter,const int samplePos) {
	if (hasMidiMessage && midiEventPos <= samplePos) {
		*midiMsg = *nextMidi;
		hasMidiMessage = iter->getNextEvent(*nextMidi, midiEventPos);
		return true;
	}
	return false;
}

void DexedAudioProcessor::processMidiMessage(const MidiMessage *msg) {
    if ( msg->isSysEx() ) {
        handleIncomingMidiMessage(*msg);
        return;
    }

    const uint8 *buf  = msg->getRawData();
    uint8_t cmd = buf[0];
    uint8_t cf0 = cmd & 0xf0;
    auto channel = msg->getChannel();


    if( controllers.mpeEnabled && channel != 1 &&
        (
            (cf0 == 0xb0 && buf[1] == 74 ) || //timbre
            (cf0 == 0xd0 ) || // aftertouch
            (cf0 == 0xe0 ) // pb
            )
        )
    {
        // OK so find my voice index
        int voiceIndex = -1;
        for( int i=0; i<MAX_ACTIVE_NOTES; ++i )
        {
            if( voices[i].keydown && voices[i].channel == channel )
            {
                voiceIndex = i;
                break;
            }
        }
        if( voiceIndex >= 0 )
        {
            int i = voiceIndex;
            switch(cf0) {
            // THIS IS COMMENTED SINCE mepTimbre and mpePressure is not used
            // case 0xb0:
            //     voices[i].mpeTimbre = (int)buf[2];
            //     voices[i].dx7_note->mpeTimbre = (int)buf[2];
            //     break;
            // case 0xd0:
            //     voices[i].mpePressure = (int)buf[1];
            //     voices[i].dx7_note->mpePressure = (int)buf[1];
            //     break;
            case 0xe0:
                voices[i].mpePitchBend = (int)( buf[1] | (buf[2] << 7) );
                voices[i].dx7_note->mpePitchBend = (int)( buf[1] | ( buf[2] << 7 ) );
                break;
            }
        }
    }
    else
    {
        switch(cmd & 0xf0) {
        case 0x80 :
            keyup(channel, buf[1], buf[2]);
        return;

        case 0x90 :
            if (!synthTuningState->is_standard_tuning() || !buf[2] ||
                !MTS_HasMaster(mtsClient) || !MTS_ShouldFilterNote(mtsClient, buf[1], channel - 1))
                keydown(channel, buf[1], buf[2]);
        return;
            
        case 0xb0 : {
            int ctrl = buf[1];
            int value = buf[2];
                switch(ctrl) {
                case 1:
                    controllers.modwheel_cc = value;
                    controllers.refresh();
                    break;
                case 2:
                    controllers.breath_cc = value;
                    controllers.refresh();
                    break;
                case 4:
                    controllers.foot_cc = value;
                    controllers.refresh();
                    break;
                case 5:
                    controllers.portamento_cc = value;
                    break;
                case 64:
                    sustain = value > 63;
                    if (!sustain) {
                        for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                            if (voices[note].sustained && !voices[note].keydown) {
                                voices[note].dx7_note->keyup();
                                voices[note].sustained = false;
                            }
                        }
                    }
                    break;
                case 65:
                    controllers.portamento_enable_cc = value >= 64;
                    break;
                case 120:
                    panic();
                    break;
                case 123:
                    for (int note = 0; note < MAX_ACTIVE_NOTES; note++) {
                        if (voices[note].keydown)
                            keyup(channel, voices[note].midi_note, 0);
                    }
                    break;
                default:
                    TRACE("handle channel %d CC %d = %d", channel, ctrl, value);
                    int channel_cc = (channel << 8) | ctrl;
                    if ( mappedMidiCC.contains(channel_cc) ) {
                        // Rendering is single threaded, so unlike the plugin
                        // there is no DSP/event thread split to defer across.
                        mappedMidiCC[channel_cc]->setValueHost((float) value / 127);
                    }
                }
            }
            return;
            
        case 0xc0 :
            setCurrentProgram(buf[1]);
            return;
            
        case 0xd0 :
            controllers.aftertouch_cc = buf[1];
            controllers.refresh();
            return;
        
		case 0xe0 :
			controllers.values_[kControllerPitch] = buf[1] | (buf[2] << 7);
            return;
        }
    }
}

#define ACT(v) (v.keydown ? v.midi_note : -1)

int DexedAudioProcessor::chooseNote(uint8_t pitch) {
    // order of preference:
    // 1. a note that is not playing
    // 2. a note with its key up, playing the same pitch
    // 3. a note with its key up, playing a different pitch
    // 4. a note with its key down, playing the same pitch
    // 5. a note with its key down, playing a different pitch
    // break ties by preferring note with least recent keydown
    int bestNote = currentNote;
    int bestScore = -1;
    int note = currentNote;
    for (int i=0; i<MAX_ACTIVE_NOTES; i++) {
        int score = 0;
        if ( !voices[note].dx7_note->isPlaying() ) score += 4;
        if ( !voices[note].keydown ) score += 2;
        if ( voices[note].midi_note == pitch ) score += 1;
        if ( (score > bestScore) || (score == bestScore && voices[note].keydown_seq < voices[bestNote].keydown_seq) ) {
            bestNote = note;
            bestScore = score;
        }
        note = (note + 1) % MAX_ACTIVE_NOTES;
    }
    return bestNote;
}

void DexedAudioProcessor::keydown(uint8_t channel, uint8_t pitch, uint8_t velo) {
    if ( velo == 0 ) {
        keyup(channel, pitch, velo);
        return;
    }

    pitch += tuningTranspositionShift();
    
    if ( normalizeDxVelocity ) {
        velo = ((float)velo) * 0.7874015; // 100/127
    }
  

    if( controllers.mpeEnabled ) {
        int note = currentNote;
        for( int i=0; i<MAX_ACTIVE_NOTES; ++i ) {
            if( voices[note].keydown && voices[note].channel == channel )
            {
                // If we get two keydowns on the same channel we are getting information from a non-mpe device
                controllers.mpeEnabled = false;
            }
            note = (note + 1) % MAX_ACTIVE_NOTES;
        }
    }

    bool triggerLfo = true;
    for (int i=0; i<MAX_ACTIVE_NOTES; i++) {
        if ( voices[i].keydown ) {
            triggerLfo = false;
            break;
        }
    }
    if ( triggerLfo ) {
        lfo.keydown();
    }

    int note = chooseNote(pitch);

    currentNote = (note + 1) % MAX_ACTIVE_NOTES;
    voices[note].channel = channel;
    voices[note].midi_note = pitch;
    voices[note].velocity = velo;
    voices[note].sustained = sustain;
    voices[note].keydown = true;
    voices[note].keydown_seq = nextKeydownSeq++;
    // to avoid click, don't sync oscillators when voice stealing
    bool voice_steal = voices[note].dx7_note->isPlaying();
    voices[note].dx7_note->init(data, pitch, velo, channel, &controllers);
    if ( data[136] && !voice_steal ) {
        voices[note].dx7_note->oscSync();
    }
    if ( (voices[lastActiveVoice].midi_note != -1 && controllers.portamento_enable_cc)
       && controllers.portamento_cc > 0 ) {
        voices[note].dx7_note->initPortamento(*voices[lastActiveVoice].dx7_note);
    }

    if ( monoMode ) {
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {            
            if ( voices[i].live ) {
                // all keys are up, only transfer signal
                if ( ! voices[i].keydown ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferSignal(*voices[i].dx7_note);
                    break;
                }
                if ( voices[i].midi_note < pitch ) {
                    voices[i].live = false;
                    voices[note].dx7_note->transferState(*voices[i].dx7_note);
                    break;
                }
                return;
            }
        }
    }
    else if ( !data[136] ) {
        // if another note at the same pitch is playing, transfer phase
        // to avoid unpredictable destructive interference. this can cause
        // clicking when voice stealing, but we've tried to choose a voice
        // to steal that will minimise the chances of clicking
        for(int i=0; i<MAX_ACTIVE_NOTES; i++) {
            if ( i != note && voices[i].dx7_note->isPlaying() && voices[i].midi_note == pitch ) {
                voices[note].dx7_note->transferPhase(*voices[i].dx7_note);
                break;
            }
        }
    }

 
    voices[note].live = true;
    lastActiveVoice = note;
	//TRACE("activate %d [ %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d ]", pitch, ACT(voices[0]), ACT(voices[1]), ACT(voices[2]), ACT(voices[3]), ACT(voices[4]), ACT(voices[5]), ACT(voices[6]), ACT(voices[7]), ACT(voices[8]), ACT(voices[9]), ACT(voices[10]), ACT(voices[11]), ACT(voices[12]), ACT(voices[13]), ACT(voices[14]), ACT(voices[15]));
}

void DexedAudioProcessor::keyup(uint8_t chan, uint8_t pitch, uint8_t velo) {
    pitch += tuningTranspositionShift();

    int note;
    for (note=0; note<MAX_ACTIVE_NOTES; ++note) {
        if ( ( ( controllers.mpeEnabled && voices[note].channel == chan ) || // MPE node - find voice by channel
               (!controllers.mpeEnabled && voices[note].midi_note == pitch ) ) && // regular mode find voice by pitch
             voices[note].keydown ) // but still only grab the one which is keydown
        {
            voices[note].keydown = false;
			//TRACE("deactivate %d [ %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d ]", pitch, ACT(voices[0]), ACT(voices[1]), ACT(voices[2]), ACT(voices[3]), ACT(voices[4]), ACT(voices[5]), ACT(voices[6]), ACT(voices[7]), ACT(voices[8]), ACT(voices[9]), ACT(voices[10]), ACT(voices[11]), ACT(voices[12]), ACT(voices[13]), ACT(voices[14]), ACT(voices[15]));
            break;
        }
    }
    
    // note not found ?
    if ( note >= MAX_ACTIVE_NOTES ) {
		TRACE("note found ??? %d [ %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d ]", pitch, ACT(voices[0]), ACT(voices[1]), ACT(voices[2]), ACT(voices[3]), ACT(voices[4]), ACT(voices[5]), ACT(voices[6]), ACT(voices[7]), ACT(voices[8]), ACT(voices[9]), ACT(voices[10]), ACT(voices[11]), ACT(voices[12]), ACT(voices[13]), ACT(voices[14]), ACT(voices[15]));
        return;
    }
    
    if ( monoMode ) {
        int highNote = -1;
        int target = 0;
        for (int i=0; i<MAX_ACTIVE_NOTES;i++) {
            if ( voices[i].keydown && voices[i].midi_note > highNote ) {
                target = i;
                highNote = voices[i].midi_note;
            }
        }
        
        if ( highNote != -1 && voices[note].live ) {
            voices[note].live = false;
            voices[target].live = true;
            voices[target].dx7_note->transferState(*voices[note].dx7_note);
        }
    }
    
    if ( sustain ) {
        voices[note].sustained = true;
    } else {
        voices[note].dx7_note->keyup();
    }
}

int DexedAudioProcessor::tuningTranspositionShift()
{
    if( synthTuningState->is_standard_tuning() || ! controllers.transpose12AsScale )
        return data[144] - 24;
    else
    {
        int d144 = data[144];
        if( d144 % 12 == 0 )
        {
            int oct = (d144 - 24) / 12;
            int res = oct * synthTuningState->scale_length();
            return res;
        }
        else
            return data[144] - 24;
    }
}

void DexedAudioProcessor::panic() {
    for(int i=0;i<MAX_ACTIVE_NOTES;i++) {
        voices[i].midi_note = -1;
        voices[i].keydown = false;
        voices[i].live = false;
        if ( voices[i].dx7_note != NULL ) {
            voices[i].dx7_note->oscSync();
        }
    }
}

void DexedAudioProcessor::handleIncomingMidiMessage(const MidiMessage& message) {
    if ( message.isActiveSense() )
        return;

    const uint8 *buf = message.getRawData();
    int sz = message.getRawDataSize();

    //TRACE("%X %X %X %X %X %X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

    if ( ! message.isSysEx() )
        return;

    // test if it is a Yamaha Sysex
    if ( buf[1] != 0x43 ) {
        TRACE("not a yamaha sysex %d", buf[1]);
        return;
    }

    int substatus = buf[2] >> 4;
    switch(substatus) {
        case 0 : {
            // single voice dump
            if ( buf[3] == 0 ) {
                if ( sz < 156 ) {
                    TRACE("wrong single voice datasize %d", sz);
                    return;
                }
                if ( updateProgramFromSysex(buf+6) )
                    TRACE("bad checksum when updating program from sysex message");
            }

            // 32 voice dump
            if ( buf[3] == 9 ) {
                if ( sz < 4104 ) {
                    TRACE("wrong 32 voice dump data size %d", sz);
                    return;
                }

                Cartridge received;
                if ( received.load(buf, sz) == 0 ) {
                    loadCartridge(received);
                    setCurrentProgram(0);
                }
            }
        }
        break;
        case 1 : {
            // parameter change
            if ( sz < 7 ) {
            TRACE("wrong single voice datasize %d", sz);
            return;
            }

            uint8 offset = (buf[3] << 7) + buf[4];
            uint8 value = buf[5];

            TRACE("parameter change message offset:%d value:%d", offset, value);

            if ( offset > 155 ) {
                TRACE("wrong offset size");
                return;
            }

            if ( offset == 155 ) {
                unpackOpSwitch(value);
            } else {
                data[offset] = value;
            }
        }
        break;
        case 2:
            // Voice/cart dump requests need a MIDI output to answer on; the
            // CLI has none.
            return;

        default: {
            TRACE("unknown sysex substatus: %d", substatus);
        }
        return;
    }
}

int DexedAudioProcessor::getEngineType() {
    return engineType;
}

void DexedAudioProcessor::setEngineType(int tp) {
    TRACE("settings engine %d", tp);

    switch (tp)  {
        case DEXED_ENGINE_MARKI:
            controllers.core = &engineMkI;
            break;
        case DEXED_ENGINE_OPL:
            controllers.core = &engineOpl;
            break;
        default:
            controllers.core = &engineMsfa;
            break;
    }
    engineType = tp;
}

void DexedAudioProcessor::setMonoMode(bool mode) {
    panic();
    monoMode = mode;
}

const String DexedAudioProcessor::getInputChannelName (int channelIndex) const {
    return String (channelIndex + 1);
}

const String DexedAudioProcessor::getOutputChannelName (int channelIndex) const {
    return String (channelIndex + 1);
}

bool DexedAudioProcessor::isInputChannelStereoPair (int index) const {
    return true;
}

bool DexedAudioProcessor::isOutputChannelStereoPair (int index) const {
    return true;
}

bool DexedAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    return layouts.getMainOutputChannelSet() == AudioChannelSet::mono()
                || layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

bool DexedAudioProcessor::acceptsMidi() const {
    return true;
}

bool DexedAudioProcessor::producesMidi() const {
    return true;
}

bool DexedAudioProcessor::silenceInProducesSilenceOut() const {
    return false;
}

double DexedAudioProcessor::getTailLengthSeconds() const {
    return 0.0;
}

const String DexedAudioProcessor::getName() const {
    return JucePlugin_Name;
}

//==============================================================================
/**
 * Render one clip: note on at sample 0, note off after spec.soundSamples(),
 * then the tail. `out` is resized to one channel of spec.totalSamples().
 *
 * Everything happens on the calling thread. The voices are reset first, so a
 * single processor instance can render clip after clip without any of the
 * rebuild-the-world cost the old renderClipToFile() paid per call.
 */
void DexedAudioProcessor::renderClip(const RenderSpec& spec, AudioSampleBuffer& out)
{
    const int soundSamples = spec.soundSamples();
    const int totalSamples = spec.totalSamples();
    const int blockSize    = juce::jmax(N, spec.blockSize);

    // A fresh clip must not inherit ringing voices or a settled filter from the
    // previous one, otherwise batch output would depend on job ordering. panic()
    // alone is not enough: it leaves the Dx7Note objects playing, and keydown()
    // treats a still-playing note as a voice steal and skips the oscillator
    // sync, which would change the attack. Reset them to construction state,
    // which is exactly what the plugin got from prepareToPlay() per clip.
    panic();
    for (int note = 0; note < MAX_ACTIVE_NOTES; ++note)
        if (voices[note].dx7_note != nullptr)
            voices[note].dx7_note->reset();

    currentNote    = 0;
    nextKeydownSeq = 0;
    sustain        = false;

    fx.init(spec.sampleRate);
    extra_buf_size = 0;
    lfo.resetState();
    lfo.reset(data + 137);
    fmPermanentlySilent = false;
    silentChunkSamples  = 0;
    skipDeadVoices      = spec.skipDeadVoices;

    out.setSize(1, totalSamples, false, false, true);
    out.clear();

    AudioSampleBuffer renderBuf(1, blockSize);

    bool noteOnSent  = false;
    bool noteOffSent = false;
    int  samplePos   = 0;

    while (samplePos < totalSamples)
    {
        const int blockLen = juce::jmin(blockSize, totalSamples - samplePos);
        renderBuf.setSize(1, blockLen, false, true, false);
        renderBuf.clear();

        MidiBuffer midi;

        if (!noteOnSent)
        {
            midi.addEvent(MidiMessage::noteOn(1, spec.midiNote,
                (uint8_t) juce::roundToInt(spec.velocity * 127.0f)), 0);
            noteOnSent = true;
        }

        if (!noteOffSent && samplePos + blockLen > soundSamples)
        {
            midi.addEvent(MidiMessage::noteOff(1, spec.midiNote),
                juce::jmax(0, soundSamples - samplePos));
            noteOffSent = true;
        }

        processBlock(renderBuf, midi);

        FloatVectorOperations::copy(out.getWritePointer(0, samplePos),
                                    renderBuf.getReadPointer(0),
                                    blockLen);

        samplePos += blockLen;
    }
}

void dexed_trace(const char *source, const char *fmt, ...) {
    char output[4096];
    va_list argptr;
    va_start(argptr, fmt);
    vsnprintf(output, 4095, fmt, argptr);
    va_end(argptr);

    String dest;
    dest << source << " " << output;
    Logger::writeToLog(dest);
}

void DexedAudioProcessor::resetTuning(std::shared_ptr<TuningState> t)
{
    synthTuningState = t;
    synthTuningStateLast = t;
    for( int i=0; i<MAX_ACTIVE_NOTES; ++i )
        if( voices[i].dx7_note != nullptr )
            voices[i].dx7_note->tuning_state_ = synthTuningState;
}

void DexedAudioProcessor::retuneToStandard()
{
    currentSCLData = "";
    currentKBMData = "";
    resetTuning(createStandardTuning());
}

void DexedAudioProcessor::applySCLTuning(File s) {
    std::string sclcontents = s.loadFileAsString().toStdString();
    applySCLTuning(sclcontents);
}

void DexedAudioProcessor::applySCLTuning(std::string sclcontents) {
    if( currentKBMData.size() < 1 )
    {
        auto t = createTuningFromSCLData( sclcontents );
        if (t != nullptr) {            
            resetTuning(t); // update tuning            
            currentSCLData = sclcontents; // remember this SCL data            
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        }
        else {            
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
        }
    }
    else
    {
        auto t = createTuningFromSCLAndKBMData( sclcontents, currentKBMData );
        if (t != nullptr) {            
            resetTuning(t); // update tuning            
            currentSCLData = sclcontents; // remember this SCL data            
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        }
        else {            
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
        }
    }
}

void DexedAudioProcessor::applyKBMMapping( File s )
{
    std::string kbmcontents = s.loadFileAsString().toStdString();
    applyKBMMapping(kbmcontents);
}

void DexedAudioProcessor::applyKBMMapping(std::string kbmcontents) {  
    if( currentSCLData.size() < 1 ) {
        auto t = createTuningFromKBMData(kbmcontents);
        if (t != nullptr) {            
            resetTuning(t); // update tuning
            currentKBMData = kbmcontents; // remember this KBM data
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        } else {            
            resetTuning(synthTuningStateLast); // revert to the "last good working state"
        }
    } else {
        auto t = createTuningFromSCLAndKBMData( currentSCLData, kbmcontents );
        if (t != nullptr) {            
            resetTuning(t); // update tuning            
            currentKBMData = kbmcontents; // remember this KBM data            
            synthTuningStateLast = t; // remember this whole state as a "last good working state"
        } else {            
            resetTuning(synthTuningStateLast); // revert to "last good working state"
        }
    }
}
