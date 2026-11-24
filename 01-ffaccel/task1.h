#pragma once

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_compress.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "[%s] " fmt "\n", __func__ __VA_OPT__(,) __VA_ARGS__)


struct app_state {
    struct doca_compress *compress;
    struct doca_buf_inventory *inventory;
    struct doca_mmap *mmap_in;
    struct doca_mmap *mmap_out;
    uint8_t *compressed_data;
    size_t compressed_size;
    uint8_t *decompressed_data;
    bool done;
};

struct doca_pe *open_progress_engine(int epoll_fd);
struct doca_mmap *open_memory_map(uint8_t *base, size_t size, struct doca_dev *dev, uint32_t permissions);
struct doca_dev *open_compress_device(void);

struct doca_compress *open_decompress_context(
    struct doca_dev *dev,
    struct doca_pe *engine,
    struct app_state *state,
    doca_ctx_state_changed_callback_t state_changed_handler,
    doca_compress_task_decompress_deflate_completion_cb_t completion_handler,
    doca_compress_task_decompress_deflate_completion_cb_t error_handler
);