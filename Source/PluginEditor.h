/**
 *
 * Copyright (c) 2013-2018 Pascal Gauthier.
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
 */

#ifndef PLUGINEDITOR_H_INCLUDED
#define PLUGINEDITOR_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"
#include "PluginProcessor.h"
#include "OperatorEditor.h"
#include "GlobalEditor.h"
#include "DXComponents.h"
#include "DXLookNFeel.h"
#include "CartManager.h"
#include "JsonServer.h"

// Status bar component: JSON server info, rendering indicator, zoom controls
class ServerStatusBar : public juce::Component
{
public:
    void setServer(JsonServer* s) { server = s; }
    void setProcessor(DexedAudioProcessor* p) { proc = p; }

    void setZoomCallbacks(std::function<float()> getZoom, std::function<void(float)> setZoom)
    {
        getZoomFn = std::move(getZoom);
        setZoomFn = std::move(setZoom);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xFF151D20));
        g.setColour(juce::Colour(0xFF2A3A40));
        g.drawLine(0.0f, 0.0f, (float)getWidth(), 0.0f, 1.0f);

        if (server == nullptr)
            return;

        const bool listening  = server->getIsListening();
        const int  clients    = server->getClients();
        juce::String lastLine = server->getLastJson();

        // --- right-side controls (right to left) ---
        const int rPad = 8, btnW = 26, labelW = 52, gap = 4;

        int plusX   = getWidth() - rPad - btnW;
        int labelX  = plusX  - gap - labelW;
        int minusX  = labelX - gap - btnW;
        int zoomSep = minusX - 10;

        zoomPlusBtn  = juce::Rectangle<int>(plusX,  2, btnW,   getHeight() - 4);
        zoomLabelBox = juce::Rectangle<int>(labelX, 2, labelW, getHeight() - 4);
        zoomMinusBtn = juce::Rectangle<int>(minusX, 2, btnW,   getHeight() - 4);

        auto drawBtn = [&](juce::Rectangle<int> r, const juce::String& label,
                           juce::Colour bg, juce::Colour fg)
        {
            g.setColour(bg);
            g.fillRoundedRectangle(r.toFloat(), 3.0f);
            g.setColour(fg);
            g.setFont(juce::Font(13.0f, juce::Font::bold));
            g.drawText(label, r, juce::Justification::centred, false);
        };

        // separator
        g.setColour(juce::Colour(0xFF2A3A40));
        g.drawLine((float)zoomSep, 4.0f, (float)zoomSep, (float)(getHeight() - 4), 1.0f);

        // zoom buttons
        drawBtn(zoomMinusBtn, "-",  juce::Colour(0xFF2E4248), juce::Colour(0xFFCCDDDD));
        drawBtn(zoomPlusBtn,  "+",  juce::Colour(0xFF2E4248), juce::Colour(0xFFCCDDDD));

        float zoom = getZoomFn ? getZoomFn() : 1.0f;
        g.setColour(juce::Colour(0xFFBBCCCC));
        g.setFont(juce::Font(13.0f));
        g.drawText(juce::String(juce::roundToInt(zoom * 100)) + "%",
                   zoomLabelBox, juce::Justification::centred, false);

        // --- left: status dot + text ---
        float dotSize = 13.0f;
        float dotY    = (getHeight() - dotSize) * 0.5f;

        const bool rendering = proc && proc->isRenderingClip.load();
        g.setColour(rendering ? juce::Colour(0xFFFFCC00)
                              : (listening ? juce::Colour(0xFF44DD44) : juce::Colour(0xFFDD4444)));
        g.fillEllipse(8.0f, dotY, dotSize, dotSize);

        // Show actual bound port; if all ports were taken, report unavailable.
        juce::String text = rendering
            ? "Rendering..."
            : (listening
                ? (juce::String("JSON Server  addr:127.0.0.1")
                   + "  port:" + juce::String(server->getPort())
                   + "  clients:" + juce::String(clients))
                : ("JSON Server  UNAVAILABLE  (ports "
                   + juce::String(JsonServer::DEFAULT_PORT) + "-"
                   + juce::String(JsonServer::DEFAULT_PORT + JsonServer::MAX_PORT_TRIES - 1)
                   + " in use)"));

        if (!rendering && lastLine.isNotEmpty())
        {
            juce::String t = lastLine.length() > 60
                ? lastLine.substring(0, 60) + "..."
                : lastLine;
            text += "  |  " + t;
        }

        g.setColour(juce::Colour(0xFFAABBBB));
        g.setFont(juce::Font(13.0f));
        g.drawText(text, 28, 0, zoomSep - 34, getHeight(), juce::Justification::centredLeft, true);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (zoomPlusBtn.contains(e.x, e.y) && setZoomFn && getZoomFn)
            setZoomFn(juce::jlimit(0.5f, 4.0f, getZoomFn() + 0.25f));
        else if (zoomMinusBtn.contains(e.x, e.y) && setZoomFn && getZoomFn)
            setZoomFn(juce::jlimit(0.5f, 4.0f, getZoomFn() - 0.25f));

        repaint();
    }

private:
    JsonServer*         server   = nullptr;
    DexedAudioProcessor*     proc     = nullptr;
    std::function<float()>       getZoomFn;
    std::function<void(float)>   setZoomFn;
    juce::Rectangle<int> zoomMinusBtn, zoomPlusBtn, zoomLabelBox;
};

//==============================================================================
/**
*/
class DexedAudioProcessorEditor  : public AudioProcessorEditor, public ComboBox::Listener, public Timer,
                                   public FileDragAndDropTarget, public KeyListener {
    MidiKeyboardComponent midiKeyboard;
    OperatorEditor operators[6];
    Colour background;
    CartManager cartManager;
    // This cover is used to disable main window when cart manager is shown
    Component cartManagerCover;

    SharedResourcePointer<DXLookNFeel> lookAndFeel;
    std::unique_ptr<juce::DialogWindow> dexedParameterDialog;
    #ifdef DEXED_EVENT_DEBUG
        FocusLogger focusLogger;
    #endif

    void resetSize();

    Component frameComponent;
    ServerStatusBar serverStatusBar;
public:
    DexedAudioProcessor *processor;
    GlobalEditor global;
    
    DexedAudioProcessorEditor (DexedAudioProcessor* ownerFilter);
    ~DexedAudioProcessorEditor();
    virtual void timerCallback() override;

    virtual void paint (Graphics& g) override;
    virtual void comboBoxChanged (ComboBox* comboBoxThatHasChanged) override;
    void updateUI();
    void rebuildProgramCombobox();
    void loadCart(File file);
    void saveCart();
    void initProgram();
    void storeProgram();
    void cartShow();
    void parmShow();
    void tuningShow();
    void discoverMidiCC(Ctrl *ctrl);

    static float getLargestScaleFactor();
    void resetZoomFactor();

    virtual bool isInterestedInFileDrag (const StringArray &files) override;
    virtual void filesDropped (const StringArray &files, int x, int y ) override;
    std::unique_ptr<ComponentTraverser> createFocusTraverser() override;

    bool keyPressed(const KeyPress& key, Component* originatingComponent) override;

    static const int WINDOW_SIZE_X = 866;
    static const int WINDOW_SIZE_Y = 702;
    static const int STATUS_BAR_H = 28;
};


#endif  // PLUGINEDITOR_H_INCLUDED
