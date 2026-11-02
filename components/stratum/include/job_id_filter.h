#ifndef JOB_ID_FILTER_H_
#define JOB_ID_FILTER_H_

#include <stdbool.h>

// Remembers the job ids a pool has handed out in the current session, so a
// re-sent notify for a job we are already mining is dropped. Mining it again
// would rebuild the exact same work from extranonce2 = 0 and produce shares
// the pool already has (duplicates).

#define JOB_ID_FILTER_SIZE   16
#define JOB_ID_FILTER_ID_LEN 64

typedef struct {
    char ids[JOB_ID_FILTER_SIZE][JOB_ID_FILTER_ID_LEN];
    int count;
    int next;
} job_id_filter_t;

void job_id_filter_reset(job_id_filter_t *filter);

// Returns false when job_id was already seen since the last clean_jobs notify
// (or reset), true when the job is new and should be mined. clean_jobs = true
// starts a new block, which forgets every id seen before. Empty ids and ids
// too long to store are always accepted.
bool job_id_filter_accept(job_id_filter_t *filter, const char *job_id, bool clean_jobs);

#endif /* JOB_ID_FILTER_H_ */
