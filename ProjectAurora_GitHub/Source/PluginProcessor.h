#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

class ProjectAuroraAudioProcessor final : public juce::AudioProcessor
{
public:
    ProjectAuroraAudioProcessor();
    ~ProjectAuroraAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::String getDetectedNote() const;
    juce::String getTargetNote() const;
    float getCorrectionCents() const { return correctionCents.load(); }

private:
    class SimplePitchShifter
    {
    public:
        void prepare(int delayLength);
        void reset();
        float process(float input, float ratio);

    private:
        float readDelay(float delay) const;
        std::vector<float> buffer;
        int writeIndex = 0;
        float phase = 0.0f;
        int delaySize = 1024;
    };

    void analysePitch(float sample);
    int quantizeMidiNote(float midiNote, int root, int scale) const;
    static float detectMidiNote(const float* samples, int count, double sampleRate);
    static juce::String noteName(int midiNote);

    static constexpr int analysisBufferSize = 1024;
    static constexpr int analysisHopSize = 256;
    static constexpr int pitchShifterDelaySize = 1024;

    double currentSampleRate = 44100.0;
    int analysisDecimationFactor = 2;
    int analysisDecimationCount = 0;
    float analysisAccumulator = 0.0f;
    std::array<float, analysisBufferSize> analysisBuffer {};
    int analysisWrite = 0;
    int samplesSinceAnalysis = 0;
    int analysisSamplesWritten = 0;
    float detectedMidi = -1.0f;
    int targetMidi = -1;
    float targetShiftSemitones = 0.0f;
    float currentShiftSemitones = 0.0f;
    int samplesSinceValidDetection = 0;
    bool pitchActive = false;
    std::array<SimplePitchShifter, 2> shifters;
    std::atomic<float> correctionCents { 0.0f };
    std::atomic<int> detectedMidiAtomic { -1 };
    std::atomic<int> targetMidiAtomic { -1 };
    juce::LinearSmoothedValue<float> inputGain, outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectAuroraAudioProcessor)
};
