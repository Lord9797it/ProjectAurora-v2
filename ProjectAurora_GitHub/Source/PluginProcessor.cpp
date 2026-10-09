#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int analysisWindowSize = 1536;
constexpr int analysisHopSize = 256;
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
    currentVoiced = false;

    const int analysisCentreDelay = (analysisBufferSize * analysisDecimationFactor) / 2;
    const int minimumPitchPeriod = static_cast<int>(std::ceil(sampleRate / minimumPitchHz));
    const int psolaLookAhead = juce::jmax(analysisCentreDelay,
        static_cast<int>(std::ceil(static_cast<float>(minimumPitchPeriod) * 1.8f)));
    const int psolaLatency = psolaLookAhead * 2;

    for (auto& shifter : shifters)
    {
        shifter.prepare(sampleRate, psolaLookAhead, psolaLatency);
        shifter.reset();
    }

    inputGain.reset(sampleRate, 0.02);
    outputGain.reset(sampleRate, 0.02);
    inputGain.setCurrentAndTargetValue(1.0f);
    outputGain.setCurrentAndTargetValue(1.0f);
    detectedMidiAtomic.store(-1);
    targetMidiAtomic.store(-1);
    correctionCents.store(0.0f);

    // PSOLA uses lookahead to place pitch-synchronous grains without truncating their future half.
    setLatencySamples(psolaLatency);
    juce::ignoreUnused(samplesPerBlock);
}

void ProjectAuroraAudioProcessor::releaseResources() {}

bool ProjectAuroraAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void ProjectAuroraAudioProcessor::PsolaPitchShifter::prepare(double sampleRate, int lookAhead, int latency)
{
    currentSampleRate = sampleRate;
    lookAheadSamples = juce::jmax(1, lookAhead);
    latencySamples = juce::jmax(lookAheadSamples * 2, latency);

    const int maximumPeriod = static_cast<int>(std::ceil(sampleRate / minimumPitchHz));
    historySize = latencySamples + maximumPeriod * 4 + 64;
    outputSize = historySize;

    audioHistory.assign(static_cast<size_t>(historySize), 0.0f);
    monoHistory.assign(static_cast<size_t>(historySize), 0.0f);
    outputSum.assign(static_cast<size_t>(outputSize), 0.0f);
    outputWeight.assign(static_cast<size_t>(outputSize), 0.0f);
    wetMixBuffer.assign(static_cast<size_t>(outputSize), 0.0f);
    reset();
}

void ProjectAuroraAudioProcessor::PsolaPitchShifter::reset()
{
    std::fill(audioHistory.begin(), audioHistory.end(), 0.0f);
    std::fill(monoHistory.begin(), monoHistory.end(), 0.0f);
    std::fill(outputSum.begin(), outputSum.end(), 0.0f);
    std::fill(outputWeight.begin(), outputWeight.end(), 0.0f);
    std::fill(wetMixBuffer.begin(), wetMixBuffer.end(), 0.0f);
    historyWriteIndex = 0;
    outputReadIndex = 0;
    synthesisPhase = 0.0f;
    wetMix = 0.0f;
}

int ProjectAuroraAudioProcessor::PsolaPitchShifter::wrapIndex(int index, int size) const
{
    int wrapped = index % size;
    if (wrapped < 0)
        wrapped += size;
    return wrapped;
}

float ProjectAuroraAudioProcessor::PsolaPitchShifter::readHistory(
    const std::vector<float>& history, int delay) const
{
    if (history.empty())
        return 0.0f;

    delay = juce::jlimit(0, historySize - 1, delay);
    const int index = wrapIndex(historyWriteIndex - delay, historySize);
    return history[static_cast<size_t>(index)];
}

void ProjectAuroraAudioProcessor::PsolaPitchShifter::addGrain(int markOffset, float periodSamples)
{
    if (outputSize == 0 || historySize == 0)
        return;

    constexpr float halfWindowPeriods = 1.25f;
    const int halfWindow = juce::jlimit(2, historySize / 4,
        static_cast<int>(std::ceil(periodSamples * halfWindowPeriods)));
    const int outputCentre = wrapIndex(outputReadIndex + lookAheadSamples, outputSize);

    for (int offset = -halfWindow; offset <= halfWindow; ++offset)
    {
        const int sourceDelay = lookAheadSamples - markOffset - offset;
        const float source = readHistory(audioHistory, sourceDelay);
        const float phase = static_cast<float>(offset) / static_cast<float>(halfWindow);
        const float window = 0.5f + 0.5f * std::cos(juce::MathConstants<float>::pi * phase);
        const int destination = wrapIndex(outputCentre + offset, outputSize);
        outputSum[static_cast<size_t>(destination)] += source * window;
        outputWeight[static_cast<size_t>(destination)] += window;
    }
}

float ProjectAuroraAudioProcessor::PsolaPitchShifter::process(
    float input, float mono, float pitchHz, float ratio, float shiftSemitones, bool voiced)
{
    if (historySize <= 0 || outputSize <= 0)
        return input;

    audioHistory[static_cast<size_t>(historyWriteIndex)] = input;
    monoHistory[static_cast<size_t>(historyWriteIndex)] = mono;

    const float safePitchHz = juce::jlimit(minimumPitchHz, maximumPitchHz,
        pitchHz > 0.0f ? pitchHz : minimumPitchHz);
    const float periodSamples = static_cast<float>(currentSampleRate) / safePitchHz;
    const float targetWet = voiced
        ? juce::jlimit(0.0f, 1.0f, (std::abs(shiftSemitones) - 0.025f) / 0.20f)
        : 0.0f;
    const float wetCoefficient = 1.0f - std::exp(-1.0f /
        static_cast<float>(currentSampleRate * (targetWet > wetMix ? 0.004 : 0.012)));
    wetMix += (targetWet - wetMix) * wetCoefficient;

    // q is H samples ahead of the current output slot; its source is L samples behind q.
    const int mixDestination = wrapIndex(outputReadIndex + lookAheadSamples, outputSize);
    wetMixBuffer[static_cast<size_t>(mixDestination)] = wetMix;

    if (voiced && targetWet > 0.0f && pitchHz >= minimumPitchHz && pitchHz <= maximumPitchHz)
    {
        const float shiftedHz = pitchHz * juce::jlimit(0.5f, 2.0f, ratio);
        synthesisPhase += shiftedHz / static_cast<float>(currentSampleRate);
        while (synthesisPhase >= 1.0f)
        {
            synthesisPhase -= 1.0f;

            // Align each analysis grain to the strongest positive peak near the expected input period.
            const int searchRadius = juce::jmax(1, static_cast<int>(std::lround(periodSamples * 0.5f)));
            int markOffset = 0;
            float strongestPeak = -std::numeric_limits<float>::max();
            for (int offset = -searchRadius; offset <= searchRadius; ++offset)
            {
                const float candidate = readHistory(monoHistory, lookAheadSamples - offset);
                if (candidate > strongestPeak)
                {
                    strongestPeak = candidate;
                    markOffset = offset;
                }
            }
            addGrain(markOffset, periodSamples);
        }
    }
    else if (!voiced)
    {
        // Pitch phase is undefined on consonants; restart cleanly for the next voiced section.
        synthesisPhase = 0.0f;
    }

    const float dry = readHistory(audioHistory, latencySamples);
    const float weight = outputWeight[static_cast<size_t>(outputReadIndex)];
    const float wet = weight > 1.0e-6f
        ? outputSum[static_cast<size_t>(outputReadIndex)] / weight
        : dry;
    const float mix = wetMixBuffer[static_cast<size_t>(outputReadIndex)];
    const float output = dry + (wet - dry) * mix;

    outputSum[static_cast<size_t>(outputReadIndex)] = 0.0f;
    outputWeight[static_cast<size_t>(outputReadIndex)] = 0.0f;
    wetMixBuffer[static_cast<size_t>(outputReadIndex)] = 0.0f;

    historyWriteIndex = (historyWriteIndex + 1) % historySize;
    outputReadIndex = (outputReadIndex + 1) % outputSize;
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
        currentVoiced = true;
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
        currentVoiced = false;
        samplesSinceValidDetection = juce::jmin(
            samplesSinceValidDetection + analysisHopSize * analysisDecimationFactor,
            static_cast<int>(currentSampleRate * 2.0));

        // Hold through short unvoiced consonants, then release smoothly instead of chasing noise.
        if (samplesSinceValidDetection > static_cast<int>(currentSampleRate * 0.12))
        {
            pitchActive = false;
            targetShiftSemitones = 0.0f;
            detectedMidi = -1.0f;
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
        const float pitchHz = detectedMidi >= 0.0f
            ? static_cast<float>(440.0 * std::exp2((static_cast<double>(detectedMidi) - 69.0) / 12.0))
            : 0.0f;
        const bool voiced = currentVoiced;
        const float inputLevel = inputGain.getNextValue();
        const float outputLevel = outputGain.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            const float in = data[i] * inputLevel;
            data[i] = shifters[static_cast<size_t>(juce::jmin(ch, 1))]
                .process(in, mono * inputLevel, pitchHz, ratio, currentShiftSemitones, voiced) * outputLevel;
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
