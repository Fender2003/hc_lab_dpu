#include "task1.h"

void compress_state_changed_callback(
    union doca_data user_data,
    struct doca_ctx *ctx,
    enum doca_ctx_states prev_state,
    enum doca_ctx_states next_state
) {
    (void) ctx;
    (void) prev_state;

    if(next_state == DOCA_CTX_STATE_RUNNING) {
        struct app_state *state = (struct app_state *) user_data.ptr;
        struct doca_buf *buf_in;
        struct doca_buf *buf_out;
        struct doca_compress_task_decompress_deflate *task;
        union doca_data task_user_data = { .ptr = state };
        doca_error_t err;

        err = doca_buf_inventory_buf_get_by_addr(
            state->inventory, state->mmap_in, state->compressed_data, state->compressed_size, &buf_in
        );
        if(err != DOCA_SUCCESS) {
            LOG("could not create input buffer: %s", doca_error_get_descr(err));
            doca_ctx_stop(doca_compress_as_ctx(state->compress));
            return;
        }

        err = doca_buf_inventory_buf_get_by_addr(
            state->inventory, state->mmap_out, state->decompressed_data, 1024, &buf_out
        );
        if(err != DOCA_SUCCESS) {
            LOG("could not create output buffer: %s", doca_error_get_descr(err));
            doca_buf_dec_refcount(buf_in, NULL);
            doca_ctx_stop(doca_compress_as_ctx(state->compress));
            return;
        }

        err = doca_compress_task_decompress_deflate_alloc_init(
            state->compress, buf_in, buf_out, task_user_data, &task
        );
        if(err != DOCA_SUCCESS) {
            LOG("could not allocate decompression task: %s", doca_error_get_descr(err));
            doca_buf_dec_refcount(buf_in, NULL);
            doca_buf_dec_refcount(buf_out, NULL);
            doca_ctx_stop(doca_compress_as_ctx(state->compress));
            return;
        }

        err = doca_task_submit(doca_compress_task_decompress_deflate_as_task(task));
        if(err != DOCA_SUCCESS) {
            LOG("could not submit decompression task: %s", doca_error_get_descr(err));
            doca_task_free(doca_compress_task_decompress_deflate_as_task(task));
            doca_buf_dec_refcount(buf_in, NULL);
            doca_buf_dec_refcount(buf_out, NULL);
            doca_ctx_stop(doca_compress_as_ctx(state->compress));
        }
    }
}

void decompress_completed_callback(
    struct doca_compress_task_decompress_deflate *decompress_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data
) {
    (void) task_user_data;

    struct app_state *state = (struct app_state *) ctx_user_data.ptr;
    struct doca_buf *buf_out = doca_compress_task_decompress_deflate_get_dst(decompress_task);
    uint8_t *data;
    size_t data_len;
    doca_error_t err;

    err = doca_buf_get_data(buf_out, (void **) &data);
    if(err != DOCA_SUCCESS) {
        LOG("could not get decompressed data: %s", doca_error_get_descr(err));
    } else {
        err = doca_buf_get_data_len(buf_out, &data_len);
        if(err != DOCA_SUCCESS) {
            LOG("could not get decompressed data length: %s", doca_error_get_descr(err));
        } else {
            fwrite(data, 1, data_len, stdout);
        }
    }

    doca_buf_dec_refcount((struct doca_buf *) doca_compress_task_decompress_deflate_get_src(decompress_task), NULL);
    doca_buf_dec_refcount(buf_out, NULL);
    doca_task_free(doca_compress_task_decompress_deflate_as_task(decompress_task));
    state->done = true;
    doca_ctx_stop(doca_compress_as_ctx(state->compress));
}

void decompress_error_callback(
    struct doca_compress_task_decompress_deflate *decompress_task,
    union doca_data task_user_data,
    union doca_data ctx_user_data
) {
    (void) task_user_data;

    struct app_state *state = (struct app_state *) ctx_user_data.ptr;
    struct doca_task *base_task = doca_compress_task_decompress_deflate_as_task(decompress_task);
    doca_error_t status = doca_task_get_status(base_task);

    LOG("Compression task failed: %s", doca_error_get_descr(status));

    // Cleanup
    struct doca_buf const *buf_in = doca_compress_task_decompress_deflate_get_src(decompress_task);
    struct doca_buf *buf_out = doca_compress_task_decompress_deflate_get_dst(decompress_task);
    doca_buf_dec_refcount((struct doca_buf*) buf_in, NULL);
    doca_buf_dec_refcount(buf_out, NULL);
    doca_task_free(base_task);
    state->done = true;
    doca_ctx_stop(doca_compress_as_ctx(state->compress));
}

int main(void) {
    struct doca_log_backend *sdk_log;
    int epoll_fd;

    doca_log_backend_create_standard();
    doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

    _Alignas(64) uint8_t compressed_data[] = {
        0x05, 0x40, 0x67, 0x15, 0x80, 0x20, 0x10, 0xfe, 0x4f, 0x8a, 0x2f, 0x81, 0x69, 0x2c, 0xe0, 0x38,
        0x36, 0x1c, 0x1b, 0x30, 0xbd, 0xef, 0xd4, 0x84, 0xdc, 0xcd, 0xe3, 0x70, 0x17, 0x9e, 0x11, 0x92,
        0x17, 0x6c, 0x0f, 0xa9, 0x82, 0x07, 0x15, 0x34, 0x4d, 0xf0, 0xd7, 0xb7, 0xf1, 0xb2, 0x3a, 0xc4,
        0x0f
    };
    _Alignas(64) uint8_t decompressed_data[1024];
    doca_error_t err;

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if(epoll_fd == -1) {
        LOG("could not create epoll file descriptor: %s", strerror(errno));
        goto end;
    }

    struct doca_pe *engine = open_progress_engine(epoll_fd);
    if(engine == NULL) {
        LOG("could not obtain progress engine");
        goto cleanup_epoll;
    }

    struct doca_dev *compress_dev = open_compress_device();
    if(compress_dev == NULL) {
        LOG("could not obtain compression device");
        goto cleanup_pe;
    }

    struct doca_mmap *mmap_in = open_memory_map(compressed_data, sizeof compressed_data, compress_dev, DOCA_ACCESS_FLAG_LOCAL_READ_ONLY);
    if(mmap_in == NULL) {
        LOG("could not obtain input memory map");
        goto cleanup_dev;
    }

    struct doca_mmap *mmap_out = open_memory_map(decompressed_data, sizeof decompressed_data, compress_dev, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if(mmap_out == NULL) {
        LOG("could not obtain output memory map");
        goto cleanup_mmap_in;
    }

    struct doca_buf_inventory *inv;
    err = doca_buf_inventory_create(2, &inv);
    if(err != DOCA_SUCCESS) {
        LOG("could not create buffer inventory: %s", doca_error_get_descr(err));
        goto cleanup_mmap_out;
    }

    err = doca_buf_inventory_start(inv);
    if(err != DOCA_SUCCESS) {
        LOG("could not start buffer inventory: %s", doca_error_get_descr(err));
        goto cleanup_inv;
    }

    struct app_state state = {
        .inventory = inv,
        .mmap_in = mmap_in,
        .mmap_out = mmap_out,
        .compressed_data = compressed_data,
        .compressed_size = sizeof compressed_data,
        .decompressed_data = decompressed_data,
        .done = false
    };

    struct doca_compress *compress = open_decompress_context(compress_dev, engine, &state, compress_state_changed_callback, decompress_completed_callback, decompress_error_callback);
    if(compress == NULL) {
        LOG("could not obtain compression context");
        goto cleanup_inv;
    }

    while(!state.done) {
        struct epoll_event event;

        err = doca_pe_request_notification(engine);
        if(err != DOCA_SUCCESS) {
            LOG("could not request progress notification: %s", doca_error_get_descr(err));
            break;
        }

        if(epoll_wait(epoll_fd, &event, 1, -1) == -1) {
            LOG("epoll_wait failed: %s", strerror(errno));
            break;
        }

        doca_pe_clear_notification(engine, 0);
        while(doca_pe_progress(engine) > 0) {
        }
    }

    doca_compress_destroy(compress);
cleanup_inv:
    doca_buf_inventory_stop(inv);
    doca_buf_inventory_destroy(inv);
cleanup_mmap_out:
    doca_mmap_destroy(mmap_out);
cleanup_mmap_in:
    doca_mmap_destroy(mmap_in);
cleanup_dev:
    doca_dev_close(compress_dev);
cleanup_pe:
    doca_pe_destroy(engine);
cleanup_epoll:
    close(epoll_fd);
end:
}
