#include "unity.h"
#include "job_id_filter.h"
#include <stdio.h>
#include <string.h>

TEST_CASE("Job id filter drops a repeated job id", "[job_id_filter]")
{
    job_id_filter_t filter;
    job_id_filter_reset(&filter);

    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a1", false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a2", false));
    TEST_ASSERT_FALSE(job_id_filter_accept(&filter, "a1", false));
    TEST_ASSERT_FALSE(job_id_filter_accept(&filter, "a2", false));
}

TEST_CASE("Job id filter forgets ids on clean_jobs", "[job_id_filter]")
{
    job_id_filter_t filter;
    job_id_filter_reset(&filter);

    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a1", false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a1", true));
    TEST_ASSERT_FALSE(job_id_filter_accept(&filter, "a1", false));
}

TEST_CASE("Job id filter forgets ids on reset", "[job_id_filter]")
{
    job_id_filter_t filter;
    job_id_filter_reset(&filter);

    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a1", false));
    job_id_filter_reset(&filter);
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "a1", false));
}

TEST_CASE("Job id filter evicts the oldest id when full", "[job_id_filter]")
{
    job_id_filter_t filter;
    job_id_filter_reset(&filter);
    char id[8];

    for (int i = 0; i < JOB_ID_FILTER_SIZE + 1; i++) {
        snprintf(id, sizeof(id), "j%d", i);
        TEST_ASSERT_TRUE(job_id_filter_accept(&filter, id, false));
    }
    // j0 was evicted by j16, j1 is still tracked.
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "j0", false));
    TEST_ASSERT_FALSE(job_id_filter_accept(&filter, "j2", false));
}

TEST_CASE("Job id filter accepts empty and oversized ids", "[job_id_filter]")
{
    job_id_filter_t filter;
    job_id_filter_reset(&filter);
    char long_id[JOB_ID_FILTER_ID_LEN + 8];
    memset(long_id, 'x', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';

    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "", false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, "", false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, NULL, false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, long_id, false));
    TEST_ASSERT_TRUE(job_id_filter_accept(&filter, long_id, false));
}
