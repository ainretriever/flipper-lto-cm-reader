// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lto_cm_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if(!(condition)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
            failures++;                                                                   \
        }                                                                                 \
    } while(0)

static bool close_to(double actual, double expected, double tolerance) {
    const double difference = actual > expected ? actual - expected : expected - actual;
    return difference <= tolerance;
}

static uint8_t* load_fixture(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if(!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    rewind(file);
    if(length <= 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* data = malloc((size_t)length);
    if(!data || (fread(data, 1, (size_t)length, file) != (size_t)length)) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void test_lto5_health(void) {
    size_t size = 0;
    uint8_t* data = load_fixture("tests/fixtures/lto5_health.bin", &size);
    CHECK(data != NULL);
    if(!data) return;

    LtoCmParser parser = {
        .data = data,
        .size = size,
        .blocks_read = NULL,
        .block_count = size / LTO_CM_BLOCK_SIZE,
    };
    LtoMediaInfo info = {0};
    LtoHealth health;
    CHECK(lto_cm_identify_media(&parser, &info));
    CHECK(info.media_kind == LtoMediaData);
    CHECK(info.generation == 5U);
    CHECK(info.cartridge_type == 0x0010U);
    CHECK(!info.worm);
    CHECK(lto_cm_parse_health(&parser, &info, &health));
    CHECK(strcmp(health.manufacture_date, "2024-01-02") == 0);
    CHECK(health.load_count == 42U);
    CHECK(!health.uninitialized);
    CHECK(close_to(health.written_tb, 0.002473, 0.000000001));
    CHECK(close_to(health.read_tb, 0.004946, 0.000000001));
    CHECK(close_to(health.fve, 3000.0 / (80.0 * 7800.0), 0.000000001));
    CHECK(health.write_retries == 11U);
    CHECK(health.read_retries == 12U);
    CHECK(health.unrecovered_writes == 13U);
    CHECK(health.unrecovered_reads == 14U);
    CHECK(health.suspended_writes == 15U);
    CHECK(health.fatal_suspended_writes == 16U);
    CHECK(health.lp3_passes == 17U);
    CHECK(health.midpoint_passes == 18U);
    CHECK(health.max_tape_temp == 42U);

    uint8_t read_map[128];
    memset(read_map, 1, sizeof(read_map));
    read_map[0x2A0U / LTO_CM_BLOCK_SIZE] = 0;
    parser.blocks_read = read_map;
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    free(data);
}

static void test_cleaning_32byte_status(void) {
    size_t size = 0;
    uint8_t* data = load_fixture("tests/fixtures/cleaning_32byte.bin", &size);
    CHECK(data != NULL);
    if(!data) return;

    const LtoCmParser parser = {
        .data = data,
        .size = size,
        .blocks_read = NULL,
        .block_count = size / LTO_CM_BLOCK_SIZE,
    };
    LtoMediaInfo info = {0};
    LtoCleaning cleaning;
    CHECK(lto_cm_identify_media(&parser, &info));
    CHECK(info.media_kind == LtoMediaCleaning);
    CHECK(info.cartridge_type == 0x8001U);
    CHECK(lto_cm_parse_cleaning(&parser, &cleaning));
    CHECK(strcmp(cleaning.manufacture_date, "2024-02-03") == 0);
    CHECK(cleaning.cleans_performed == 10U);
    CHECK(cleaning.tape_length_quarter_m == 1200U);
    CHECK(cleaning.last_location_quarter_m == 220U);
    CHECK(cleaning.estimated_remaining == 42U);
    CHECK(!cleaning.expired);
    free(data);
}

static void test_uninitialized(const char* path) {
    size_t size = 0;
    uint8_t* data = load_fixture(path, &size);
    CHECK(data != NULL);
    if(!data) return;
    uint8_t read_map[255];
    memset(read_map, 1, sizeof(read_map));
    LtoCmParser parser = {data, size, read_map, size / LTO_CM_BLOCK_SIZE};
    LtoMediaInfo info = {0};
    LtoHealth health;
    CHECK(lto_cm_identify_media(&parser, &info));
    CHECK(info.generation == 5U);
    CHECK(lto_cm_has_factory_layout(&parser));
    CHECK(lto_cm_parse_health(&parser, &info, &health));
    CHECK(health.uninitialized);
    CHECK(health.load_count == 0U);
    CHECK(!health.enhanced_metrics);
    uint16_t status = 0;
    CHECK(lto_cm_find_page(&parser, 0x105U, &status));
    /* Reproduce the targeted health read: bootstrap, directory, status only. */
    memset(read_map, 0, sizeof(read_map));
    for(size_t i = 0; i < 4U; i++) read_map[i] = 1;
    for(size_t i = 8U; i < 16U; i++) read_map[i] = 1;
    read_map[status / 32U] = 1;
    read_map[status / 32U + 1U] = 1;
    CHECK(lto_cm_parse_health(&parser, &info, &health));
    data[status + 15U] = 1;
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    data[status + 15U] = 0;
    read_map[status / 32U] = 0;
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    read_map[status / 32U] = 1;
    read_map[0x140U / 32U] = 0;
    CHECK(!lto_cm_has_factory_layout(&parser));
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    read_map[0x140U / 32U] = 1;
    data[0x110] = 0x11;
    data[0x111] = 0x08;
    CHECK(!lto_cm_has_factory_layout(&parser));
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    data[0x111] = 0x01;
    CHECK(!lto_cm_has_factory_layout(&parser));
    CHECK(!lto_cm_parse_health(&parser, &info, &health));
    free(data);
}

int main(int argc, char** argv) {
    test_uninitialized("tests/fixtures/lto5_uninitialized.bin");
    if(argc > 1) test_uninitialized(argv[1]);
    test_lto5_health();
    test_cleaning_32byte_status();
    if(failures != 0) {
        fprintf(stderr, "%d parser regression test(s) failed\n", failures);
        return 1;
    }
    puts("All parser regression tests passed");
    return 0;
}
