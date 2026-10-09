# Project Aurora — GitHub-only Windows VST3 build

Project Aurora is an original experimental pitch-correction VST3 built with C++ and JUCE. The project is designed so GitHub Actions compiles it on a hosted Windows runner; your own PC does not need to compile the code.

## Build from the GitHub website only

1. Download this ZIP and extract it.
2. On GitHub, create a new repository named `ProjectAurora` (public or private).
3. In the new repository choose **Add file → Upload files** and upload the *contents* of the extracted folder, including the hidden `.github` folder. The workflow must appear at `.github/workflows/build-windows.yml` in the repository. If your browser hides `.github`, create that folder/file using **Add file → Create new file** and enter `.github/workflows/build-windows.yml`, then paste the workflow from this project.
4. Commit the files to the `main` branch.
5. Open **Actions → Build Project Aurora VST3 → Run workflow**. A push to `main` also starts the build automatically.
6. When the run succeeds, open its page and download the `ProjectAurora-VST3-Windows` artifact. Extract it; the archive contains the `.vst3` bundle.

The workflow checks out JUCE from its official GitHub repository, configures CMake, builds with MSBuild `/m:1`, packages the VST3 bundle, and uploads it as a GitHub Actions artifact. It does not use Signalsmith Stretch or any other separate pitch-shifting dependency.

## Important DSP limitations

This is an **experimental prototype**, not a commercial-quality Auto-Tune replacement. It uses a basic autocorrelation pitch estimate and a lightweight dual-delay-line shifter; artifacts, octave errors, tracking instability, and latency/quality trade-offs are expected, especially on polyphonic or noisy audio. The controls and GUI are real, but the audio algorithm needs substantial DSP work before professional release. Formant preservation is not implemented.

## License

The project source is provided under the MIT License. JUCE has its own license terms; review the JUCE license and applicable usage terms before distributing builds.
