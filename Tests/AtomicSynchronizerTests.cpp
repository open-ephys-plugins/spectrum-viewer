#include <gtest/gtest.h>

#include "AtomicSynchronizer.h"

TEST (AtomicSynchronizerTests, StartsWithoutPublishedData)
{
    AtomicallyShared<int> shared (0);

    EXPECT_FALSE (shared.hasUpdate());

    AtomicScopedReadPtr<int> reader (shared);
    EXPECT_FALSE (reader.isValid());
}

TEST (AtomicSynchronizerTests, PublishesValueToReader)
{
    AtomicallyShared<int> shared (0);
    AtomicScopedWritePtr<int> writer (shared);

    ASSERT_TRUE (writer.isValid());
    *writer = 42;
    writer.pushUpdate();

    EXPECT_TRUE (shared.hasUpdate());

    AtomicScopedReadPtr<int> reader (shared);
    ASSERT_TRUE (reader.isValid());
    EXPECT_EQ (*reader, 42);
    EXPECT_FALSE (shared.hasUpdate());
}

TEST (AtomicSynchronizerTests, ReaderSnapshotStaysStableUntilPulled)
{
    AtomicallyShared<int> shared (0);
    AtomicScopedWritePtr<int> writer (shared);

    *writer = 1;
    writer.pushUpdate();

    AtomicScopedReadPtr<int> reader (shared);
    ASSERT_TRUE (reader.isValid());
    EXPECT_EQ (*reader, 1);

    *writer = 2;
    writer.pushUpdate();
    *writer = 3;
    writer.pushUpdate();

    EXPECT_EQ (*reader, 1);
    EXPECT_TRUE (shared.hasUpdate());

    reader.pullUpdate();
    ASSERT_TRUE (reader.isValid());
    EXPECT_EQ (*reader, 3);
    EXPECT_FALSE (shared.hasUpdate());
}

TEST (AtomicSynchronizerTests, ReaderReceivesNewestUnconsumedValue)
{
    AtomicallyShared<int> shared (0);
    AtomicScopedWritePtr<int> writer (shared);

    for (int value = 1; value <= 10; ++value)
    {
        *writer = value;
        writer.pushUpdate();
    }

    AtomicScopedReadPtr<int> reader (shared);
    ASSERT_TRUE (reader.isValid());
    EXPECT_EQ (*reader, 10);
}

TEST (AtomicSynchronizerTests, RejectsASecondWriter)
{
    AtomicallyShared<int> shared (0);
    AtomicScopedWritePtr<int> first (shared);
    AtomicScopedWritePtr<int> second (shared);

    EXPECT_TRUE (first.isValid());
    EXPECT_FALSE (second.isValid());
}

TEST (AtomicSynchronizerTests, RejectsASecondReader)
{
    AtomicallyShared<int> shared (0);

    {
        AtomicScopedWritePtr<int> writer (shared);
        *writer = 7;
        writer.pushUpdate();
    }

    AtomicScopedReadPtr<int> first (shared);
    AtomicScopedReadPtr<int> second (shared);

    EXPECT_TRUE (first.isValid());
    EXPECT_FALSE (second.isValid());
}

TEST (AtomicSynchronizerTests, MapRequiresExclusiveAccess)
{
    AtomicallyShared<int> shared (0);

    {
        AtomicScopedWritePtr<int> writer (shared);
        EXPECT_FALSE (shared.map ([] (int& value) { value = 9; }));
    }

    ASSERT_TRUE (shared.map ([] (int& value) { value = 9; }));

    AtomicScopedWritePtr<int> writer (shared);
    ASSERT_TRUE (writer.isValid());
    EXPECT_EQ (*writer, 9);
}

TEST (AtomicSynchronizerTests, ResetDiscardsPendingValue)
{
    AtomicallyShared<int> shared (0);

    {
        AtomicScopedWritePtr<int> writer (shared);
        *writer = 5;
        writer.pushUpdate();
    }

    ASSERT_TRUE (shared.hasUpdate());
    ASSERT_TRUE (shared.reset());
    EXPECT_FALSE (shared.hasUpdate());

    AtomicScopedReadPtr<int> reader (shared);
    EXPECT_FALSE (reader.isValid());
}
