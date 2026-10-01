/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef BACKLOG_SHEDDING_POLICY_H_INCLUDED
#define BACKLOG_SHEDDING_POLICY_H_INCLUDED

namespace spectrumviewer
{
/**
    Selects work after each input block in a latency-bounded consumer.

    The policy is independent of the queue and the work being scheduled. Once
    another input block is observed, completed work is discarded until the
    consumer reaches the last queued block; only its newest completed item is
    processed. The next block after catch-up resumes normal processing.
*/
class BacklogSheddingPolicy
{
public:
    enum class Action
    {
        processAll,
        discardAll,
        processLatest
    };

    Action inputBlockDequeued (bool moreInputQueued) noexcept
    {
        shedding = shedding || moreInputQueued;
        if (! shedding)
            return Action::processAll;
        if (moreInputQueued)
            return Action::discardAll;

        shedding = false;
        return Action::processLatest;
    }

    void reset() noexcept { shedding = false; }

private:
    bool shedding = false;
};
} // namespace spectrumviewer

#endif // BACKLOG_SHEDDING_POLICY_H_INCLUDED
