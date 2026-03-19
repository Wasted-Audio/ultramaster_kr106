#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "PluginProcessor.h"

// Small text display showing the current preset name.
// Drag up/down to scroll through presets.
class KR106PresetDisplay : public juce::Component
{
public:
    KR106PresetDisplay(KR106AudioProcessor* processor)
        : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        mTypeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::Segment14_otf,
            BinaryData::Segment14_otfSize);
    }

    void paint(juce::Graphics& g) override
    {
        int w = getWidth(), h = getHeight();

        // Black background, no border
        g.setColour(juce::Colour(0, 0, 0));
        g.fillRect(0, 0, w, h);

        if (!mProcessor) return;

        // Get current preset name
        int idx = mProcessor->getCurrentProgram();
        juce::String name = mProcessor->getProgramName(idx).substring(0, 18);

        // Draw preset name left-aligned in green (Segment14 font)
        // Use drawSingleLineText at a fixed baseline to bypass JUCE font metrics
        g.setColour(juce::Colour(0, 220, 0));
        g.setFont(juce::Font(juce::FontOptions(mTypeface).withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(8.f));
        g.drawSingleLineText(name, 3, h - 3);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        mDragAccum = 0.f;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mProcessor) return;

        float dy = static_cast<float>(e.getDistanceFromDragStartY()) - mDragAccum;
        mDragAccum = static_cast<float>(e.getDistanceFromDragStartY());

        mDragRemainder += dy;
        static constexpr float kThreshold = 8.f;

        while (mDragRemainder <= -kThreshold)
        {
            mDragRemainder += kThreshold;
            changePreset(-1);
        }
        while (mDragRemainder >= kThreshold)
        {
            mDragRemainder -= kThreshold;
            changePreset(1);
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        mDragRemainder = 0.f;
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (!mProcessor) return;
        if (wheel.deltaY > 0.f)
            changePreset(1);
        else if (wheel.deltaY < 0.f)
            changePreset(-1);
    }

private:
    void changePreset(int delta)
    {
        int num = mProcessor->getNumPrograms();
        if (num <= 0) return;
        int idx = mProcessor->getCurrentProgram() + delta;
        idx = ((idx % num) + num) % num; // wrap around
        mProcessor->setCurrentProgram(idx);
        repaint();
    }

    KR106AudioProcessor* mProcessor = nullptr;
    juce::Typeface::Ptr mTypeface;
    float mDragAccum = 0.f;
    float mDragRemainder = 0.f;
};
