#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class ProjectAuroraAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ProjectAuroraAudioProcessorEditor(ProjectAuroraAudioProcessor&);
    ~ProjectAuroraAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    class AuroraLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        AuroraLookAndFeel();
    };
    void timerCallback() override;
    ProjectAuroraAudioProcessor& processor;
    AuroraLookAndFeel darkLook;
    juce::GenericAudioProcessorEditor controls;
    juce::Label title, subtitle, detectedLabel, targetLabel, centsLabel;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectAuroraAudioProcessorEditor)
};
