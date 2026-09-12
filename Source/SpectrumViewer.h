/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2019 Translational NeuroEngineering Laboratory

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef SPECTRUM_VIEWER_H_INCLUDED
#define SPECTRUM_VIEWER_H_INCLUDED

#include <ProcessorHeaders.h>

#include "AsyncSpectrumAnalysis.h"
#include "SampleBlockFifo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define MAX_CHANS 8

enum DisplayType
{
    POWER_SPECTRUM = 1,
    SPECTROGRAM = 2
};

enum class SpectrumAnalysisProfile
{
    fast = 1,
    balanced = 2,
    fine = 3
};

enum class SpectrumAnalysisReadiness
{
    stopped,
    preparing,
    warmingUp,
    live,
    configurationFailed
};

enum class SpectrumAmplitudeDisplay
{
    psd = 1,
    asd = 2
};

enum class SpectrumCaptureState
{
    live,
    preparing,
    capturing,
    frozen,
    restoringLive,
    failed
};

/*

	Compute and display power spectra for incoming
	continuous channels.

*/
class TESTABLE SpectrumViewer : public GenericProcessor,
                                public Thread
{
public:
    /** Constructor */
    explicit SpectrumViewer (spectrumviewer::AsyncSpectrumAnalysis::Builder configurationBuilder = {});

    /** Destructor */
    ~SpectrumViewer() override;

    /** Register parameters for this processor */
    void registerParameters() override;

    /** Create SpectrumViewerEditor */
    AudioProcessorEditor* createEditor() override;

    /** Called when upstream settings are updated */
    void updateSettings() override;

    /** Update buffers for FFT calculation*/
    void process (AudioBuffer<float>& continuousBuffer) override;

    /** Launch FFT calculation thread */
    bool startAcquisition() override;

    /** Stop FFT calculation thread*/
    bool stopAcquisition() override;

    /** Run FFT calculation in a separate thread */
    void run() override;

    /** Called when parameter value is updated*/
    void parameterValueChanged (Parameter* param) override;

    /** Called by the canvas to get the number of active chans*/
    Array<int> getActiveChans();

    /** Returns the name of the selected channel at a given index */
    const String getChanName (int localIdx);
    const String getChanName (std::uint16_t streamId, int localIdx);

    /** Sets the min/max frequency range*/
    void setFrequencyRange (Range<int>);

    /** Uses the complete one-sided frequency range through Nyquist. */
    void setFullFrequencyRange() noexcept
    {
        beginDisplaySettingsUpdate();
        displayMinimumFrequencyHz.store (0.0, std::memory_order_relaxed);
        displayMaximumFrequencyHz.store (0.0, std::memory_order_relaxed);
        endDisplaySettingsUpdate();
    }

    void setFrequencyScale (spectrumviewer::FrequencyScale scale) noexcept
    {
        beginDisplaySettingsUpdate();
        displayFrequencyScale.store (scale, std::memory_order_relaxed);
        endDisplaySettingsUpdate();
    }

    void setDisplayColumnCount (std::size_t count) noexcept
    {
        beginDisplaySettingsUpdate();
        displayColumnCount.store (std::max<std::size_t> (1, count), std::memory_order_relaxed);
        endDisplaySettingsUpdate();
    }

    void setAperiodicDisplayMode (
        spectrumviewer::AperiodicDisplayMode mode) noexcept
    {
        beginDisplaySettingsUpdate();
        aperiodicDisplayMode.store (mode, std::memory_order_relaxed);
        endDisplaySettingsUpdate();
    }

    /** Returns the frequency step for the currently selected range*/
    float getFreqStep() const noexcept
    {
        const auto active = activeBinWidthHz.load (std::memory_order_acquire);
        return active > 0.0f ? active : tfrParams.freqStep;
    };

    /** Selects and asynchronously prepares an analysis profile. */
    void setAnalysisProfile (SpectrumAnalysisProfile profile);

    /** Asynchronously starts a non-overlapping Fine-profile capture. */
    bool startSpectrumCapture (double durationSeconds);
    void cancelSpectrumCapture();
    void returnToLive() { cancelSpectrumCapture(); }

    SpectrumCaptureState getCaptureState() const noexcept
    {
        return captureState.load (std::memory_order_acquire);
    }

    std::size_t getCaptureIncludedWindowCount() const noexcept
    {
        return captureIncludedWindows.load (std::memory_order_relaxed);
    }

    std::size_t getCaptureTargetWindowCount() const noexcept
    {
        return captureTargetWindows.load (std::memory_order_relaxed);
    }

    double getCaptureAnalyzedSeconds() const noexcept
    {
        return static_cast<double> (getCaptureIncludedWindowCount())
               * captureWindowSeconds.load (std::memory_order_relaxed);
    }

    double getCaptureWallSpanSeconds() const noexcept
    {
        return captureWallSpanSeconds.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureFailedWindowCount() const noexcept
    {
        return captureFailedWindows.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureShedWindowCount() const noexcept
    {
        return captureShedWindows.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureDiscontinuityCount() const noexcept
    {
        return captureDiscontinuities.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureId() const noexcept
    {
        return requestedCaptureId.load (std::memory_order_relaxed);
    }

    /** Marks the completed frozen capture as the session-local reference. */
    bool setCurrentCaptureAsReference() noexcept;
    void clearSpectrumReference() noexcept;
    void setSpectrumComparisonMode (spectrumviewer::SpectrumComparisonMode mode) noexcept;

    spectrumviewer::SpectrumComparisonMode getSpectrumComparisonMode() const noexcept
    {
        return comparisonMode.load (std::memory_order_relaxed);
    }

    spectrumviewer::SpectrumReferenceCompatibility getReferenceCompatibility() const noexcept
    {
        return referenceCompatibility.load (std::memory_order_acquire);
    }

    bool hasSpectrumReference() const noexcept
    {
        return referenceCaptureId.load (std::memory_order_acquire) != 0;
    }

    std::uint64_t getReferenceCaptureId() const noexcept
    {
        return referenceCaptureId.load (std::memory_order_relaxed);
    }

    std::int64_t getReferenceCapturedAtMilliseconds() const noexcept
    {
        return referenceCapturedAtMilliseconds.load (std::memory_order_relaxed);
    }

    SpectrumAnalysisProfile getAnalysisProfile() const noexcept
    {
        return analysisProfile.load (std::memory_order_relaxed);
    }

    SpectrumAnalysisReadiness getAnalysisReadiness() const noexcept
    {
        return analysisReadiness.load (std::memory_order_acquire);
    }

    bool isAnalysisConfigurationPending() const noexcept
    {
        return configurationPending.load (std::memory_order_acquire);
    }

    bool hasActiveAnalysis() const noexcept
    {
        return activeConfigurationGeneration.load (std::memory_order_acquire) != 0;
    }

    std::size_t getWarmupSampleCount() const noexcept
    {
        return warmupSampleCount.load (std::memory_order_relaxed);
    }

    std::size_t getWarmupTargetSampleCount() const noexcept
    {
        return warmupTargetSampleCount.load (std::memory_order_relaxed);
    }

    /** Returns whole input blocks dropped because the worker queue was full. */
    std::uint64_t getDroppedInputBlockCount() const noexcept { return droppedInputBlocks.load (std::memory_order_relaxed); }

    /** Returns input samples discarded as part of full-queue block drops. */
    std::uint64_t getDroppedInputSampleCount() const noexcept { return droppedInputSamples.load (std::memory_order_relaxed); }

    /** Returns input blocks rejected because their shape or channel mapping was invalid. */
    std::uint64_t getRejectedInputBlockCount() const noexcept
    {
        return rejectedInputBlocks.load (std::memory_order_relaxed)
               + invalidMappedInputBlocks.load (std::memory_order_relaxed);
    }

    /** Returns sample-index gaps or overlaps observed by the worker. */
    std::uint64_t getInputDiscontinuityCount() const noexcept { return inputDiscontinuities.load (std::memory_order_relaxed); }

    /** Returns analysis windows rejected because samples were non-finite. */
    std::uint64_t getFailedSpectrumWindowCount() const noexcept { return failedSpectrumWindows.load (std::memory_order_relaxed); }

    /** Returns obsolete analysis windows skipped while the input queue was backlogged. */
    std::uint64_t getShedSpectrumWindowCount() const noexcept
    {
        return shedSpectrumWindows.load (std::memory_order_relaxed);
    }

    /** Returns callback blocks ignored while no prepared runtime was active. */
    std::uint64_t getUnconfiguredInputBlockCount() const noexcept
    {
        return unconfiguredInputBlocks.load (std::memory_order_relaxed);
    }

    /** Returns samples ignored while initial configuration was being prepared. */
    std::uint64_t getUnconfiguredInputSampleCount() const noexcept
    {
        return unconfiguredInputSamples.load (std::memory_order_relaxed);
    }

    /** Returns queued blocks discarded across configuration-generation boundaries. */
    std::uint64_t getStaleConfigurationBlockCount() const noexcept
    {
        return staleConfigurationBlocks.load (std::memory_order_relaxed);
    }

    /** Returns asynchronous configuration attempts that failed. */
    std::uint64_t getConfigurationFailureCount() const noexcept
    {
        return configurationFailures.load (std::memory_order_relaxed);
    }

    /** Consumes all pending display frames and passes only the newest complete frame to consumer. */
    template <typename Consumer>
    bool consumeLatestSpectrumFrame (Consumer&& consumer)
    {
        auto analysis = tryGetDisplayAnalysis();
        return analysis != nullptr
               && analysis->getFrameFifo().tryPopLatest (std::forward<Consumer> (consumer));
    }

    std::uint64_t getDroppedSpectrumFrameCount() const noexcept
    {
        auto analysis = tryGetDisplayAnalysis();
        return analysis != nullptr ? analysis->getFrameFifo().getDroppedFrameCount() : 0;
    }

    std::uint64_t getStaleSpectrumFrameCount() const noexcept
    {
        auto analysis = tryGetDisplayAnalysis();
        return analysis != nullptr ? analysis->getFrameFifo().getStaleFrameCount() : 0;
    }

    /** Type of visualization */
    DisplayType displayType;

private:
    struct DisplaySettings
    {
        std::uint64_t sequence = 0;
        std::size_t columnCount = 1;
        spectrumviewer::FrequencyScale frequencyScale = spectrumviewer::FrequencyScale::linear;
        spectrumviewer::AperiodicDisplayMode aperiodicMode =
            spectrumviewer::AperiodicDisplayMode::off;
        double minimumFrequencyHz = 0.0;
        double maximumFrequencyHz = 0.0;
    };

    void beginDisplaySettingsUpdate() noexcept
    {
        displaySettingsSequence.fetch_add (1, std::memory_order_acq_rel);
    }

    void endDisplaySettingsUpdate() noexcept
    {
        displaySettingsSequence.fetch_add (1, std::memory_order_release);
    }

    /** Reads a stable display-settings snapshot, or fails after a bounded
        number of attempts rather than spinning against a burst of updates. */
    bool tryReadDisplaySettings (DisplaySettings& settings) const noexcept;

    // Both seqlock readers give up after this many attempts. readInputRoute
    // runs on the audio callback, where spinning is never acceptable;
    // tryReadDisplaySettings runs on the worker, where it would stall frame
    // publication.
    static constexpr int seqlockReadAttempts = 3;

    struct InputRouteSnapshot
    {
        std::uint16_t streamId = 0;
        std::size_t channelCount = 0;
        std::array<int, MAX_CHANS> globalChannelIndices {};
        std::uint64_t generation = 0;
    };

    void requestAnalysisConfiguration (bool forCapture = false);
    void adoptPreparedAnalysis();
    bool updateRequestedInputRoute (DataStream* stream);
    void requestInputRouteReplacement();
    void rejectInputRouteReplacement() noexcept;
    void publishInputRoute (std::uint16_t streamId,
                            std::size_t channelCount,
                            const int* globalChannelIndices,
                            std::uint64_t generation) noexcept;
    bool readInputRoute (InputRouteSnapshot& route) const noexcept;
    void clearAcquisitionState();
    bool stopWorkerSafely (int timeoutMilliseconds) noexcept;
    void applyReferenceRequest() noexcept;
    void finalizeCapturedSpectrum();
    bool publishCapturedSpectrum (bool complete) noexcept;
    bool publishReducedSpectrum (const float* planarPsd,
                                 std::size_t channelCount,
                                 std::size_t binCount,
                                 const spectrumviewer::SpectrumFrameDescriptor& descriptor,
                                 std::int64_t firstSample,
                                 std::uint64_t sequence,
                                 spectrumviewer::SpectrumCaptureFrameStatus capture = {}) noexcept;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> tryGetDisplayAnalysis() const noexcept
    {
        std::unique_lock<std::mutex> lock (displayAnalysisMutex, std::try_to_lock);
        return lock.owns_lock() ? displayAnalysis
                                : std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> {};
    }

    Array<int> channels;

    static constexpr std::size_t INPUT_QUEUE_CAPACITY = 8;
    // GenericProcessor reports JUCE's nominal 128-sample graph quantum, not an
    // upper bound for stream payloads. Reserve enough room for the largest
    // callback supported by the GUI's audio settings while retaining a larger
    // value if a future host supplies one.
    static constexpr std::size_t MINIMUM_INPUT_BLOCK_CAPACITY = 8192;
    // Display frames are replaceable latest-state data: one may be read, one
    // ready, and one provides scheduling tolerance without retaining history.
    static constexpr std::size_t OUTPUT_QUEUE_CAPACITY = 3;
    std::unique_ptr<spectrumviewer::SampleBlockFifo> inputFifo;
    spectrumviewer::AsyncSpectrumAnalysis asynchronousAnalysis;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> activeAnalysis;
    mutable std::mutex displayAnalysisMutex;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> displayAnalysis;
    std::array<int, MAX_CHANS> acquisitionChannels {};
    std::array<int, MAX_CHANS> acquisitionGlobalChannels {};
    std::array<std::string, MAX_CHANS> acquisitionChannelUnits {};
    std::size_t acquisitionChannelCount = 0;
    uint16 acquisitionStream = 0;
    std::size_t acquisitionMaximumInputBlockSamples = 0;
    double acquisitionSampleRateHz = 0.0;
    std::atomic<std::uint64_t> inputRouteSequence { 0 };
    std::atomic<std::uint16_t> publishedInputStream { 0 };
    std::atomic<std::size_t> publishedInputChannelCount { 0 };
    std::array<std::atomic<int>, MAX_CHANS> publishedGlobalChannels {};
    std::atomic<std::uint64_t> publishedInputGeneration { 0 };
    std::atomic<std::uint64_t> activeConfigurationGeneration { 0 };
    std::atomic<float> activeBinWidthHz { 0.0f };
    std::atomic<std::size_t> displayColumnCount { 800 };
    std::atomic<spectrumviewer::FrequencyScale> displayFrequencyScale {
        spectrumviewer::FrequencyScale::linear
    };
    std::atomic<spectrumviewer::AperiodicDisplayMode> aperiodicDisplayMode {
        spectrumviewer::AperiodicDisplayMode::off
    };
    std::atomic<double> displayMinimumFrequencyHz { 0.0 };
    // A non-positive maximum means the active stream's Nyquist frequency.
    std::atomic<double> displayMaximumFrequencyHz { 0.0 };
    // Even values identify stable control-thread snapshots; odd means update in progress.
    std::atomic<std::uint64_t> displaySettingsSequence { 0 };
    std::atomic<std::uint64_t> requestedConfigurationGeneration { 0 };
    std::uint64_t nextConfigurationGeneration = 1;
    std::atomic<SpectrumAnalysisProfile> analysisProfile { SpectrumAnalysisProfile::fast };
    std::atomic<SpectrumAnalysisReadiness> analysisReadiness { SpectrumAnalysisReadiness::stopped };
    std::atomic<bool> configurationPending { false };
    std::atomic<bool> acquisitionRunning { false };
    std::atomic<std::size_t> activeAudioCallbacks { 0 };
    std::atomic<std::size_t> warmupSampleCount { 0 };
    std::atomic<std::size_t> warmupTargetSampleCount { 0 };
    std::atomic<std::uint64_t> droppedInputBlocks { 0 };
    std::atomic<std::uint64_t> droppedInputSamples { 0 };
    std::atomic<std::uint64_t> rejectedInputBlocks { 0 };
    std::atomic<std::uint64_t> invalidMappedInputBlocks { 0 };
    std::atomic<std::uint64_t> inputDiscontinuities { 0 };
    std::atomic<std::uint64_t> failedSpectrumWindows { 0 };
    std::atomic<std::uint64_t> shedSpectrumWindows { 0 };
    std::atomic<std::uint64_t> unconfiguredInputBlocks { 0 };
    std::atomic<std::uint64_t> unconfiguredInputSamples { 0 };
    std::atomic<std::uint64_t> staleConfigurationBlocks { 0 };
    std::atomic<std::uint64_t> configurationFailures { 0 };
    std::atomic<SpectrumCaptureState> captureState { SpectrumCaptureState::live };
    std::atomic<SpectrumCaptureState> replacementFailureCaptureState {
        SpectrumCaptureState::frozen
    };
    std::atomic<std::uint64_t> requestedCaptureId { 0 };
    std::atomic<std::size_t> captureTargetWindows { 0 };
    std::atomic<std::size_t> captureIncludedWindows { 0 };
    std::atomic<double> captureWindowSeconds { 0.0 };
    std::atomic<double> captureWallSpanSeconds { 0.0 };
    std::atomic<std::uint64_t> captureFailedWindows { 0 };
    std::atomic<std::uint64_t> captureShedWindows { 0 };
    std::atomic<std::uint64_t> captureDiscontinuities { 0 };
    std::uint64_t nextCaptureId = 1;

    // The following capture fields are owned exclusively by the worker thread.
    std::int64_t captureFirstSample = 0;
    std::int64_t captureLastSampleExclusive = 0;
    std::uint64_t captureLastFrameSequence = 0;
    std::uint64_t captureLastPublishedDisplaySettings = 0;
    std::uint64_t captureLastPublishedComparisonSettings = 0;
    bool captureHasFirstSample = false;
    bool captureCompletionPending = false;

    // Reference commands are published by the message thread and applied by
    // the spectrum worker. Spectral arrays remain worker-owned and immutable.
    static constexpr std::uint64_t CLEAR_REFERENCE_REQUEST =
        std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> referenceRequest { 0 };
    std::atomic<std::uint64_t> comparisonSettingsSequence { 0 };
    std::atomic<spectrumviewer::SpectrumComparisonMode> comparisonMode {
        spectrumviewer::SpectrumComparisonMode::absolute
    };
    std::atomic<spectrumviewer::SpectrumReferenceCompatibility> referenceCompatibility {
        spectrumviewer::SpectrumReferenceCompatibility::noReference
    };
    std::atomic<std::uint64_t> referenceCaptureId { 0 };
    std::atomic<std::int64_t> referenceCapturedAtMilliseconds { 0 };
    std::shared_ptr<const spectrumviewer::CapturedSpectrum> completedCapture;
    std::shared_ptr<const spectrumviewer::CapturedSpectrum> spectrumReference;

    //int bufferSize;
    //int stepSize;
    //int stepsPerBuffer;

    uint16 activeStream = 0;

    // This is to store data in case of switch and we wish to retrive old data
    //AtomicallyShared<Array<FFTWArrayType>> dataBufferII;
    //Array<AtomicallyShared<FFTWArrayType>> updatedDataBuffer;

    struct TFRParameters
    {
        float segLen; // Segment Length
        float winLen; // Window Length
        float stepLen; // Interval between times of interest
        int interpRatio; //

        // Number of freq of interest
        int nFreqs;
        float freqStep;
        int freqStart;
        int freqEnd;

        // Number of times of interest
        int nTimes;

        // Fs (sampling rate?)
        float Fs;

        // frequency of interest
        Array<float> foi;

        float alpha;
    };

    TFRParameters tfrParams;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumViewer);
};

#endif // SPECTRUM_VIEWER_H_INCLUDED
