/* Tests for the reusable backlog scheduling policy. */

#include "gtest/gtest.h"

#include "BacklogSheddingPolicy.h"

namespace
{
using Action = spectrumviewer::BacklogSheddingPolicy::Action;
using spectrumviewer::BacklogSheddingPolicy;

TEST (BacklogSheddingPolicyTests, ProcessesAllWorkWithoutAQueueBacklog)
{
    BacklogSheddingPolicy policy;
    EXPECT_EQ (policy.inputBlockDequeued (false), Action::processAll);
    EXPECT_EQ (policy.inputBlockDequeued (false), Action::processAll);
}

TEST (BacklogSheddingPolicyTests, DiscardsIntermediateWorkAndProcessesLatestAtCatchUp)
{
    BacklogSheddingPolicy policy;
    EXPECT_EQ (policy.inputBlockDequeued (true), Action::discardAll);
    EXPECT_EQ (policy.inputBlockDequeued (true), Action::discardAll);
    EXPECT_EQ (policy.inputBlockDequeued (false), Action::processLatest);
    EXPECT_EQ (policy.inputBlockDequeued (false), Action::processAll);
}

TEST (BacklogSheddingPolicyTests, ResetEndsAnIncompleteCatchUpCycle)
{
    BacklogSheddingPolicy policy;
    EXPECT_EQ (policy.inputBlockDequeued (true), Action::discardAll);
    policy.reset();
    EXPECT_EQ (policy.inputBlockDequeued (false), Action::processAll);
}
} // namespace
