# Project Aurora — Windows VST3

Project Aurora is an experimental monophonic vocal pitch-correction plug-in built with C++ and JUCE. GitHub Actions builds the Windows VST3 package.

## DSP changes in this revision

- Replaced integer-note autocorrelation with a YIN-style normalized difference estimator and fractional-period interpolation. This lets the correction respond to cents between notes.
- Fixed the dual-delay crossfade so each read head fades out at its buffer wrap. The previous window weighting could leave a head audible while its delay jumped back to the start, which creates clicks and grainy artifacts.
- Shortened the delay window and added cubic read interpolation. The processor reports its 512-sample fixed latency to the host and uses a matching dry delay.
- Added a brief note hold through unvoiced consonants and a speed control mapped to a useful 8–120 ms correction glide.
- Added a bypass parameter that keeps the same plug-in delay for reliable host compensation.

## Build on GitHub

The root workflow builds the VST3 on Windows. Pushes to main, pull requests targeting main, and manual workflow runs produce a Project-Aurora-VST3-Windows artifact. Download the artifact from the successful Actions run and copy the included VST3 bundle into the plug-in folder used by your DAW.

## Limits

This remains an experimental monophonic pitch shifter. It does not preserve formants, and large corrections, breathy/noisy input, polyphonic material, or octave errors can still sound unnatural. It has not been evaluated against the user's recording because no audio sample was available.


## Pitch engine

The correction path uses time-domain PSOLA (pitch-synchronous overlap-add) for voiced, monophonic material. Pitch-synchronous grains are normalized while overlapping; unvoiced consonants and very small corrections crossfade to a latency-matched dry path. The reported latency is recalculated during prepare from the sample rate and pitch range so the grain lookahead remains available.
