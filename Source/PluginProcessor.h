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

#ifndef PLUGINPROCESSOR_H_INCLUDED
#define PLUGINPROCESSOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"

#include "msfa/controllers.h"
#include "msfa/dx7note.h"
#include "msfa/lfo.h"
#include "msfa/synth.h"
#include "msfa/fm_core.h"
#include "msfa/tuning.h"
#include "PluginParam.h"
#include "PluginData.h"
#include "PluginFx.h"
#include "EngineMkI.h"
#include "EngineOpl.h"
#include "RenderSpec.h"

struct ProcessorVoice {
    int channel;
    int midi_note;
    int velocity;
    bool keydown;
    bool sustained;
    bool live;
    int32_t keydown_seq;

    int mpePitchBend;
    Dx7Note *dx7_note;
};

enum DexedEngineResolution {
    DEXED_ENGINE_MODERN,
    DEXED_ENGINE_MARKI,
    DEXED_ENGINE_OPL
};

/// Maximum allowed size for SCL and KBM files.
/// (COMMENT: Since none of the 5175 .scl files in the Scala archive
/// at https://www.huygens-fokker.org/scala/downloads.html#scales
/// exceed 6 KB (in 25th Mar 2024), a maximum size of 16 KB appears
/// to be a practical choice.)
const int MAX_SCL_KBM_FILE_SIZE = 16384;

//==============================================================================
/**
*/
class DexedAudioProcessor  : public AudioProcessor
{
    static const int MAX_ACTIVE_NOTES = 16;
    ProcessorVoice voices[MAX_ACTIVE_NOTES];
    int currentNote;

    // The original DX7 had one single LFO. Later units had an LFO per note.
    Lfo lfo;

    bool sustain;
    bool monoMode;
    
    // Extra buffering for when GetSamples wants a buffer not a multiple of N
    float extra_buf[N];
    int extra_buf_size;

    int currentProgram;
    
    /**
     * The last time the state was save, to be able to bypass a VST host bug.
     */
    long lastStateSave;
    
    /**
     * Plugin fx (the filter)
     */
    PluginFx fx;

    /**
     * This flag is used in the audio thread to know if the voice has changed
     * and needs to be updated.
     */
    bool refreshVoice;
    bool normalizeDxVelocity;

    void processMidiMessage(const MidiMessage *msg);
    void keydown(uint8_t chan, uint8_t pitch, uint8_t velo);
    void keyup(uint8_t, uint8_t pitch, uint8_t velo);

    void initCtrl();

	MidiMessage* nextMidi,*midiMsg;
	bool hasMidiMessage;
    int midiEventPos;
	bool getNextEvent(MidiBuffer::Iterator* iter,const int samplePos);

    void handleIncomingMidiMessage(const MidiMessage& message);
    uint32_t engineType;

    // Offline "the rest of this clip is silence" detection. See trackFmSilence().
    bool     skipDeadVoices     = false;
    bool     fmPermanentlySilent = false;
    uint64_t silentChunkSamples = 0;
    void trackFmSilence(const float* sumbuf);
    
    FmCore engineMsfa;
    EngineMkI engineMkI;
    EngineOpl engineOpl;

    void resolvAppDir();
    
    void unpackOpSwitch(char packOpValue);
    void packOpSwitch();

public :
    // in MIDI units (0x4000 is neutral)
    Controllers controllers;
    StringArray programNames;
    Cartridge currentCart;
    uint8_t data[161];

    VoiceStatus voiceStatus;
    File activeFileCartridge;

    int getEngineType();
    void setEngineType(int rs);
    
    HashMap<int, Ctrl*> mappedMidiCC;
    
    Array<Ctrl*> ctrl;

    OperatorCtrl opCtrl[6];
    std::unique_ptr<CtrlDX> pitchEgRate[4];
    std::unique_ptr<CtrlDX> pitchEgLevel[4];
    std::unique_ptr<CtrlDX> pitchModSens;
    std::unique_ptr<CtrlDX> algo;
    std::unique_ptr<CtrlDX> oscSync;
    std::unique_ptr<CtrlDX> feedback;
    std::unique_ptr<CtrlDX> lfoRate;
    std::unique_ptr<CtrlDX> lfoDelay;
    std::unique_ptr<CtrlDX> lfoAmpDepth;
    std::unique_ptr<CtrlDX> lfoPitchDepth;
    std::unique_ptr<CtrlDX> lfoWaveform;
    std::unique_ptr<CtrlDX> lfoSync;
    std::unique_ptr<CtrlDX> transpose;

    std::unique_ptr<CtrlFloat> fxCutoff;
    std::unique_ptr<CtrlFloat> fxReso;
    std::unique_ptr<CtrlFloat> output;
    std::unique_ptr<Ctrl> tune;
    std::unique_ptr<Ctrl> monoModeCtrl;

    void loadCartridge(Cartridge &cart);
    void setDxValue(int offset, int v);

    //==============================================================================
    DexedAudioProcessor();
    ~DexedAudioProcessor();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (AudioSampleBuffer& buffer, MidiBuffer& midiMessages) override;
    void panic();
    bool isMonoMode() {
        return monoMode;
    }
    void setMonoMode(bool mode);

    //==============================================================================
    AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int updateProgramFromSysex(const uint8 *rawdata);
    void setupStartupCart();
    
    //==============================================================================
    const String getName() const override;
    int getNumParameters() override;
    float getParameter (int index) override;
    void setParameter (int index, float newValue) override;
    const String getParameterName (int index) override;
    const String getParameterText (int index) override;
    String getParameterID (int index) override;

    const String getInputChannelName (int channelIndex) const override;
    const String getOutputChannelName (int channelIndex) const override;
    bool isInputChannelStereoPair (int index) const override;
    bool isOutputChannelStereoPair (int index) const override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool silenceInProducesSilenceOut() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const String getProgramName (int index) override;
    void changeProgramName(int index, const String& newName) override;
    void resetToInitVoice() ;
    
    //==============================================================================
    void getStateInformation (MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    static File dexedAppDir;
    static File dexedCartDir;

    int lastActiveVoice = 0;

    MTSClient *mtsClient;
    std::shared_ptr<TuningState> synthTuningState;

    // holds the previous working tuning state;
    // used to restore tuning state when there was a problem 
    // with loading/applying a new .SCL and/or .KBM file 
    std::shared_ptr<TuningState> synthTuningStateLast;

    // Load a file
    void applySCLTuning(File sclf);
    void applyKBMMapping(File kbmf);

    // Load from text
    void applySCLTuning(std::string scld);
    void applyKBMMapping(std::string kbmd);
    
    void retuneToStandard();
    void resetTuning(std::shared_ptr<TuningState> t);
    int tuningTranspositionShift();
    
    std::string currentSCLData = "";
    std::string currentKBMData = "";

    //==============================================================================
    // Offline rendering. See RenderSpec in RenderJob.h for the clip geometry.
    //
    // renderClip() owns the whole note-on/note-off/tail sequence and writes
    // `spec.totalSamples()` mono samples into `out`. It runs entirely on the
    // calling thread: no audio device, no message thread, no locks. One
    // DexedAudioProcessor belongs to exactly one worker thread.
    void renderClip(const RenderSpec& spec, AudioSampleBuffer& out);

    // Sample-rate dependent lookup tables in msfa are process-global statics.
    // They must be initialised once, before any worker thread starts, and then
    // stay read-only for the rest of the run.
    static void initSharedTables(double sampleRate);

    // Per-instance setup at a fixed sample rate. Replaces prepareToPlay() for
    // the CLI: allocates the voices exactly once and never touches the shared
    // tables, so it is safe to call from a worker thread.
    void prepareForOfflineRender(double sampleRate, int blockSize);

private:
    int chooseNote(uint8_t pitch);
    int32_t nextKeydownSeq;;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DexedAudioProcessor)

};

#endif  // PLUGINPROCESSOR_H_INCLUDED
