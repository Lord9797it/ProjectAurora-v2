#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int analysisWindowSize = 1024;
constexpr int analysisHopSize = 256;
constexpr int pitchShifterDelaySize = 1024;
constexpr float minimumPitchHz = 65.0f;
constexpr float maximumPitchHz = 1000.0f;
constexpr float minimumRms = 0.004f;

const std::array<const char*, 12> noteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const std::array<std::array<int, 12>, 7> scales {{
    {{0,1,2,3,4,5,6,7,8,9,10,11}},
    {{0,2,4,5,7,9,11,-1,-1,-1,-1,-1}},
    {{0,2,3,5,7,8,10,-1,-1,-1,-1,-1}},
    {{0,2,3,5,7,8,11,-1,-1,-1,-1,-1}},
    {{0,2,4,5,7,9,10,-1,-1,-1,-1,-1}},
    {{0,2,4,7,9,-1,-1,-1,-1,-1,-1,-1}},
    {{0,3,5,7,10,-1,-1,-1,-1,-1,-1,-1}}
}};
}

ProjectAuroraAudioProcessor::ProjectAuroraAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout ProjectAuroraAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterChoice>("KEY", "Key", juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    p.push_back(std::make_unique<juce::AudioParameterChoice>("SCALE", "Scale", juce::StringArray { "Chromatic", "Major", "Minor", "Harmonic Minor", "Mixolydian", "Major Pentatonic", "Minor Pentatonic" }, 1));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("SPEED", "Correction Speed", juce::NormalisableRange<float>(1.0f, 100.0f, 0.1f), 35.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("AMOUNT", "Correction Amount", juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("FINE", "Fine Tune", juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("INGAIN", "Input Gain", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("OUTGAIN", "Output Gain", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterBool>("BYPASS", "Bypass", false));
    return { p.begin(), p.end() };
}

void ProjectAuroraAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    analysisDecimationFactor = juce::jmax(1, static_cast<int>(std::lround(sampleRate / 24000.0)));
    analysisDecimationCount = 0;
    analysisAccumulator = 0.0f;
    analysisBuffer.fill(0.0f);
    analysisWrite = samplesSinceAnalysis = analysisSamplesWritten = 0;
    detectedMidi = -1.0f;
    targetMidi = -1;
    targetShiftSemitones = 0.0f;
    samplesSinceValidDetection = static_cast<int>(sampleRate * 2.0);
    pitchActive = false;

    for (auto& shifter : shifters)
    {
        shifter.prepare(pitchShifterDelaySize);
        shifter.reset();
    }

    inputGain.reset(sampleRate, 0.02);
    outputGain.reset(sampleRate, 0.02);
    inputGain.setCurrentAndTargetValue(1.0f);
    outputGain.setCurrentAndTargetValue(1.0f);
    detectedMidiAtomic.store(-1);
    targetMidiAtomic.store(-1);
    correctionCents.store(0.0f);

    // The dual-head shifter is centred around this fixed delay; the dry path uses the same delay.
    setLatencySamples(pitchShifterDelaySize / 2);
    juce::ignoreUnused(samplesPerBlock);
}

void ProjectAuroraAudioProcessor::releaseResources() {}

bool ProjectAuroraAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void ProjectAuroraAudioProcessor::SimplePitchShifter::prepare(int delayLength)
{
    delaySize = juce::jmax(256, delayLength);
    buffer.assign(static_cast<size_t>(delaySize), 0.0f);
    reset();
}

void ProjectAuroraAudioProcessor::SimplePitchShifter::reset()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
    phase = 0.0f;
}

float ProjectAuroraAudioProcessor::SimplePitchShifter::readDelay(float delay) const
{
    if (buffer.empty())
        return 0.0f;

    delay = juce::jlimit(2.0f, static_cast<float>(delaySize - 3), delay);
    float position = static_cast<float>(writeIndex) - delay;
    while (position < 0.0f)
        position += static_cast<float>(delaySize);

    const int i1 = static_cast<int>(std::floor(position)) % delaySize;
    const int i0 = (i1 + delaySize - 1) % delaySize;
    const int i2 = (i1 + 1) % delaySize;
    const int i3 = (i1 + 2) % delaySize;
    const float fraction = position - std::floor(position);
    const float y0 = buffer[static_cast<size_t>(i0)];
    const float y1 = buffer[static_cast<size_t>(i1)];
    const float y2 = buffer[static_cast<size_t>(i2)];
    const float y3 = buffer[static_cast<size_t>(i3)];

    // Cubic interpolation reduces the high-frequency loss of a linear read head.
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * fraction + c2) * fraction + c1) * fraction + y1;
}

float ProjectAuroraAudioProcessor::SimplePitchShifter::process(float input, float ratio)
{
    if (buffer.empty())
        return input;

    buffer[static_cast<size_t>(writeIndex)] = input;
    ratio = juce::jlimit(0.5f, 2.0f, ratio);
    float output = readDelay(static_cast<float>(delaySize) * 0.5f);

    if (std::abs(1.0f - ratio) >= 0.0005f)
    {
        phase += (1.0f - ratio) / static_cast<float>(delaySize - 64);
        if (phase >= 1.0f)
            phase -= 1.0f;
        else if (phase < 0.0f)
            phase += 1.0f;

        const float phaseB = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
        const float delayA = 32.0f + phase * static_cast<float>(delaySize - 64);
        const float delayB = 32.0f + phaseB * static_cast<float>(delaySize - 64);
        const float a = readDelay(delayA);
        const float b = readDelay(delayB);

        // Each head reaches zero gain exactly where its delay wraps to the start.
        const float weightA = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * phase);
        const float weightB = 1.0f - weightA;
        output = a * weightA + b * weightB;
    }

    writeIndex = (writeIndex + 1) % delaySize;
    return output;
}

float ProjectAuroraAudioProcessor::detectMidiNote(const float* samples, int count, double sampleRate)
{
    if (samples == nullptr || count < 512 || sampleRate <= 0.0)
        return -1.0f;

    double mean = 0.0;
    for (int i = 0; i < count; ++i)
        mean += samples[i];
    mean /= static_cast<double>(count);

    double squareSum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double centred = static_cast<double>(samples[i]) - mean;
        squareSum += centred * centred;
    }
    const float rms = static_cast<float>(std::sqrt(squareSum / static_cast<double>(count)));
    if (rms < minimumRms)
        return -1.0f;

    const int minLag = juce::jmax(2, static_cast<int>(sampleRate / maximumPitchHz));
    const int maxLag = juce::jmin(count / 2 - 1, static_cast<int>(sampleRate / minimumPitchHz));
    if (maxLag <= minLag)
        return -1.0f;

    std::array<float, analysisWindowSize> difference {};
    std::array<float, analysisWindowSize> normalizedDifference {};
    const int comparisonLength = count - maxLag;
    double runningDifference = 0.0;

    // YIN-style normalized difference avoids choosing the strongest harmonic as often as raw autocorrelation.
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double differenceSum = 0.0;
        for (int i = 0; i < comparisonLength; i += 2)
        {
            const double a = static_cast<double>(samples[i]) - mean;
            const double b = static_cast<double>(samples[i + lag]) - mean;
            const double delta = a - b;
            differenceSum += delta * delta;
        }

        const float differenceValue = static_cast<float>(differenceSum);
        difference[static_cast<size_t>(lag)] = differenceValue;
        runningDifference += differenceValue;
        normalizedDifference[static_cast<size_t>(lag)] =
            runningDifference > 1.0e-12 ? differenceValue * static_cast<float>(lag) / static_cast<float>(runningDifference) : 1.0f;
    }

    int bestLag = -1;
    float bestScore = 1.0f;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const float score = normalizedDifference[static_cast<size_t>(lag)];
        if (score < bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }

        if (score < 0.16f)
        {
            int trough = lag;
            while (trough < maxLag
                   && normalizedDifference[static_cast<size_t>(trough + 1)]
                          < normalizedDifference[static_cast<size_t>(trough)])
                ++trough;
            bestLag = trough;
            bestScore = normalizedDifference[static_cast<size_t>(trough)];
            break;
        }
    }

    if (bestLag < 0 || bestScore > 0.30f)
        return -1.0f;

    float refinedLag = static_cast<float>(bestLag);
    if (bestLag > 1 && bestLag < maxLag)
    {
        const float left = normalizedDifference[static_cast<size_t>(bestLag - 1)];
        const float centre = normalizedDifference[static_cast<size_t>(bestLag)];
        const float right = normalizedDifference[static_cast<size_t>(bestLag + 1)];
        const float denominator = left - 2.0f * centre + right;
        if (std::abs(denominator) > 1.0e-8f)
            refinedLag += juce::jlimit(-0.5f, 0.5f, 0.5f * (left - right) / denominator);
    }

    const double hz = sampleRate / static_cast<double>(refinedLag);
    if (hz < minimumPitchHz || hz > maximumPitchHz)
        return -1.0f;

    const float midi = static_cast<float>(69.0 + 12.0 * std::log2(hz / 440.0));
    return midi >= 28.0f && midi <= 90.0f ? midi : -1.0f;
}

int ProjectAuroraAudioProcessor::quantizeMidiNote(float midiNote, int root, int scale) const
{
    if (midiNote < 0.0f)
        return -1;

    const auto& allowed = scales[static_cast<size_t>(juce::jlimit(0, 6, scale))];
    const int firstCandidate = static_cast<int>(std::floor(midiNote)) - 2;
    const int lastCandidate = static_cast<int>(std::ceil(midiNote)) + 2;
    int bestNote = -1;
    float bestDistance = std::numeric_limits<float>::max();

    for (int candidate = firstCandidate; candidate <= lastCandidate; ++candidate)
    {
        const int pitchClass = (candidate - root + 120) % 12;
        bool valid = false;
        for (int note : allowed)
        {
            if (note >= 0 && note == pitchClass)
            {
                valid = true;
                break;
            }
        }

        const float distance = std::abs(static_cast<float>(candidate) - midiNote);
        if (valid && distance < bestDistance)
        {
            bestDistance = distance;
            bestNote = candidate;
        }
    }
    return bestNote;
}

void ProjectAuroraAudioProcessor::analysePitch(float sample)
{
    analysisAccumulator += sample;
    if (++analysisDecimationCount < analysisDecimationFactor)
        return;

    const float analysisSample = analysisAccumulator / static_cast<float>(analysisDecimationFactor);
    analysisAccumulator = 0.0f;
    analysisDecimationCount = 0;

    analysisBuffer[static_cast<size_t>(analysisWrite)] = analysisSample;
    analysisWrite = (analysisWrite + 1) % analysisBufferSize;
    analysisSamplesWritten = juce::jmin(analysisBufferSize, analysisSamplesWritten + 1);
    if (analysisSamplesWritten < analysisBufferSize || ++samplesSinceAnalysis < analysisHopSize)
        return;

    samplesSinceAnalysis = 0;
    std::array<float, analysisBufferSize> ordered {};
    for (int i = 0; i < analysisBufferSize; ++i)
        ordered[static_cast<size_t>(i)] = analysisBuffer[static_cast<size_t>((analysisWrite + i) % analysisBufferSize)];

    const double analysisSampleRate = currentSampleRate / static_cast<double>(analysisDecimationFactor);
    detectedMidi = detectMidiNote(ordered.data(), analysisBufferSize, analysisSampleRate);

    if (detectedMidi >= 0.0f)
    {
        const int root = static_cast<int>(parameters.getRawParameterValue("KEY")->load());
        const int scale = static_cast<int>(parameters.getRawParameterValue("SCALE")->load());
        targetMidi = quantizeMidiNote(detectedMidi, root, scale);
        targetShiftSemitones = targetMidi >= 0 ? static_cast<float>(targetMidi) - detectedMidi : 0.0f;
        samplesSinceValidDetection = 0;
        pitchActive = targetMidi >= 0;
        detectedMidiAtomic.store(static_cast<int>(std::lround(detectedMidi)));
        targetMidiAtomic.store(targetMidi);
    }
    else
    {
        samplesSinceValidDetection = juce::jmin(
            samplesSinceValidDetection + analysisHopSize * analysisDecimationFactor,
            static_cast<int>(currentSampleRate * 2.0));

        // Hold through short unvoiced consonants, then release smoothly instead of chasing noise.
        if (samplesSinceValidDetection > static_cast<int>(currentSampleRate * 0.12))
        {
            pitchActive = false;
            targetShiftSemitones = 0.0f;
            detectedMidiAtomic.store(-1);
            targetMidiAtomic.store(-1);
        }
    }
}

void ProjectAuroraAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midi);

    const int channels = buffer.getNumChannels();
    const int samples = buffer.getNumSamples();
    const float inDb = parameters.getRawParameterValue("INGAIN")->load();
    const float outDb = parameters.getRawParameterValue("OUTGAIN")->load();
    const float amount = parameters.getRawParameterValue("AMOUNT")->load() * 0.01f;
    const float speed = parameters.getRawParameterValue("SPEED")->load();
    const float fineSemitones = parameters.getRawParameterValue("FINE")->load() * 0.01f;
    const bool bypass = parameters.getRawParameterValue("BYPASS")->load() >= 0.5f;

    inputGain.setTargetValue(juce::Decibels::decibelsToGain(inDb));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outDb));

    const float speedNormalised = juce::jlimit(0.0f, 1.0f, (speed - 1.0f) / 99.0f);
    const float smoothingMilliseconds = 120.0f * std::pow(8.0f / 120.0f, speedNormalised);
    const float smoothing = 1.0f - std::exp(-1.0f / static_cast<float>(currentSampleRate * smoothingMilliseconds * 0.001));

    for (int i = 0; i < samples; ++i)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            mono += buffer.getReadPointer(ch)[i];
        mono /= static_cast<float>(juce::jmax(1, channels));
        analysePitch(mono);

        const float desiredShift = (!bypass && pitchActive)
            ? targetShiftSemitones * amount + fineSemitones
            : 0.0f;

        if (bypass)
            currentShiftSemitones = 0.0f;
        else
            currentShiftSemitones += (desiredShift - currentShiftSemitones) * smoothing;

        correctionCents.store(currentShiftSemitones * 100.0f);
        const float ratio = std::exp2(currentShiftSemitones / 12.0f);
        const float inputLevel = inputGain.getNextValue();
        const float outputLevel = outputGain.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            const float in = data[i] * inputLevel;
            data[i] = shifters[static_cast<size_t>(juce::jmin(ch, 1))].process(in, ratio) * outputLevel;
        }
    }
}

juce::String ProjectAuroraAudioProcessor::noteName(int midiNote)
{
    if (midiNote < 0)
        return "—";
    return juce::String(noteNames[static_cast<size_t>(midiNote % 12)]) + juce::String(midiNote / 12 - 1);
}

juce::String ProjectAuroraAudioProcessor::getDetectedNote() const
{
    return noteName(detectedMidiAtomic.load());
}

juce::String ProjectAuroraAudioProcessor::getTargetNote() const
{
    return noteName(targetMidiAtomic.load());
}

void ProjectAuroraAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void ProjectAuroraAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* ProjectAuroraAudioProcessor::createEditor()
{
    return new ProjectAuroraAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ProjectAuroraAudioProcessor();
}
