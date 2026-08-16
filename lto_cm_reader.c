// SPDX-License-Identifier: GPL-3.0-or-later

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <nfc/helpers/iso14443_crc.h>
#include <nfc/nfc.h>
#include <storage/storage.h>
#include <toolbox/bit_buffer.h>

#include "lto_cm_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "LtoCmReader"

#define LTO_REQUEST_STANDARD      (0x45U)
#define LTO_REQUEST_STANDARD_BITS (7U)
#define LTO_RESPONSE_CAPACITY     (32U)
#define LTO_MAX_BLOCK_COUNT       (1023U)
#define LTO_MAX_CM_SIZE           (32736U)
#define LTO_BLOCK_SIZE            (32U)
#define LTO_HALF_BLOCK_SIZE       (16U)
#define LTO_READ_RESPONSE_SIZE    (18U)
#define LTO_FWT_FC                (60000U)
#define LTO_GUARD_TIME_US         (5000U)
#define LTO_FDT_POLL_FC           (1620U)
#define LTO_POLL_POLL_MIN_US      (1100U)
#define LTO_RETRY_DELAY_MS        (25U)
#define LTO_MAX_CONSECUTIVE_ERRORS (20U)
#define LTO_EXTENDED_READ_BLOCK   (0x21U)
#define LTO_STANDARD_READ_BLOCK   (0x30U)
#define LTO_DIRECTORY_SCAN_BYTES  (256U)

typedef enum {
    LtoScreenMenu,
    LtoScreenReading,
    LtoScreenHealth,
    LtoScreenCleaning,
    LtoScreenFullComplete,
    LtoScreenError,
} LtoScreen;

typedef enum {
    LtoModeHealth,
    LtoModeFull,
} LtoReadMode;

typedef enum {
    LtoPlanBootstrap,
    LtoPlanDirectory,
    LtoPlanPayload,
} LtoPlanStage;

typedef enum {
    LtoStageRequestStandard,
    LtoStageRequestSerial,
    LtoStageSelect,
    LtoStageReadFirstHalf,
    LtoStageReadSecondHalf,
} LtoProtocolStage;

typedef enum {
    LtoErrorNone,
    LtoErrorCommunication,
    LtoErrorUnsupported,
    LtoErrorDirectory,
    LtoErrorRead,
    LtoErrorSave,
    LtoErrorExtendedRead,
} LtoAppError;

typedef struct {
    LtoScreen screen;
    LtoReadMode mode;
    LtoPlanStage plan_stage;
    LtoMediaKind media_kind;
    LtoProtocolStage stage;
    LtoAppError error;
    uint8_t menu_index;
    uint8_t result_page;
    uint32_t attempts;
    uint32_t consecutive_errors;
    uint16_t cartridge_type;
    uint16_t cm_block_count;
    uint16_t generation;
    uint32_t cm_size;
    uint32_t kb_per_dataset;
    uint32_t sets_per_wrap;
    uint16_t wrap_count;
    size_t current_block;
    size_t blocks_read;
    size_t target_index;
    size_t target_count;
    bool health_plan_built;
    bool operation_complete;
    bool save_complete;
    bool stop_requested;
    bool stop_acknowledged;
    bool worm;
    bool type_m;
    uint8_t serial[5];
    LtoHealth health;
    LtoCleaning cleaning;
} LtoState;

typedef struct {
    FuriMutex* mutex;
    LtoState state;
    Nfc* nfc;
    BitBuffer* tx_buffer;
    BitBuffer* rx_buffer;
    uint8_t* cm;
    uint16_t* target_blocks;
    uint8_t* blocks_read;
} LtoApp;

static uint16_t lto_be16(const uint8_t* data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

static void lto_prepare_read(LtoApp* app, size_t block) {
    if(block <= UINT8_MAX) {
        const uint8_t command[] = {LTO_STANDARD_READ_BLOCK, (uint8_t)block};
        bit_buffer_copy_bytes(app->tx_buffer, command, sizeof(command));
    } else {
        const uint8_t command[] = {
            LTO_EXTENDED_READ_BLOCK,
            (uint8_t)block,
            (uint8_t)(block >> 8),
        };
        bit_buffer_copy_bytes(app->tx_buffer, command, sizeof(command));
    }
    iso14443_crc_append(Iso14443CrcTypeA, app->tx_buffer);
}

static bool lto_add_target_block(LtoApp* app, uint16_t block) {
    for(size_t i = 0; i < app->state.target_count; i++) {
        if(app->target_blocks[i] == block) return true;
    }
    if(app->state.target_count >= LTO_MAX_BLOCK_COUNT) return false;
    app->target_blocks[app->state.target_count++] = block;
    return true;
}

static bool lto_find_page(const LtoApp* app, uint16_t page_id, uint16_t* address) {
    const LtoCmParser parser = {
        .data = app->cm,
        .size = app->state.cm_size,
        .blocks_read = app->blocks_read,
        .block_count = app->state.cm_block_count,
    };
    return lto_cm_find_page(&parser, page_id, address);
}

static bool lto_add_page_blocks(
    LtoApp* app,
    uint16_t page_id,
    size_t expected_length) {
    uint16_t address;
    if(!lto_find_page(app, page_id, &address)) return false;
    if((expected_length == 0U) || (address >= app->state.cm_size) ||
       (expected_length > (app->state.cm_size - address))) {
        return false;
    }
    const size_t first_block = address / LTO_BLOCK_SIZE;
    const size_t last_block = (address + expected_length - 1U) / LTO_BLOCK_SIZE;
    if(last_block >= app->state.cm_block_count) return false;
    for(size_t block = first_block; block <= last_block; block++) {
        if(!lto_add_target_block(app, (uint16_t)block)) return false;
    }
    return true;
}

static bool lto_identify_media(LtoApp* app) {
    const LtoCmParser parser = {
        .data = app->cm,
        .size = app->state.cm_size,
        .blocks_read = app->blocks_read,
        .block_count = app->state.cm_block_count,
    };
    LtoMediaInfo info = {0};
    if(!lto_cm_identify_media(&parser, &info)) {
        app->state.error =
            info.media_kind == LtoMediaData ? LtoErrorUnsupported : LtoErrorDirectory;
        return false;
    }
    app->state.media_kind = info.media_kind;
    app->state.cartridge_type = info.cartridge_type;
    app->state.generation = info.generation;
    app->state.kb_per_dataset = info.kb_per_dataset;
    app->state.sets_per_wrap = info.sets_per_wrap;
    app->state.wrap_count = info.wrap_count;
    app->state.worm = info.worm;
    return true;
}

static bool lto_build_payload_plan(LtoApp* app) {
    if(app->state.mode == LtoModeFull) {
        for(uint16_t block = 0; block < app->state.cm_block_count; block++) {
            if(!lto_add_target_block(app, block)) return false;
        }
        return true;
    }

    if(app->state.media_kind == LtoMediaCleaning) {
        return lto_add_page_blocks(app, 0x105U, 28U);
    }

    const size_t usage_length = app->state.generation >= 5U ? 96U : 64U;
    bool success = lto_add_page_blocks(app, 0x105U, 64U) &&
                   lto_add_page_blocks(app, 0x108U, usage_length) &&
                   lto_add_page_blocks(app, 0x109U, usage_length) &&
                   lto_add_page_blocks(app, 0x10AU, usage_length) &&
                   lto_add_page_blocks(app, 0x10BU, usage_length);
    if(success && (app->state.generation == 7U)) {
        success = lto_add_page_blocks(app, 0x10DU, 64U);
    }
    return success;
}

static bool lto_set_cm_size(LtoState* state, uint8_t size_kib) {
    switch(size_kib) {
    case 4U:
        state->cm_size = 4096U;
        state->cm_block_count = 128U;
        break;
    case 8U:
        state->cm_size = 8160U;
        state->cm_block_count = 255U;
        break;
    case 16U:
        state->cm_size = 16352U;
        state->cm_block_count = 511U;
        break;
    case 32U:
        state->cm_size = 32736U;
        state->cm_block_count = 1023U;
        break;
    default:
        return false;
    }
    return true;
}

static bool lto_parse_health(LtoApp* app) {
    const LtoCmParser parser = {
        .data = app->cm,
        .size = app->state.cm_size,
        .blocks_read = app->blocks_read,
        .block_count = app->state.cm_block_count,
    };
    LtoMediaInfo info = {
        .media_kind = app->state.media_kind,
        .cartridge_type = app->state.cartridge_type,
        .generation = app->state.generation,
        .kb_per_dataset = app->state.kb_per_dataset,
        .sets_per_wrap = app->state.sets_per_wrap,
        .wrap_count = app->state.wrap_count,
        .worm = app->state.worm,
        .type_m = app->state.type_m,
    };
    if(!lto_cm_parse_health(&parser, &info, &app->state.health)) return false;
    app->state.wrap_count = info.wrap_count;
    app->state.type_m = info.type_m;
    return true;
}

static bool lto_parse_cleaning(LtoApp* app) {
    const LtoCmParser parser = {
        .data = app->cm,
        .size = app->state.cm_size,
        .blocks_read = app->blocks_read,
        .block_count = app->state.cm_block_count,
    };
    return lto_cm_parse_cleaning(&parser, &app->state.cleaning);
}

static const char* lto_error_text(LtoAppError error) {
    switch(error) {
    case LtoErrorCommunication:
        return "Communication lost";
    case LtoErrorUnsupported:
        return "Unsupported LTO media";
    case LtoErrorDirectory:
        return "Invalid page directory";
    case LtoErrorRead:
        return "Read or CRC error";
    case LtoErrorSave:
        return "SD card save error";
    case LtoErrorExtendedRead:
        return "Extended read unverified";
    case LtoErrorNone:
    default:
        return "Unknown error";
    }
}

static void lto_draw_menu(Canvas* canvas, const LtoState* state) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 10, "LTO CM Reader");
    canvas_draw_str(canvas, 2, 22, "Place firmly on CM chip");
    canvas_draw_str(canvas, 2, 37, state->menu_index == 0U ? "> Health information" :
                                                               "  Health information");
    canvas_draw_str(canvas, 2, 50, state->menu_index == 1U ? "> Full dump to file" :
                                                               "  Full dump to file");
    canvas_draw_str(canvas, 2, 63, "Up/Down  OK: start");
}

static void lto_draw_health(Canvas* canvas, const LtoState* state) {
    const LtoHealth* h = &state->health;
    char line[32];
    canvas_set_font(canvas, FontPrimary);
    snprintf(
        line,
        sizeof(line),
        "LTO-%u%s",
        state->generation,
        state->worm ? " WORM" : (state->type_m ? " M8" : ""));
    canvas_draw_str(canvas, 2, 11, line);
    snprintf(line, sizeof(line), "%u/5", state->result_page + 1U);
    canvas_draw_str_aligned(canvas, 126, 11, AlignRight, AlignBottom, line);
    canvas_set_font(canvas, FontSecondary);

    if(state->result_page == 0U) {
        snprintf(line, sizeof(line), "Written: %.2f TB", h->written_tb);
        canvas_draw_str(canvas, 2, 26, line);
        snprintf(line, sizeof(line), "Read: %.2f TB", h->read_tb);
        canvas_draw_str(canvas, 2, 39, line);
        snprintf(line, sizeof(line), "Full volumes: %.2f FVE", h->fve);
        canvas_draw_str(canvas, 2, 52, line);
    } else if(state->result_page == 1U) {
        snprintf(line, sizeof(line), "Load count: %lu", (unsigned long)h->load_count);
        canvas_draw_str(canvas, 2, 26, line);
        if(h->enhanced_metrics) {
            snprintf(
                line, sizeof(line), "LP3 passes: %lu", (unsigned long)h->lp3_passes);
        } else {
            snprintf(line, sizeof(line), "LP3 passes: N/A");
        }
        canvas_draw_str(canvas, 2, 39, line);
        if(h->enhanced_metrics) {
            snprintf(
                line, sizeof(line), "Midpoint: %lu", (unsigned long)h->midpoint_passes);
        } else {
            snprintf(line, sizeof(line), "Midpoint: N/A");
        }
        canvas_draw_str(canvas, 2, 52, line);
    } else if(state->result_page == 2U) {
        snprintf(line, sizeof(line), "Write retries: %lu", (unsigned long)h->write_retries);
        canvas_draw_str(canvas, 2, 31, line);
        snprintf(line, sizeof(line), "Read retries: %lu", (unsigned long)h->read_retries);
        canvas_draw_str(canvas, 2, 47, line);
    } else if(state->result_page == 3U) {
        snprintf(line, sizeof(line), "Unrec write: %u", h->unrecovered_writes);
        canvas_draw_str(canvas, 2, 26, line);
        snprintf(line, sizeof(line), "Unrec read: %u", h->unrecovered_reads);
        canvas_draw_str(canvas, 2, 39, line);
        snprintf(line, sizeof(line), "Suspended write: %u", h->suspended_writes);
        canvas_draw_str(canvas, 2, 52, line);
    } else {
        snprintf(line, sizeof(line), "Made: %s", h->manufacture_date);
        canvas_draw_str(canvas, 2, 26, line);
        snprintf(line, sizeof(line), "Fatal suspended: %u", h->fatal_suspended_writes);
        canvas_draw_str(canvas, 2, 39, line);
        if(h->enhanced_metrics) {
            snprintf(line, sizeof(line), "Max tape temp: %u C", h->max_tape_temp);
        } else {
            snprintf(line, sizeof(line), "Max tape temp: N/A");
        }
        canvas_draw_str(canvas, 2, 52, line);
    }
    canvas_draw_str(canvas, 2, 63, "Left/Right  Back: menu");
}

static void lto_draw_cleaning(Canvas* canvas, const LtoState* state) {
    const LtoCleaning* cleaning = &state->cleaning;
    char line[32];
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Universal Cleaning Tape");
    canvas_set_font(canvas, FontSecondary);
    snprintf(line, sizeof(line), "Made: %s", cleaning->manufacture_date);
    canvas_draw_str(canvas, 2, 24, line);
    snprintf(
        line,
        sizeof(line),
        "Cleans: %lu  Remain~%u",
        (unsigned long)cleaning->cleans_performed,
        cleaning->estimated_remaining);
    canvas_draw_str(canvas, 2, 37, line);
    snprintf(
        line,
        sizeof(line),
        "Used: %u.%02u m  %s",
        cleaning->last_location_quarter_m / 4U,
        (cleaning->last_location_quarter_m % 4U) * 25U,
        cleaning->expired ? "EXPIRED" : "USABLE");
    canvas_draw_str(canvas, 2, 50, line);
    canvas_draw_str(canvas, 2, 63, "Back: menu");
}

static void lto_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    LtoApp* app = context;
    LtoState state;
    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
    state = app->state;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    if(state.screen == LtoScreenMenu) {
        lto_draw_menu(canvas, &state);
    } else if(state.screen == LtoScreenHealth) {
        lto_draw_health(canvas, &state);
    } else if(state.screen == LtoScreenCleaning) {
        lto_draw_cleaning(canvas, &state);
    } else if(state.screen == LtoScreenReading) {
        char line[32];
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 11, state.mode == LtoModeHealth ? "Reading health" :
                                                                  "Reading full CM");
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            line,
            sizeof(line),
            "Blocks: %u / %u",
            (unsigned int)state.blocks_read,
            (unsigned int)state.target_count);
        canvas_draw_str(canvas, 2, 29, line);
        snprintf(line, sizeof(line), "Current block: %u", (unsigned int)state.current_block);
        canvas_draw_str(canvas, 2, 43, line);
        canvas_draw_str(canvas, 2, 57, "Keep Flipper firmly still");
    } else if(state.screen == LtoScreenFullComplete) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "Full dump complete");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 31, state.save_complete ? "Saved to apps_data" : "Saving...");
        canvas_draw_str(canvas, 2, 50, "Back: main menu");
    } else {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "LTO CM read failed");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 32, lto_error_text(state.error));
        canvas_draw_str(canvas, 2, 50, "Back: main menu");
    }
}

static void lto_input_callback(InputEvent* input_event, void* context) {
    furi_assert(input_event);
    furi_assert(context);
    furi_message_queue_put(context, input_event, FuriWaitForever);
}

static void lto_set_terminal_error(LtoApp* app, LtoAppError error) {
    app->state.error = error;
    app->state.screen = LtoScreenError;
    app->state.operation_complete = true;
}

static bool lto_add_directory_blocks(LtoApp* app) {
    uint16_t unprotected_address = 0;
    for(size_t offset = 0x24U; (offset + 4U) <= 128U; offset += 4U) {
        const uint8_t version = app->cm[offset] >> 4;
        const uint16_t id =
            ((uint16_t)(app->cm[offset] & 0x0FU) << 8) | app->cm[offset + 1U];
        if((version == 0U) && (id == 0x0FFFU)) {
            unprotected_address = lto_be16(&app->cm[offset + 2U]);
            break;
        }
    }
    if((unprotected_address == 0U) ||
       (unprotected_address >= app->state.cm_size)) {
        return false;
    }

    const uint16_t first_block = unprotected_address / LTO_BLOCK_SIZE;
    uint16_t last_block =
        (unprotected_address + LTO_DIRECTORY_SCAN_BYTES - 1U) / LTO_BLOCK_SIZE;
    if(last_block >= app->state.cm_block_count) {
        last_block = app->state.cm_block_count - 1U;
    }
    for(uint16_t block = first_block; block <= last_block; block++) {
        if(!lto_add_target_block(app, block)) return false;
    }
    return true;
}

static bool lto_finish_payload(LtoApp* app) {
    if(app->state.mode == LtoModeFull) {
        app->state.screen = LtoScreenFullComplete;
        return true;
    }
    if(app->state.media_kind == LtoMediaCleaning) {
        if(!lto_parse_cleaning(app)) return false;
        app->state.screen = LtoScreenCleaning;
        return true;
    }
    if(!lto_parse_health(app)) return false;
    app->state.screen = LtoScreenHealth;
    return true;
}

static bool lto_advance_read_plan(LtoApp* app, bool* complete) {
    *complete = false;
    if(app->state.plan_stage == LtoPlanBootstrap) {
        if((app->cm[0] ^ app->cm[1] ^ app->cm[2] ^ app->cm[3]) != app->cm[4] ||
           !lto_set_cm_size(&app->state, app->cm[5]) ||
           !lto_identify_media(app)) {
            return false;
        }

        if(app->state.mode == LtoModeFull) {
            app->state.plan_stage = LtoPlanPayload;
            return lto_build_payload_plan(app);
        }
        app->state.plan_stage = LtoPlanDirectory;
        return lto_add_directory_blocks(app);
    }

    if(app->state.plan_stage == LtoPlanDirectory) {
        app->state.plan_stage = LtoPlanPayload;
        return lto_build_payload_plan(app);
    }

    *complete = true;
    return lto_finish_payload(app);
}

static NfcCommand lto_nfc_callback(NfcEvent event, void* context) {
    furi_assert(context);
    LtoApp* app = context;
    if(event.type != NfcEventTypePollerReady) return NfcCommandContinue;

    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
    const bool stop_requested = app->state.stop_requested;
    const bool operation_complete = app->state.operation_complete;
    if(stop_requested || operation_complete) {
        app->state.stop_acknowledged = true;
    }
    furi_mutex_release(app->mutex);
    if(stop_requested || operation_complete) {
        return NfcCommandStop;
    }

    bit_buffer_reset(app->rx_buffer);
    const NfcError error =
        nfc_poller_trx(app->nfc, app->tx_buffer, app->rx_buffer, LTO_FWT_FC);

    bool terminal = false;
    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);
    app->state.attempts++;

    bool valid_response = false;
    if((error == NfcErrorNone) && (bit_buffer_get_size(app->rx_buffer) != 0U)) {
        const size_t rx_bits = bit_buffer_get_size(app->rx_buffer);
        const size_t rx_bytes = bit_buffer_get_size_bytes(app->rx_buffer);
        const uint8_t* rx = bit_buffer_get_data(app->rx_buffer);

        if(app->state.stage == LtoStageRequestStandard) {
            const uint8_t request_serial[] = {0x93U, 0x20U};
            bit_buffer_copy_bytes(app->tx_buffer, request_serial, sizeof(request_serial));
            app->state.stage = LtoStageRequestSerial;
            valid_response = true;
        } else if(app->state.stage == LtoStageRequestSerial) {
            if((rx_bytes >= 5U) &&
               ((rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) == rx[4])) {
                memcpy(app->state.serial, rx, sizeof(app->state.serial));
                const uint8_t select_command[] = {
                    0x93U,
                    0x70U,
                    rx[0],
                    rx[1],
                    rx[2],
                    rx[3],
                    rx[4],
                };
                bit_buffer_copy_bytes(app->tx_buffer, select_command, sizeof(select_command));
                iso14443_crc_append(Iso14443CrcTypeA, app->tx_buffer);
                app->state.stage = LtoStageSelect;
                valid_response = true;
            }
        } else if(app->state.stage == LtoStageSelect) {
            if(((rx_bits == 4U) || (rx_bits == 8U)) && ((rx[0] & 0x0FU) == 0x0AU)) {
                app->state.current_block = app->target_blocks[0];
                app->state.stage = LtoStageReadFirstHalf;
                lto_prepare_read(app, app->state.current_block);
                valid_response = true;
            }
        } else if(app->state.stage == LtoStageReadFirstHalf) {
            if((rx_bytes == LTO_READ_RESPONSE_SIZE) &&
               iso14443_crc_check(Iso14443CrcTypeA, app->rx_buffer)) {
                memcpy(
                    &app->cm[app->state.current_block * LTO_BLOCK_SIZE],
                    rx,
                    LTO_HALF_BLOCK_SIZE);
                const uint8_t read_continue = 0x80U;
                bit_buffer_copy_bytes(app->tx_buffer, &read_continue, sizeof(read_continue));
                app->state.stage = LtoStageReadSecondHalf;
                valid_response = true;
            }
        } else if(app->state.stage == LtoStageReadSecondHalf) {
            if((rx_bytes == LTO_READ_RESPONSE_SIZE) &&
               iso14443_crc_check(Iso14443CrcTypeA, app->rx_buffer)) {
                memcpy(
                    &app->cm[app->state.current_block * LTO_BLOCK_SIZE + LTO_HALF_BLOCK_SIZE],
                    rx,
                    LTO_HALF_BLOCK_SIZE);
                app->blocks_read[app->state.current_block] = 1U;
                app->state.blocks_read++;
                app->state.target_index++;
                valid_response = true;

                if(!terminal && (app->state.target_index >= app->state.target_count)) {
                    bool complete = false;
                    if(!lto_advance_read_plan(app, &complete)) {
                        if(app->state.error == LtoErrorNone) {
                            app->state.error = LtoErrorDirectory;
                        }
                        app->state.screen = LtoScreenError;
                        app->state.operation_complete = true;
                        terminal = true;
                    } else if(complete) {
                        app->state.operation_complete = true;
                        terminal = true;
                    }
                }

                if(!terminal) {
                    app->state.current_block = app->target_blocks[app->state.target_index];
                    app->state.stage = LtoStageReadFirstHalf;
                    lto_prepare_read(app, app->state.current_block);
                }
            }
        }
    }

    if(valid_response) {
        app->state.consecutive_errors = 0;
    } else if(!terminal) {
        app->state.consecutive_errors++;
        if(app->state.consecutive_errors >= LTO_MAX_CONSECUTIVE_ERRORS) {
            const bool reading_block = (app->state.stage == LtoStageReadFirstHalf) ||
                                       (app->state.stage == LtoStageReadSecondHalf);
            lto_set_terminal_error(
                app, reading_block ? LtoErrorRead : LtoErrorCommunication);
            terminal = true;
        }
    }
    const bool should_stop = terminal || app->state.stop_requested;
    if(should_stop) app->state.stop_acknowledged = true;
    furi_mutex_release(app->mutex);

    if(should_stop) return NfcCommandStop;
    furi_delay_ms(LTO_RETRY_DELAY_MS);
    return NfcCommandContinue;
}

static bool lto_save_full_dump(LtoApp* app) {
    char path[48];
    snprintf(
        path,
        sizeof(path),
        APP_DATA_PATH("%02X%02X%02X%02X.bin"),
        app->state.serial[0],
        app->state.serial[1],
        app->state.serial[2],
        app->state.serial[3]);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(success) {
        const size_t size = app->state.cm_size;
        success = storage_file_write(file, app->cm, size) == size;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return success;
}

static void lto_prepare_operation(LtoApp* app, LtoReadMode mode) {
    memset(app->cm, 0, LTO_MAX_CM_SIZE);
    memset(app->target_blocks, 0, LTO_MAX_BLOCK_COUNT * sizeof(uint16_t));
    memset(app->blocks_read, 0, LTO_MAX_BLOCK_COUNT);
    app->state.mode = mode;
    app->state.plan_stage = LtoPlanBootstrap;
    app->state.media_kind = LtoMediaUnknown;
    app->state.screen = LtoScreenReading;
    app->state.stage = LtoStageRequestStandard;
    app->state.error = LtoErrorNone;
    app->state.attempts = 0;
    app->state.consecutive_errors = 0;
    app->state.cartridge_type = 0;
    app->state.cm_block_count = 0;
    app->state.generation = 0;
    app->state.cm_size = 0;
    app->state.kb_per_dataset = 0;
    app->state.sets_per_wrap = 0;
    app->state.wrap_count = 0;
    app->state.blocks_read = 0;
    app->state.target_index = 0;
    app->state.target_count = 0;
    app->state.health_plan_built = false;
    app->state.operation_complete = false;
    app->state.save_complete = false;
    app->state.stop_requested = false;
    app->state.stop_acknowledged = false;
    app->state.worm = false;
    app->state.type_m = false;
    app->state.result_page = 0;
    memset(&app->state.health, 0, sizeof(app->state.health));
    memset(&app->state.cleaning, 0, sizeof(app->state.cleaning));

    const uint16_t bootstrap[] = {0U, 1U, 2U, 3U};
    for(size_t i = 0; i < COUNT_OF(bootstrap); i++) {
        app->target_blocks[i] = bootstrap[i];
    }
    app->state.target_count = COUNT_OF(bootstrap);

    const uint8_t request_standard = LTO_REQUEST_STANDARD;
    bit_buffer_copy_bits(app->tx_buffer, &request_standard, LTO_REQUEST_STANDARD_BITS);
}

int32_t lto_cm_probe_app(void* context) {
    UNUSED(context);
    LtoApp app = {
        .state =
            {
                .screen = LtoScreenMenu,
                .mode = LtoModeHealth,
            },
    };

    app.mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app.tx_buffer = bit_buffer_alloc(9);
    app.rx_buffer = bit_buffer_alloc(LTO_RESPONSE_CAPACITY);
    app.cm = malloc(LTO_MAX_CM_SIZE);
    app.target_blocks = malloc(LTO_MAX_BLOCK_COUNT * sizeof(uint16_t));
    app.blocks_read = malloc(LTO_MAX_BLOCK_COUNT);
    furi_check(app.cm);
    furi_check(app.target_blocks);
    furi_check(app.blocks_read);

    FuriMessageQueue* input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, lto_draw_callback, &app);
    view_port_input_callback_set(view_port, lto_input_callback, input_queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    app.nfc = nfc_alloc();

    bool running = true;
    bool nfc_running = false;
    bool save_attempted = false;
    while(running) {
        bool start_nfc = false;
        InputEvent event;
        const FuriStatus queue_status =
            furi_message_queue_get(input_queue, &event, furi_ms_to_ticks(100));

        if(queue_status == FuriStatusOk && event.type == InputTypeShort) {
            furi_check(furi_mutex_acquire(app.mutex, FuriWaitForever) == FuriStatusOk);
            if(app.state.screen == LtoScreenMenu) {
                if((event.key == InputKeyUp) || (event.key == InputKeyDown)) {
                    app.state.menu_index ^= 1U;
                } else if((event.key == InputKeyOk) && !nfc_running) {
                    const LtoReadMode mode =
                        app.state.menu_index == 0U ? LtoModeHealth : LtoModeFull;
                    lto_prepare_operation(&app, mode);
                    start_nfc = true;
                    save_attempted = false;
                } else if(event.key == InputKeyBack) {
                    running = false;
                }
            } else if(app.state.screen == LtoScreenHealth) {
                if(event.key == InputKeyLeft) {
                    app.state.result_page = app.state.result_page == 0U ? 4U :
                                                                           app.state.result_page - 1U;
                } else if(event.key == InputKeyRight) {
                    app.state.result_page = (app.state.result_page + 1U) % 5U;
                } else if(event.key == InputKeyBack) {
                    app.state.screen = LtoScreenMenu;
                }
            } else if(app.state.screen == LtoScreenReading) {
                if(event.key == InputKeyBack) {
                    app.state.stop_requested = true;
                    app.state.screen = LtoScreenMenu;
                }
            } else if(event.key == InputKeyBack) {
                app.state.screen = LtoScreenMenu;
            }
            furi_mutex_release(app.mutex);
        }

        if(start_nfc) {
            nfc_config(app.nfc, NfcModePoller, NfcTechIso14443a);
            nfc_set_guard_time_us(app.nfc, LTO_GUARD_TIME_US);
            nfc_set_fdt_poll_fc(app.nfc, LTO_FDT_POLL_FC);
            nfc_set_fdt_poll_poll_us(app.nfc, LTO_POLL_POLL_MIN_US);
            nfc_start(app.nfc, lto_nfc_callback, &app);
            nfc_running = true;
        }

        bool operation_complete;
        bool stop_acknowledged;
        LtoReadMode completed_mode;
        LtoScreen completed_screen;
        furi_check(furi_mutex_acquire(app.mutex, FuriWaitForever) == FuriStatusOk);
        operation_complete = app.state.operation_complete;
        stop_acknowledged = app.state.stop_acknowledged;
        completed_mode = app.state.mode;
        completed_screen = app.state.screen;
        furi_mutex_release(app.mutex);

        if(nfc_running && stop_acknowledged) {
            nfc_stop(app.nfc);
            nfc_running = false;
        }

        if(operation_complete && (completed_mode == LtoModeFull) &&
           (completed_screen == LtoScreenFullComplete) && !save_attempted) {
            save_attempted = true;
            const bool saved = lto_save_full_dump(&app);
            furi_check(furi_mutex_acquire(app.mutex, FuriWaitForever) == FuriStatusOk);
            app.state.save_complete = saved;
            if(!saved) {
                app.state.error = LtoErrorSave;
                app.state.screen = LtoScreenError;
            }
            furi_mutex_release(app.mutex);
        }

        view_port_update(view_port);
    }

    if(nfc_running) {
        furi_check(furi_mutex_acquire(app.mutex, FuriWaitForever) == FuriStatusOk);
        app.state.stop_requested = true;
        furi_mutex_release(app.mutex);
        nfc_stop(app.nfc);
    }
    nfc_free(app.nfc);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(input_queue);
    bit_buffer_free(app.rx_buffer);
    bit_buffer_free(app.tx_buffer);
    free(app.blocks_read);
    free(app.target_blocks);
    free(app.cm);
    furi_mutex_free(app.mutex);
    return 0;
}
