/**
 *
 * Copyright (c) 2013 Pascal Gauthier.
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

#ifndef PLUGINPARAM_H_INCLUDED
#define PLUGINPARAM_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"

class DexedAudioProcessor;

/**
 * One addressable synth parameter.
 *
 * In the plugin this also acted as the glue to a Slider/Button/ComboBox; the
 * CLI has no components, so all that remains is the value mapping between the
 * host's normalised 0..1 range and the DX7 representation.
 */
class Ctrl {
public:
    String label;

    Ctrl(String name);
    virtual ~Ctrl() = default;

    // use this to signal a parameter change to the host
    void publishValue(float value);

    /**
     * Host value is related 0.0 to 1.0 values
     */
    virtual void setValueHost(float f) = 0;
    virtual float getValueHost() = 0;
    virtual String getValueDisplay() = 0;

    /**
     * Index of this parameter
     */
    int idx;
    DexedAudioProcessor *parent;
};

class CtrlFloat : public Ctrl {
	float *vPointer;
public:

	CtrlFloat(String name, float *storageValue);
	void setValueHost(float f);
	float getValueHost();
	String getValueDisplay();
};

// CtrlDX is a controller that is related to DX parameters
class CtrlDX : public Ctrl {
    int dxValue;
    int steps;
    int dxOffset;
    int displayValue;

public:
    CtrlDX(String name, int steps, int offset = -1, int displayValue = 0);
    void setValueHost(float f);
    float getValueHost();
    void publishValue(float value);

    void setValue(int value);
    int getValue();
    int getOffset();
    String getValueDisplay();
};

struct OperatorCtrl {
    std::unique_ptr<CtrlDX> egRate[4];
    std::unique_ptr<CtrlDX> egLevel[4];
    std::unique_ptr<CtrlDX> level;
    std::unique_ptr<CtrlDX> opMode;
    std::unique_ptr<CtrlDX> coarse;
    std::unique_ptr<CtrlDX> fine;
    std::unique_ptr<CtrlDX> detune;
    std::unique_ptr<CtrlDX> sclBrkPt;
    std::unique_ptr<CtrlDX> sclLeftDepth;
    std::unique_ptr<CtrlDX> sclRightDepth;
    std::unique_ptr<CtrlDX> sclLeftCurve;
    std::unique_ptr<CtrlDX> sclRightCurve;
    std::unique_ptr<CtrlDX> sclRate;
    std::unique_ptr<CtrlDX> ampModSens;
    std::unique_ptr<CtrlDX> velModSens;
    std::unique_ptr<Ctrl> opSwitch;
};

#endif  // PLUGINPARAM_H_INCLUDED
