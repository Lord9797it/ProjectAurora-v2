#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr int analysisSize = 2048;
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
    p.push_back(std::make_unique<juce::AudioParameterChoice>("KEY", "Key", juce::StringArray{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}, 0));
    p.push_back(std::make_unique<juce::AudioParameterChoice>("SCALE", "Scale", juce::StringArray{"Chromatic","Major","Minor","Harmonic Minor","Mixolydian","Major Pentatonic","Minor Pentatonic"}, 1));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("SPEED", "Correction Speed", juce::NormalisableRange<float>(1.0f, 100.0f, 0.1f), 35.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("AMOUNT", "Correction Amount", juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("FINE", "Fine Tune", juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("INGAIN", "Input Gain", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("OUTGAIN", "Output Gain", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    return { p.begin(), p.end() };
}

void ProjectAuroraAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    analysisBuffer.assign(analysisSize, 0.0f);
    analysisWrite = samplesSinceAnalysis = analysisSamplesWritten = 0;
    detectedMidi = targetMidi = -1;
    currentShiftSemitones = 0.0f;
    for (auto& s : shifters) { s.prepare(4096); s.reset(); }
    inputGain.reset(sampleRate, 0.02);
    outputGain.reset(sampleRate, 0.02);
    inputGain.setCurrentAndTargetValue(1.0f);
    outputGain.setCurrentAndTargetValue(1.0f);
    setLatencySamples(0);
    juce::ignoreUnused(samplesPerBlock);
}

void ProjectAuroraAudioProcessor::releaseResources() {}

bool ProjectAuroraAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void ProjectAuroraAudioProcessor::SimplePitchShifter::prepare(int maxDelay)
{
    delaySize = juce::jmax(256, maxDelay);
    buffer.assign(static_cast<size_t>(delaySize), 0.0f);
    reset();
}
void ProjectAuroraAudioProcessor::SimplePitchShifter::reset()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
    phase = 0.0f;
}
float ProjectAuroraAudioProcessor::SimplePitchShifter::process(float input, float ratio)
{
    if (buffer.empty()) return input;
    buffer[static_cast<size_t>(writeIndex)] = input;
    ratio = juce::jlimit(0.5f, 2.0f, ratio);
    // Dual-head delay-line pitch shifter. This is a lightweight prototype; large shifts can sound grainy.
    phase += (1.0f - ratio) / static_cast<float>(delaySize - 64);
    while (phase >= 1.0f) phase -= 1.0f;
    while (phase < 0.0f) phase += 1.0f;
    const float delayA = 32.0f + phase * static_cast<float>(delaySize - 64);
    const float phaseB = std::fmod(phase + 0.5f, 1.0f);
    const float delayB = 32.0f + phaseB * static_cast<float>(delaySize - 64);
    auto read = [this](float delay)
    {
        float pos = static_cast<float>(writeIndex) - delay;
        while (pos < 0.0f) pos += static_cast<float>(delaySize);
        const int i0 = static_cast<int>(pos) % delaySize;
        const int i1 = (i0 + 1) % delaySize;
        const float frac = pos - std::floor(pos);
        return buffer[static_cast<size_t>(i0)] * (1.0f - frac) + buffer[static_cast<size_t>(i1)] * frac;
    };
    const float a = read(delayA), b = read(delayB);
    const float blend = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
    const float out = a * std::cos(blend * juce::MathConstants<float>::halfPi) + b * std::sin(blend * juce::MathConstants<float>::halfPi);
    writeIndex = (writeIndex + 1) % delaySize;
    if (std::abs(1.0f - ratio) < 0.0005f) return input;
    return out;
}

int ProjectAuroraAudioProcessor::detectMidiNote(const float* samples, int count, double sampleRate)
{
    if (count < 512) return -1;
    float rms = 0.0f;
    for (int i = 0; i < count; ++i) rms += samples[i] * samples[i];
    rms = std::sqrt(rms / static_cast<float>(count));
    if (rms < 0.008f) return -1;
    const int minLag = juce::jmax(2, static_cast<int>(sampleRate / 1000.0));
    const int maxLag = juce::jmin(count / 2, static_cast<int>(sampleRate / 65.0));
    float best = 0.0f;
    int bestLag = -1;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        float corr = 0.0f, normA = 0.0f, normB = 0.0f;
        for (int i = 0; i < count - lag; i += 2)
        {
            const float a = samples[i], b = samples[i + lag];
            corr += a * b; normA += a * a; normB += b * b;
        }
        const float denom = std::sqrt(normA * normB) + 1.0e-9f;
        const float score = corr / denom;
        if (score > best) { best = score; bestLag = lag; }
    }
    if (bestLag < 0 || best < 0.72f) return -1;
    const double hz = sampleRate / static_cast<double>(bestLag);
    const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0)));
    if (midi < 28 || midi > 90) return -1;
    return midi;
}

int ProjectAuroraAudioProcessor::quantizeMidiNote(int midiNote, int root, int scale) const
{
    if (midiNote < 0) return -1;
    const auto& allowed = scales[static_cast<size_t>(juce::jlimit(0, 6, scale))];
    int bestNote = midiNote;
    int bestDistance = 100;
    for (int candidate = midiNote - 6; candidate <= midiNote + 6; ++candidate)
    {
        const int pc = (candidate - root + 120) % 12;
        bool valid = false;
        for (int n : allowed) if (n >= 0 && n == pc) { valid = true; break; }
        const int distance = std::abs(candidate - midiNote);
        if (valid && distance < bestDistance) { bestDistance = distance; bestNote = candidate; }
    }
    return bestNote;
}

void ProjectAuroraAudioProcessor::analysePitch(float sample)
{
    analysisBuffer[static_cast<size_t>(analysisWrite)] = sample;
    analysisWrite = (analysisWrite + 1) % analysisSize;
    analysisSamplesWritten = juce::jmin(analysisSize, analysisSamplesWritten + 1);
    if (analysisSamplesWritten < analysisSize || ++samplesSinceAnalysis < 512) return;
    samplesSinceAnalysis = 0;
    std::array<float, analysisSize> ordered {};
    for (int i = 0; i < analysisSize; ++i) ordered[static_cast<size_t>(i)] = analysisBuffer[static_cast<size_t>((analysisWrite + i) % analysisSize)];
    detectedMidi = detectMidiNote(ordered.data(), analysisSize, currentSampleRate);
    const int root = static_cast<int>(parameters.getRawParameterValue("KEY")->load());
    const int scale = static_cast<int>(parameters.getRawParameterValue("SCALE")->load());
    targetMidi = quantizeMidiNote(detectedMidi, root, scale);
    detectedMidiAtomic.store(detectedMidi);
    targetMidiAtomic.store(targetMidi);
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
    const float fine = parameters.getRawParameterValue("FINE")->load() * 0.01f;
    inputGain.setTargetValue(juce::Decibels::decibelsToGain(inDb));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(outDb));
    for (int i = 0; i < samples; ++i)
    {
        float mono = 0.0f;
        for (int ch = 0; ch < channels; ++ch) mono += buffer.getReadPointer(ch)[i];
        mono /= static_cast<float>(juce::jmax(1, channels));
        analysePitch(mono);
        float desiredShift = 0.0f;
        if (detectedMidi >= 0 && targetMidi >= 0)
            desiredShift = static_cast<float>(targetMidi - detectedMidi) * amount + fine;
        const float smoothing = juce::jlimit(0.001f, 0.25f, 1.0f / (speed * 0.08f));
        currentShiftSemitones += (desiredShift - currentShiftSemitones) * smoothing;
        correctionCents.store(currentShiftSemitones * 100.0f);
        const float ratio = std::pow(2.0f, currentShiftSemitones / 12.0f);
        const float ig = inputGain.getNextValue(), og = outputGain.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            const float in = data[i] * ig;
            data[i] = shifters[static_cast<size_t>(juce::jmin(ch, 1))].process(in, ratio) * og;
        }
    }
}

juce::String ProjectAuroraAudioProcessor::noteName(int midiNote)
{
    if (midiNote < 0) return "—";
    return juce::String(noteNames[static_cast<size_t>(midiNote % 12)]) + juce::String(midiNote / 12 - 1);
}
juce::String ProjectAuroraAudioProcessor::getDetectedNote() const { return noteName(detectedMidiAtomic.load()); }
juce::String ProjectAuroraAudioProcessor::getTargetNote() const { return noteName(targetMidiAtomic.load()); }

void ProjectAuroraAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}
void ProjectAuroraAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* ProjectAuroraAudioProcessor::createEditor() { return new ProjectAuroraAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ProjectAuroraAudioProcessor(); }
