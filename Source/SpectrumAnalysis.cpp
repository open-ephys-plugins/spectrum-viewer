/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "SpectrumAnalysis.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace spectrumviewer
{
namespace
{
    const SpectrumAnalysisConfiguration& requireConfiguration (
        const std::shared_ptr<const SpectrumAnalysisConfiguration>& configuration)
    {
        if (configuration == nullptr)
            throw std::invalid_argument ("Spectrum analysis configuration is null");
        return *configuration;
    }

    void validateParameters (const SpectrumAnalysisParameters& parameters)
    {
        const auto validDetrendMode = parameters.detrendMode == DetrendMode::none
                                      || parameters.detrendMode == DetrendMode::mean
                                      || parameters.detrendMode == DetrendMode::linear;
        if (parameters.channelCount == 0 || parameters.windowSampleCount == 0
            || parameters.hopSampleCount == 0
            || parameters.maximumInputBlockSampleCount == 0
            || parameters.taperCount == 0
            || ! std::isfinite (parameters.sampleRateHz)
            || parameters.sampleRateHz <= 0.0
            || ! std::isfinite (parameters.timeHalfBandwidth)
            || parameters.timeHalfBandwidth <= 0.0
            || parameters.timeHalfBandwidth >= 0.5 * static_cast<double> (parameters.windowSampleCount)
            || ! validDetrendMode)
            throw std::invalid_argument ("Spectrum analysis configuration is invalid");
    }
} // namespace

SpectrumAnalysisConfiguration::SpectrumAnalysisConfiguration (
    SpectrumAnalysisParameters parameters)
    : values (parameters)
{
    validateParameters (values);
    auto generated = generateDpssTapers (values.windowSampleCount,
                                         values.timeHalfBandwidth,
                                         values.taperCount,
                                         values.dpssPlanningFlags);
    if (! generated.succeeded())
        throw std::runtime_error (getDpssGenerationStatusDescription (generated.status));

    taperBank = std::make_shared<const DpssTaperBank> (std::move (generated));
    descriptor.sampleRateHz = values.sampleRateHz;
    descriptor.binWidthHz = values.sampleRateHz / static_cast<double> (values.windowSampleCount);
    descriptor.timeHalfBandwidth = values.timeHalfBandwidth;
    descriptor.windowSampleCount = values.windowSampleCount;
    descriptor.hopSampleCount = values.hopSampleCount;
    descriptor.taperCount = values.taperCount;
    descriptor.configurationGeneration = values.generation;
    descriptor.detrendMode = values.detrendMode;
}

SpectrumAnalysisPipeline::SpectrumAnalysisPipeline (
    std::shared_ptr<const SpectrumAnalysisConfiguration> newConfiguration)
    : configuration (std::move (newConfiguration)),
      assembler (requireConfiguration (configuration).getParameters().channelCount,
                 requireConfiguration (configuration).getParameters().windowSampleCount,
                 requireConfiguration (configuration).getParameters().hopSampleCount,
                 requireConfiguration (configuration).getParameters().maximumInputBlockSampleCount),
      estimator (requireConfiguration (configuration).getParameters().channelCount,
                 requireConfiguration (configuration).getParameters().sampleRateHz,
                 requireConfiguration (configuration).getTaperBank(),
                 requireConfiguration (configuration).getParameters().detrendMode,
                 requireConfiguration (configuration).getParameters().estimatorPlanningFlags),
      channelViews (requireConfiguration (configuration).getParameters().channelCount)
{
}

SpectrumAnalysisPipeline::AppendResult SpectrumAnalysisPipeline::appendBlock (
    const float* const* source,
    std::size_t numChannels,
    std::size_t numSamples,
    std::int64_t firstSample,
    std::uint64_t configurationGeneration)
{
    if (configurationGeneration != configuration->getParameters().generation)
        return { 0, false, AppendStatus::configurationMismatch };

    const auto result = assembler.appendBlock (
        source, numChannels, numSamples, firstSample);
    return { result.windowsReady,
             result.discontinuity,
             result.accepted ? AppendStatus::accepted : AppendStatus::invalidBlock };
}
} // namespace spectrumviewer
