#ifndef __NBA_DATABLOCK_SHARED_HH__
#define __NBA_DATABLOCK_SHARED_HH__

/*
 * This header is included by both .cc/.cu sources.
 * Note that the nvcc should support C++11 (CUDA v6.5 or higher).
 */

#include <cstdint>
#include <nba/core/shiftedint.hh>

struct alignas(8) datablock_batch_info {
    void *buffer_bases_in;
    void *buffer_bases_out;
    uint32_t item_count_in;
    uint32_t item_count_out;
    uint16_t *item_sizes_in;
    uint16_t *item_sizes_out;
    nba::dev_offset_t *item_offsets_in;
    nba::dev_offset_t *item_offsets_out;
};

struct alignas(8) datablock_kernel_arg {
    uint32_t total_item_count_in;
    uint32_t total_item_count_out;
    uint16_t item_size_in;  // for fixed-size cases
    uint16_t item_size_out; // for fixed-size cases
    struct datablock_batch_info batches[0];
};

#endif
