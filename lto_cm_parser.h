// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LTO_CM_BLOCK_SIZE (32U)

typedef enum {
    LtoMediaUnknown,
    LtoMediaData,
    LtoMediaCleaning,
} LtoMediaKind;

typedef struct {
    double written_tb;
    double read_tb;
    double fve;
    uint32_t load_count;
    uint32_t write_retries;
    uint32_t read_retries;
    uint16_t unrecovered_writes;
    uint16_t unrecovered_reads;
    uint16_t suspended_writes;
    uint16_t fatal_suspended_writes;
    uint32_t lp3_passes;
    uint32_t midpoint_passes;
    uint8_t max_tape_temp;
    bool enhanced_metrics;
    char manufacture_date[11];
} LtoHealth;

typedef struct {
    uint32_t cleans_performed;
    uint16_t tape_length_quarter_m;
    uint16_t last_location_quarter_m;
    uint16_t estimated_remaining;
    bool expired;
    char manufacture_date[11];
} LtoCleaning;

typedef struct {
    LtoMediaKind media_kind;
    uint16_t cartridge_type;
    uint16_t generation;
    uint32_t kb_per_dataset;
    uint32_t sets_per_wrap;
    uint16_t wrap_count;
    bool worm;
    bool type_m;
} LtoMediaInfo;

typedef struct {
    const uint8_t* data;
    size_t size;
    const uint8_t* blocks_read;
    size_t block_count;
} LtoCmParser;

bool lto_cm_find_page(const LtoCmParser* parser, uint16_t page_id, uint16_t* address);
bool lto_cm_validate_page(
    const LtoCmParser* parser,
    uint16_t page_id,
    uint16_t address,
    size_t required_length);
bool lto_cm_identify_media(const LtoCmParser* parser, LtoMediaInfo* info);
bool lto_cm_parse_health(
    const LtoCmParser* parser,
    LtoMediaInfo* info,
    LtoHealth* health);
bool lto_cm_parse_cleaning(const LtoCmParser* parser, LtoCleaning* cleaning);
