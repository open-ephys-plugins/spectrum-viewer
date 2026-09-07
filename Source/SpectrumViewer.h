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

/*

	Compute and display power spectra for incoming
	continuous channels.

*/
class SpectrumViewer : public GenericProcessor,
                       public Thread
{
public:
    /** Constructor */
    SpectrumViewer();

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

    /** Sets the min/max frequency range*/
    void setFrequencyRange (Range<int>);

    /** Returns the frequency step for the currently selected range*/
    float getFreqStep() const noexcept
    {
        const auto active = activeBinWidthHz.load (std::memory_order_acquire);
        return active > 0.0f ? active : tfrParams.freqStep;
    };

    /** Selects and asynchronously prepares an analysis profile. */
    void setAnalysisProfile (SpectrumAnalysisProfile profile);

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

    /** Returns input blocks rejected because their shape exceeded the fixed configuration. */
    std::uint64_t getRejectedInputBlockCount() const noexcept { return rejectedInputBlocks.load (std::memory_order_relaxed); }

    /** Returns sample-index gaps or overlaps observed by the worker. */
    std::uint64_t getInputDiscontinuityCount() const noexcept { return inputDiscontinuities.load (std::memory_order_relaxed); }

    /** Returns analysis windows rejected because samples were non-finite. */
    std::uint64_t getFailedSpectrumWindowCount() const noexcept { return failedSpectrumWindows.load (std::memory_order_relaxed); }

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
    void requestAnalysisConfiguration();
    void adoptPreparedAnalysis();
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> tryGetDisplayAnalysis() const noexcept
    {
        std::unique_lock<std::mutex> lock (displayAnalysisMutex, std::try_to_lock);
        return lock.owns_lock() ? displayAnalysis
                                : std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> {};
    }

    Array<int> channels;

    static constexpr std::size_t INPUT_QUEUE_CAPACITY = 8;
    // Display frames are replaceable latest-state data: one may be read, one
    // ready, and one provides scheduling tolerance without retaining history.
    static constexpr std::size_t OUTPUT_QUEUE_CAPACITY = 3;
    std::unique_ptr<spectrumviewer::SampleBlockFifo> inputFifo;
    spectrumviewer::AsyncSpectrumAnalysis asynchronousAnalysis;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> activeAnalysis;
    mutable std::mutex displayAnalysisMutex;
    std::shared_ptr<spectrumviewer::PreparedSpectrumAnalysis> displayAnalysis;
    std::array<int, MAX_CHANS> acquisitionChannels {};
    std::size_t acquisitionChannelCount = 0;
    std::size_t acquisitionMaximumInputBlockSamples = 0;
    double acquisitionSampleRateHz = 0.0;
    std::atomic<std::uint64_t> activeConfigurationGeneration { 0 };
    std::atomic<float> activeBinWidthHz { 0.0f };
    std::atomic<std::uint64_t> requestedConfigurationGeneration { 0 };
    std::uint64_t nextConfigurationGeneration = 1;
    std::atomic<SpectrumAnalysisProfile> analysisProfile { SpectrumAnalysisProfile::fast };
    std::atomic<SpectrumAnalysisReadiness> analysisReadiness { SpectrumAnalysisReadiness::stopped };
    std::atomic<bool> configurationPending { false };
    std::atomic<bool> acquisitionRunning { false };
    std::atomic<std::size_t> warmupSampleCount { 0 };
    std::atomic<std::size_t> warmupTargetSampleCount { 0 };
    std::atomic<std::uint64_t> droppedInputBlocks { 0 };
    std::atomic<std::uint64_t> droppedInputSamples { 0 };
    std::atomic<std::uint64_t> rejectedInputBlocks { 0 };
    std::atomic<std::uint64_t> inputDiscontinuities { 0 };
    std::atomic<std::uint64_t> failedSpectrumWindows { 0 };
    std::atomic<std::uint64_t> unconfiguredInputBlocks { 0 };
    std::atomic<std::uint64_t> unconfiguredInputSamples { 0 };
    std::atomic<std::uint64_t> staleConfigurationBlocks { 0 };
    std::atomic<std::uint64_t> configurationFailures { 0 };

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
