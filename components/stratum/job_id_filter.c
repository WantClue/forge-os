#include "job_id_filter.h"

#include <string.h>

void job_id_filter_reset(job_id_filter_t *filter)
{
    filter->count = 0;
    filter->next = 0;
}

bool job_id_filter_accept(job_id_filter_t *filter, const char *job_id, bool clean_jobs)
{
    if (clean_jobs) {
        job_id_filter_reset(filter);
    }

    if (job_id == NULL || job_id[0] == '\0') {
        return true;
    }

    // A truncated copy could falsely match a different long id, so don't
    // track ids that don't fit.
    size_t len = strnlen(job_id, JOB_ID_FILTER_ID_LEN);
    if (len >= JOB_ID_FILTER_ID_LEN) {
        return true;
    }

    for (int i = 0; i < filter->count; i++) {
        if (strcmp(filter->ids[i], job_id) == 0) {
            return false;
        }
    }

    // Ring buffer: once full, the oldest id is overwritten.
    memcpy(filter->ids[filter->next], job_id, len + 1);
    filter->next = (filter->next + 1) % JOB_ID_FILTER_SIZE;
    if (filter->count < JOB_ID_FILTER_SIZE) {
        filter->count++;
    }
    return true;
}
