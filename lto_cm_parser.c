// SPDX-License-Identifier: GPL-3.0-or-later

#include "lto_cm_parser.h"

#include <stdio.h>
#include <string.h>

#define LTO_DIRECTORY_SCAN_BYTES (256U)
#define LTO_CLEANING_BIT         (0x8000U)
#define LTO_WORM_BIT             (0x2000U)

static uint16_t lto_be16(const uint8_t* data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t lto_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t lto_be64(const uint8_t* data) {
    uint64_t value = 0;
    for(size_t i = 0; i < 8; i++) {
        value = (value << 8) | data[i];
    }
    return value;
}

static bool lto_range_is_read(const LtoCmParser* parser, size_t address, size_t length) {
    if(!parser || !parser->data || (length == 0U) || (address >= parser->size) ||
       (length > (parser->size - address))) {
        return false;
    }

    const size_t first_block = address / LTO_CM_BLOCK_SIZE;
    const size_t last_block = (address + length - 1U) / LTO_CM_BLOCK_SIZE;
    if(last_block >= parser->block_count) return false;
    if(!parser->blocks_read) return true;
    for(size_t block = first_block; block <= last_block; block++) {
        if(parser->blocks_read[block] == 0U) return false;
    }
    return true;
}

bool lto_cm_validate_page(
    const LtoCmParser* parser,
    uint16_t page_id,
    uint16_t address,
    size_t required_length) {
    if(!lto_range_is_read(parser, address, 4U)) return false;
    const uint16_t header_id = lto_be16(&parser->data[address]) & 0x0FFFU;
    const uint16_t declared_length = lto_be16(&parser->data[address + 2U]);
    return (header_id == page_id) && (declared_length >= required_length) &&
           lto_range_is_read(parser, address, required_length);
}

bool lto_cm_find_page(const LtoCmParser* parser, uint16_t page_id, uint16_t* address) {
    if(!parser || !address) return false;
    const uint8_t* cm = parser->data;
    uint16_t unprotected_address = 0;
    bool protected_eopt_found = false;

    const size_t protected_end = parser->size < 128U ? parser->size : 128U;
    for(size_t offset = 0x24U; (offset + 4U) <= protected_end; offset += 4U) {
        if(!lto_range_is_read(parser, offset, 4U)) return false;
        const uint8_t version = cm[offset] >> 4;
        const uint16_t id = ((uint16_t)(cm[offset] & 0x0FU) << 8) | cm[offset + 1U];
        const uint16_t start = lto_be16(&cm[offset + 2U]);
        if(id == page_id) {
            if(start >= parser->size) return false;
            *address = start;
            return true;
        }
        if((version == 0U) && (id == 0x0FFFU)) {
            unprotected_address = start;
            protected_eopt_found = true;
            break;
        }
    }

    if(!protected_eopt_found || (unprotected_address >= parser->size)) return false;
    size_t unprotected_end = unprotected_address + LTO_DIRECTORY_SCAN_BYTES;
    if((unprotected_end < unprotected_address) || (unprotected_end > parser->size)) {
        unprotected_end = parser->size;
    }
    for(size_t offset = unprotected_address; (offset + 4U) <= unprotected_end; offset += 4U) {
        if(!lto_range_is_read(parser, offset, 4U)) return false;
        const uint8_t version = cm[offset] >> 4;
        const uint16_t id = ((uint16_t)(cm[offset] & 0x0FU) << 8) | cm[offset + 1U];
        const uint16_t start = lto_be16(&cm[offset + 2U]);
        if(id == page_id) {
            if(start >= parser->size) return false;
            *address = start;
            return true;
        }
        if((version == 0U) && (id == 0x0FFFU)) break;
    }
    return false;
}

static bool lto_set_generation_parameters(LtoMediaInfo* info, uint8_t type_code) {
    switch(type_code) {
    case 0x01U:
        info->generation = 1;
        info->kb_per_dataset = 404;
        info->sets_per_wrap = 5500;
        info->wrap_count = 48;
        break;
    case 0x02U:
        info->generation = 2;
        info->kb_per_dataset = 404;
        info->sets_per_wrap = 8200;
        info->wrap_count = 64;
        break;
    case 0x04U:
        info->generation = 3;
        info->kb_per_dataset = 1617;
        info->sets_per_wrap = 6000;
        info->wrap_count = 44;
        break;
    case 0x08U:
        info->generation = 4;
        info->kb_per_dataset = 1590;
        info->sets_per_wrap = 9500;
        info->wrap_count = 56;
        break;
    case 0x10U:
        info->generation = 5;
        info->kb_per_dataset = 2473;
        info->sets_per_wrap = 7800;
        info->wrap_count = 80;
        break;
    case 0x20U:
        info->generation = 6;
        info->kb_per_dataset = 2473;
        info->sets_per_wrap = 7805;
        info->wrap_count = 136;
        break;
    case 0x40U:
        info->generation = 7;
        info->kb_per_dataset = 5032;
        info->sets_per_wrap = 10950;
        info->wrap_count = 112;
        break;
    case 0x80U:
        info->generation = 8;
        info->kb_per_dataset = 5032;
        info->sets_per_wrap = 11660;
        info->wrap_count = 208;
        break;
    case 0x81U:
        info->generation = 9;
        info->kb_per_dataset = 9806;
        info->sets_per_wrap = 6770;
        info->wrap_count = 280;
        break;
    default:
        return false;
    }
    return true;
}

bool lto_cm_identify_media(const LtoCmParser* parser, LtoMediaInfo* info) {
    if(!parser || !info) return false;
    uint16_t manufacturer_address;
    if(!lto_cm_find_page(parser, 0x001U, &manufacturer_address) ||
       !lto_cm_validate_page(parser, 0x001U, manufacturer_address, 64U)) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->cartridge_type = lto_be16(&parser->data[manufacturer_address + 22U]);
    info->worm = (info->cartridge_type & LTO_WORM_BIT) != 0U;
    if((info->cartridge_type & LTO_CLEANING_BIT) != 0U) {
        info->media_kind = LtoMediaCleaning;
        return true;
    }

    info->media_kind = LtoMediaData;
    return lto_set_generation_parameters(info, (uint8_t)(info->cartridge_type & 0xFFU));
}

static void lto_parse_date(char output[11], const uint8_t* date) {
    bool valid = true;
    for(size_t i = 0; i < 8U; i++) {
        if((date[i] < '0') || (date[i] > '9')) {
            valid = false;
            break;
        }
    }
    if(valid) {
        snprintf(
            output,
            11,
            "%c%c%c%c-%c%c-%c%c",
            date[0],
            date[1],
            date[2],
            date[3],
            date[4],
            date[5],
            date[6],
            date[7]);
    } else {
        snprintf(output, 11, "Unknown");
    }
}

bool lto_cm_parse_health(
    const LtoCmParser* parser,
    LtoMediaInfo* info,
    LtoHealth* health) {
    if(!parser || !info || !health || (info->media_kind != LtoMediaData)) return false;
    uint16_t manufacturer_address;
    uint16_t status_address;
    uint16_t usage_addresses[4];
    if(!lto_cm_find_page(parser, 0x001U, &manufacturer_address) ||
       !lto_cm_find_page(parser, 0x105U, &status_address)) {
        return false;
    }
    for(size_t i = 0; i < 4; i++) {
        const uint16_t usage_page_id = (uint16_t)(0x108U + i);
        if(!lto_cm_find_page(parser, usage_page_id, &usage_addresses[i])) return false;
    }

    const bool enhanced = info->generation >= 5U;
    const size_t usage_length = enhanced ? 96U : 64U;
    if(!lto_cm_validate_page(parser, 0x001U, manufacturer_address, 64U) ||
       !lto_cm_validate_page(parser, 0x105U, status_address, 64U)) {
        return false;
    }
    for(size_t i = 0; i < 4; i++) {
        const uint16_t usage_page_id = (uint16_t)(0x108U + i);
        if(!lto_cm_validate_page(parser, usage_page_id, usage_addresses[i], usage_length)) {
            return false;
        }
    }

    const size_t thread_offset = enhanced ? 32U : 24U;
    size_t newest = 0;
    uint32_t newest_thread_count = 0;
    for(size_t i = 0; i < 4; i++) {
        const uint32_t thread_count =
            lto_be32(&parser->data[usage_addresses[i] + thread_offset]);
        if(thread_count >= newest_thread_count) {
            newest_thread_count = thread_count;
            newest = i;
        }
    }

    const uint8_t* usage = &parser->data[usage_addresses[newest]];
    const size_t sets_written_offset = enhanced ? 36U : 28U;
    const size_t sets_read_offset = enhanced ? 44U : 36U;
    const size_t write_retries_offset = enhanced ? 52U : 44U;
    const size_t read_retries_offset = enhanced ? 56U : 48U;
    const size_t unrecovered_writes_offset = enhanced ? 60U : 52U;
    const size_t unrecovered_reads_offset = enhanced ? 62U : 54U;
    const size_t suspended_writes_offset = enhanced ? 64U : 56U;
    const size_t fatal_writes_offset = enhanced ? 66U : 58U;
    const uint64_t sets_written = lto_be64(&usage[sets_written_offset]);
    const uint64_t sets_read = lto_be64(&usage[sets_read_offset]);

    memset(health, 0, sizeof(*health));
    health->written_tb =
        ((double)sets_written * (double)info->kb_per_dataset) / (double)1000000000ULL;
    health->read_tb =
        ((double)sets_read * (double)info->kb_per_dataset) / (double)1000000000ULL;
    health->fve = ((double)(sets_written + sets_read)) /
                  (double)(info->wrap_count * info->sets_per_wrap);
    health->load_count = lto_be32(&parser->data[status_address + 12U]);
    health->write_retries = lto_be32(&usage[write_retries_offset]);
    health->read_retries = lto_be32(&usage[read_retries_offset]);
    health->unrecovered_writes = lto_be16(&usage[unrecovered_writes_offset]);
    health->unrecovered_reads = lto_be16(&usage[unrecovered_reads_offset]);
    health->suspended_writes = lto_be16(&usage[suspended_writes_offset]);
    health->fatal_suspended_writes = lto_be16(&usage[fatal_writes_offset]);
    health->enhanced_metrics = enhanced;
    if(enhanced) {
        health->lp3_passes = lto_be32(&usage[68]);
        health->midpoint_passes = lto_be32(&usage[72]);
        health->max_tape_temp = usage[76];
    }

    if(info->generation == 7U) {
        uint16_t content_address;
        if(lto_cm_find_page(parser, 0x10DU, &content_address) &&
           lto_cm_validate_page(parser, 0x10DU, content_address, 64U)) {
            info->type_m = (parser->data[content_address + 28U] & 1U) != 0U;
            if(info->type_m) {
                info->wrap_count = 168U;
                health->fve = ((double)(sets_written + sets_read)) /
                              (double)(info->wrap_count * info->sets_per_wrap);
            }
        }
    }
    lto_parse_date(health->manufacture_date, &parser->data[manufacturer_address + 24U]);
    return true;
}

bool lto_cm_parse_cleaning(const LtoCmParser* parser, LtoCleaning* cleaning) {
    if(!parser || !cleaning) return false;
    uint16_t manufacturer_address;
    uint16_t status_address;
    if(!lto_cm_find_page(parser, 0x001U, &manufacturer_address) ||
       !lto_cm_find_page(parser, 0x105U, &status_address) ||
       !lto_cm_validate_page(parser, 0x001U, manufacturer_address, 64U) ||
       !lto_cm_validate_page(parser, 0x105U, status_address, 28U)) {
        return false;
    }

    memset(cleaning, 0, sizeof(*cleaning));
    cleaning->cleans_performed = lto_be32(&parser->data[status_address + 12U]);
    cleaning->last_location_quarter_m = lto_be16(&parser->data[status_address + 26U]);
    cleaning->tape_length_quarter_m = lto_be16(&parser->data[manufacturer_address + 32U]);
    lto_parse_date(
        cleaning->manufacture_date, &parser->data[manufacturer_address + 24U]);

    const int32_t remaining_quarter_m =
        (int32_t)cleaning->tape_length_quarter_m - 44 -
        (int32_t)cleaning->last_location_quarter_m;
    cleaning->expired = remaining_quarter_m <= 0;
    cleaning->estimated_remaining =
        remaining_quarter_m > 0 ? (uint16_t)(remaining_quarter_m / 22) : 0U;
    return true;
}
