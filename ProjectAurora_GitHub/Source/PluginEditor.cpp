#include "PluginEditor.h"

ProjectAuroraAudioProcessorEditor::AuroraLookAndFeel::AuroraLookAndFeel()
    : juce::LookAndFeel_V4(juce::LookAndFeel_V4::ColourScheme(
        juce::Colour(0xff11131a), juce::Colour(0xff202431), juce::Colour(0xff343a4a),
        juce::Colour(0xff8b7cff), juce::Colour(0xfff4f5fb), juce::Colour(0xffb7bdcf),
        juce::Colour(0xff8b7cff), juce::Colour(0xff8b7cff), juce::Colour(0xfff4f5fb)))
{
    setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff11131a));
    setColour(juce::Label::textColourId, juce::Colour(0xfff4f5fb));
    setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xfff4f5fb));
    setColour(juce::ComboBox::textColourId, juce::Colour(0xfff4f5fb));
}

ProjectAuroraAudioProcessorEditor::ProjectAuroraAudioProcessorEditor(ProjectAuroraAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p), controls(p)
{
    setLookAndFeel(&darkLook);
    title.setText("PROJECT AURORA", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(25.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xfff4f5fb));
    subtitle.setText("REAL-TIME PITCH CORRECTION · EXPERIMENTAL", juce::dontSendNotification);
    subtitle.setFont(juce::Font(juce::FontOptions(11.0f)));
    subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff9da5ba));
    detectedLabel.setJustificationType(juce::Justification::centredLeft);
    targetLabel.setJustificationType(juce::Justification::centredLeft);
    centsLabel.setJustificationType(juce::Justification::centredRight);
    for (auto* l : { &detectedLabel, &targetLabel, &centsLabel })
    {
        l->setColour(juce::Label::textColourId, juce::Colour(0xfff4f5fb));
        l->setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        addAndMakeVisible(*l);
    }
    addAndMakeVisible(title);
    addAndMakeVisible(subtitle);
    addAndMakeVisible(controls);
    controls.setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff171a23));
    setSize(600, 720);
    startTimerHz(12);
}

ProjectAuroraAudioProcessorEditor::~ProjectAuroraAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}

void ProjectAuroraAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff11131a));
    g.setColour(juce::Colour(0xff8b7cff));
    g.fillRoundedRectangle(18.0f, 18.0f, 5.0f, 48.0f, 2.5f);
    g.setColour(juce::Colour(0xff272c3a));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 12.0f, 1.0f);
   g.drawRoundedRectangle(
    18.0f,
    83.0f,
    static_cast<float>(getWidth() - 36),
    72.0f,
    9.0f,
    1.5f
);
}

void ProjectAuroraAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(22);
    title.setBounds(area.removeFromTop(34));
    subtitle.setBounds(area.removeFromTop(22));
    area.removeFromTop(14);
    auto meter = area.removeFromTop(72).reduced(10, 6);
    detectedLabel.setBounds(meter.removeFromLeft(meter.getWidth() / 3));
    targetLabel.setBounds(meter.removeFromLeft(meter.getWidth() / 3));
    centsLabel.setBounds(meter);
    area.removeFromTop(14);
    controls.setBounds(area);
}

void ProjectAuroraAudioProcessorEditor::timerCallback()
{
    detectedLabel.setText("INPUT  " + processor.getDetectedNote(), juce::dontSendNotification);
    targetLabel.setText("TARGET  " + processor.getTargetNote(), juce::dontSendNotification);
    centsLabel.setText(juce::String(processor.getCorrectionCents(), 1) + " cents", juce::dontSendNotification);
}
