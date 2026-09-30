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
#include "BacklogSheddingPolicy.h"
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
    configurationFailed,
    // The stream or channel selection cannot be routed - no channels are
    // selected, or the selected stream is gone. Distinct from
    // configurationFailed: nothing is broken, and the user can fix it by
    // changing the selection.
    invalidSelection
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

    /** Stream currently selected for display. Message thread only. */
    std::uint16_t getActiveStreamId() const noexcept { return activeStream; }

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
        return capture.state.load (std::memory_order_acquire);
    }

    std::size_t getCaptureIncludedWindowCount() const noexcept
    {
        return capture.includedWindows.load (std::memory_order_relaxed);
    }

    std::size_t getCaptureTargetWindowCount() const noexcept
    {
        return capture.targetWindows.load (std::memory_order_relaxed);
    }

    double getCaptureAnalyzedSeconds() const noexcept
    {
        return static_cast<double> (getCaptureIncludedWindowCount())
               * capture.windowSeconds.load (std::memory_order_relaxed);
    }

    double getCaptureWallSpanSeconds() const noexcept
    {
        return capture.wallSpanSeconds.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureFailedWindowCount() const noexcept
    {
        return capture.failedWindows.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureShedWindowCount() const noexcept
    {
        return capture.shedWindows.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureDiscontinuityCount() const noexcept
    {
        return capture.discontinuities.load (std::memory_order_relaxed);
    }

    std::uint64_t getCaptureId() const noexcept
    {
        return capture.requestedId.load (std::memory_order_relaxed);
    }

    /** Returns true while the worker holds a completed capture that can become
        the reference. Gate the reference controls on this rather than on the
        frozen state alone: freezing only means the accumulation finished. */
    bool hasRetainedCapture() const noexcept
    {
        return capture.retainedId.load (std::memory_order_acquire) != 0;
    }

    /** Marks the completed frozen capture as the comparison reference.

        The reference lives until acquisition stops, which releases it along
        with the rest of the acquisition state.

        A true return means the request was accepted, not that it was applied:
        the worker applies it, and drops it if the frozen capture was released
        in between. getDroppedReferenceRequestCount() reports that. */
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
    std::uint64_t getDroppedInputBlockCount() const noexcept { return diagnostics.droppedInputBlocks.load (std::memory_order_relaxed); }

    /** Returns input samples discarded as part of full-queue block drops. */
    std::uint64_t getDroppedInputSampleCount() const noexcept { return diagnostics.droppedInputSamples.load (std::memory_order_relaxed); }

    /** Returns input blocks rejected because their shape or channel mapping was
        invalid, whether the callback or the worker caught it. */
    std::uint64_t getRejectedInputBlockCount() const noexcept
    {
        return diagnostics.rejectedInputBlocks.load (std::memory_order_relaxed)
               + diagnostics.invalidMappedInputBlocks.load (std::memory_order_relaxed)
               + diagnostics.invalidWorkerInputBlocks.load (std::memory_order_relaxed);
    }

    /** Returns sample-index gaps or overlaps observed by the worker. */
    std::uint64_t getInputDiscontinuityCount() const noexcept { return diagnostics.inputDiscontinuities.load (std::memory_order_relaxed); }

    /** Returns analysis windows rejected because samples were non-finite. */
    std::uint64_t getFailedSpectrumWindowCount() const noexcept { return diagnostics.failedSpectrumWindows.load (std::memory_order_relaxed); }

    /** Returns obsolete analysis windows skipped while the input queue was backlogged. */
    std::uint64_t getShedSpectrumWindowCount() const noexcept
    {
        return diagnostics.shedSpectrumWindows.load (std::memory_order_relaxed);
    }

    /** Returns callback blocks ignored while no prepared runtime was active. */
    std::uint64_t getUnconfiguredInputBlockCount() const noexcept
    {
        return diagnostics.unconfiguredInputBlocks.load (std::memory_order_relaxed);
    }

    /** Returns samples ignored while initial configuration was being prepared. */
    std::uint64_t getUnconfiguredInputSampleCount() const noexcept
    {
        return diagnostics.unconfiguredInputSamples.load (std::memory_order_relaxed);
    }

    /** Returns queued blocks discarded across configuration-generation boundaries. */
    std::uint64_t getStaleConfigurationBlockCount() const noexcept
    {
        return diagnostics.staleConfigurationBlocks.load (std::memory_order_relaxed);
    }

    /** Returns asynchronous configuration attempts that failed. */
    std::uint64_t getConfigurationFailureCount() const noexcept
    {
        return diagnostics.configurationFailures.load (std::memory_order_relaxed);
    }

    /** Returns reference requests the worker accepted but could not apply,
        because the frozen capture they named was never retained. */
    std::uint64_t getDroppedReferenceRequestCount() const noexcept
    {
        return diagnostics.droppedReferenceRequests.load (std::memory_order_acquire);
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
    void discardInvalidatedInputRoute() noexcept;
    void publishInputRoute (std::uint16_t streamId,
                            std::size_t channelCount,
                            const int* globalChannelIndices,
                            std::uint64_t generation) noexcept;
    bool readInputRoute (InputRouteSnapshot& route) const noexcept;
    void clearAcquisitionState();
    bool stopWorkerSafely (int timeoutMilliseconds) noexcept;
    void applyReferenceRequest() noexcept;

    // The steps of run(), in the order it takes them. All worker only.
    void republishFrozenCapture() noexcept;
    bool processQueuedInput();
    bool drainHeldCaptureBlock (spectrumviewer::SampleBlockFifo& fifo);
    void noteInputBlockDuration (std::size_t sampleCount) noexcept;
    spectrumviewer::BacklogSheddingPolicy::Action chooseSheddingAction (
        spectrumviewer::BacklogSheddingPolicy& policy,
        spectrumviewer::SampleBlockFifo& fifo);
    void recordRejectedBlock (spectrumviewer::SpectrumAnalysisPipeline::AppendStatus status);
    bool shedObsoleteWindows (spectrumviewer::BacklogSheddingPolicy::Action action);
    void publishReadyFrames();
    void addCaptureWindow (const spectrumviewer::SpectrumAnalysisPipeline::FrameView& frame);
    void updateWindowCounters() noexcept;
    void trackWarmup (const spectrumviewer::SpectrumAnalysisPipeline::AppendResult& appended) noexcept;
    void waitForInput();

    bool finalizeCapturedSpectrum();
    bool publishCapturedSpectrum (bool complete) noexcept;
    bool publishReducedSpectrum (const float* planarPsd,
                                 std::size_t channelCount,
                                 std::size_t binCount,
                                 const spectrumviewer::SpectrumFrameDescriptor& descriptor,
                                 std::int64_t firstSample,
                                 std::uint64_t sequence,
                                 spectrumviewer::SpectrumCaptureFrameStatus captureStatus = {}) noexcept;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> tryGetDisplayAnalysis() const noexcept
    {
        std::unique_lock<std::mutex> lock (displayAnalysisMutex, std::try_to_lock);
        return lock.owns_lock() ? displayAnalysis
                                : std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> {};
    }

    Array<int> channels;

    // Depth in whole callback blocks, and the only thing standing between a
    // worker stall and a discontinuity. A Fine capture assembles 2 s windows,
    // and one dropped block anywhere inside a window resets history and throws
    // that whole window away, so the queue has to absorb a complete
    // estimate-reduce-baseline burst without overflowing.
    static constexpr std::size_t INPUT_QUEUE_CAPACITY = 32;
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
    // A one-shot command from the message thread to the worker: the selection
    // the live route was built from is no longer routable, so release it. The
    // worker has to do the releasing because it is the input route seqlock's
    // only writer while acquisition runs.
    std::atomic<bool> inputRouteInvalidated { false };
    std::atomic<bool> acquisitionRunning { false };
    std::atomic<std::size_t> activeAudioCallbacks { 0 };
    std::atomic<std::size_t> warmupSampleCount { 0 };
    std::atomic<std::size_t> warmupTargetSampleCount { 0 };

    /** Counts of input and analysis windows the pipeline lost, rejected or
        skipped. The callback and the worker write them and the message thread
        reads them. Reset when acquisition starts and kept after it stops, so a
        run can still be inspected once it is over. */
    struct TransportDiagnostics
    {
        std::atomic<std::uint64_t> droppedInputBlocks { 0 };
        std::atomic<std::uint64_t> droppedInputSamples { 0 };
        std::atomic<std::uint64_t> rejectedInputBlocks { 0 };
        std::atomic<std::uint64_t> invalidMappedInputBlocks { 0 };
        // Worker-side counterpart of the two above. Kept separate because
        // rejectedInputBlocks is overwritten from the FIFO's own count.
        std::atomic<std::uint64_t> invalidWorkerInputBlocks { 0 };
        std::atomic<std::uint64_t> inputDiscontinuities { 0 };
        std::atomic<std::uint64_t> failedSpectrumWindows { 0 };
        std::atomic<std::uint64_t> shedSpectrumWindows { 0 };
        std::atomic<std::uint64_t> unconfiguredInputBlocks { 0 };
        std::atomic<std::uint64_t> unconfiguredInputSamples { 0 };
        std::atomic<std::uint64_t> staleConfigurationBlocks { 0 };
        std::atomic<std::uint64_t> configurationFailures { 0 };
        std::atomic<std::uint64_t> droppedReferenceRequests { 0 };

        void reset() noexcept;
    };

    /** The capture in progress or on screen.

        The atomics are published to the message thread. The fields after them
        belong to the worker alone, so only reset() touches them from anywhere
        else, and only while the worker is stopped. */
    struct CaptureRuntimeState
    {
        std::atomic<SpectrumCaptureState> state { SpectrumCaptureState::live };
        // What state becomes if the runtime replacing a capture fails to build.
        std::atomic<SpectrumCaptureState> replacementFailureState {
            SpectrumCaptureState::frozen
        };
        std::atomic<std::uint64_t> requestedId { 0 };
        // Nonzero only once the worker owns `completed` for that id.
        std::atomic<std::uint64_t> retainedId { 0 };
        std::atomic<std::size_t> targetWindows { 0 };
        std::atomic<std::size_t> includedWindows { 0 };
        std::atomic<double> windowSeconds { 0.0 };
        std::atomic<double> wallSpanSeconds { 0.0 };
        std::atomic<std::uint64_t> failedWindows { 0 };
        std::atomic<std::uint64_t> shedWindows { 0 };
        std::atomic<std::uint64_t> discontinuities { 0 };

        // Worker only. The sample span is anchored on included windows, so it
        // always runs forward and satisfies CapturedSpectrum's metadata
        // contract.
        std::shared_ptr<const spectrumviewer::CapturedSpectrum> completed;
        std::int64_t firstSample = 0;
        std::int64_t lastSampleExclusive = 0;
        std::uint64_t lastFrameSequence = 0;
        std::uint64_t lastPublishedDisplaySettings = 0;
        std::uint64_t lastPublishedComparisonSettings = 0;
        bool hasFirstSample = false;
        bool completionPending = false;

        /** Zeroes the progress and quality counts an accumulation reports.
            Atomics only, so any thread may call it. */
        void resetProgress() noexcept;

        /** Forgets the analysed span and publication watermarks of the
            previous runtime. Worker only. */
        void resetWindowTracking() noexcept;

        /** Back to live with nothing captured, retained or in progress. Does
            not touch requestedId, which numbers requests across runtimes. */
        void reset() noexcept;
    };

    TransportDiagnostics diagnostics;
    CaptureRuntimeState capture;
    std::uint64_t nextCaptureId = 1;

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
    std::shared_ptr<const spectrumviewer::CapturedSpectrum> spectrumReference;

    // Worker only: the duration of the last input block, which sets how often
    // the worker polls for the next one. See waitForInput().
    double lastInputBlockMilliseconds = 0.0;

    uint16 activeStream = 0;

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
