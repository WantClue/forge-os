#ifndef MINING_H_
#define MINING_H_

#include <stdbool.h>
#include <stdint.h>
#include "stratum_api.h"

typedef struct
{
    uint32_t version;
    uint32_t version_mask;
    uint8_t prev_block_hash[32];
    uint8_t prev_block_hash_be[32];
    uint8_t merkle_root[32];
    uint8_t merkle_root_be[32];
    uint32_t ntime;
    uint32_t target; // aka difficulty, aka nbits
    uint32_t starting_nonce;

    uint8_t num_midstates;
    uint8_t midstate[32];
    uint8_t midstate1[32];
    uint8_t midstate2[32];
    uint8_t midstate3[32];
    double pool_diff;
    int pool_id;
    stratum_protocol_t protocol;
    uint32_t asic_diff;
    char *jobid;
    char *extranonce2;
} bm_job;

void free_bm_job(bm_job *job);

char *construct_coinbase_tx(const char *coinbase_1, const char *coinbase_2,
                            const char *extranonce, const char *extranonce_2);

char *calculate_merkle_root_hash(const char *coinbase_tx, const uint8_t merkle_branches[][32], const int num_merkle_branches);

bm_job construct_bm_job(mining_notify *params, const char *merkle_root, const uint32_t version_mask);

double test_nonce_value(const bm_job *job, const uint32_t nonce, const uint32_t rolled_version);

// Same as test_nonce_value, but also writes the block header's double-SHA256
// into hash_out (raw digest, i.e. Bitcoin's internal little-endian byte order,
// the same order a following header uses for its prev_block_hash).
double test_nonce_hash(const bm_job *job, const uint32_t nonce, const uint32_t rolled_version, uint8_t hash_out[32]);

// Expand compact nBits into a 256-bit little-endian target. Returns false for
// encodings Bitcoin treats as invalid (negative, zero or overflowing).
bool nbits_to_target(uint32_t nbits, uint8_t target_out[32]);

// True when hash <= target, both 256-bit little-endian. This is the exact
// proof-of-work check a node applies to a block header.
bool hash_meets_target(const uint8_t hash[32], const uint8_t target[32]);

char *extranonce_2_generate(uint64_t extranonce_2, uint32_t length);

uint32_t increment_bitmask(const uint32_t value, const uint32_t mask);

#endif /* MINING_H_ */
