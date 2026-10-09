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
    class PsolaPitchShifter
    {
    public:
        void prepare(double sampleRate, int lookAhead, int latency);
        void reset();
        float process(float input, float mono, float pitchHz, float ratio, float shiftSemitones, bool voiced);

    private:
        int wrapIndex(int index, int size) const;
        float readHistory(const std::vector<float>& history, int delay) const;
        void addGrain(int markOffset, float periodSamples);

        double currentSampleRate = 44100.0;
        int lookAheadSamples = 0;
        int latencySamples = 0;
        int historySize = 0;
        int outputSize = 0;
        int historyWriteIndex = 0;
        int outputReadIndex = 0;
        float synthesisPhase = 0.0f;
        float wetMix = 0.0f;
        std::vector<float> audioHistory;
        std::vector<float> monoHistory;
        std::vector<float> outputSum;
        std::vector<float> outputWeight;
        std::vector<float> wetMixBuffer;
    };

    void analysePitch(float sample);
    int quantizeMidiNote(float midiNote, int root, int scale) const;
    static float detectMidiNote(const float* samples, int count, double sampleRate);
    static juce::String noteName(int midiNote);

    static constexpr int analysisBufferSize = 1536;
    static constexpr int analysisHopSize = 256;

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
    bool currentVoiced = false;
    std::array<PsolaPitchShifter, 2> shifters;
    std::atomic<float> correctionCents { 0.0f };
    std::atomic<int> detectedMidiAtomic { -1 };
    std::atomic<int> targetMidiAtomic { -1 };
    juce::LinearSmoothedValue<float> inputGain, outputGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectAuroraAudioProcessor)
};
