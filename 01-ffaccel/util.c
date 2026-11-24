#include "task1.h"

#include <stdio.h>
#include <sys/epoll.h>

struct doca_pe *open_progress_engine(int epoll_fd) {
    struct doca_pe *engine;
    doca_error_t err;

    err = doca_pe_create(&engine);
    if(err != DOCA_SUCCESS) {
        LOG("could not create progress engine: %s", doca_error_get_descr(err));
        return NULL;
    }

    doca_event_handle_t event_handle;
    err = doca_pe_get_notification_handle(engine, &event_handle);
    if(err != DOCA_SUCCESS) {
        LOG("could not obtain notification handle: %s", doca_error_get_descr(err));
        goto failure;
    }

    struct epoll_event events_in = { EPOLLIN, { .fd = event_handle }};
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_handle, &events_in) != 0) {
        LOG("could not attach to epoll handle: %s", strerror(errno));
        goto failure;
    }

    return engine;

failure:
    doca_pe_destroy(engine);
    return NULL;
}

struct doca_mmap *open_memory_map(uint8_t *base, size_t size, struct doca_dev *dev, uint32_t permissions) {
    struct doca_mmap *map;
    doca_error_t err;

    err = doca_mmap_create(&map);
    if(err != DOCA_SUCCESS) {
        LOG("could not create memory map: %s", doca_error_get_descr(err));
        return NULL;
    }

    err = doca_mmap_set_memrange(map, base, size);
    if(err != DOCA_SUCCESS) {
        LOG("could not set memory range: %s", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_mmap_set_permissions(map, permissions);
    if(err != DOCA_SUCCESS) {
        LOG("could not set permissions: %s", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_mmap_add_dev(map, dev);
    if(err != DOCA_SUCCESS) {
        LOG("could not add device: %s", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_mmap_start(map);
    if(err != DOCA_SUCCESS) {
        LOG("could not start memory map: %s", doca_error_get_descr(err));
        goto failure;
    }

    return map;

failure:
    doca_mmap_destroy(map);
    return NULL;
}

struct doca_dev *open_compress_device(void) {
    struct doca_dev *result = NULL;
    struct doca_devinfo **dev_list;
    uint32_t nb_devs;
    doca_error_t err;

    err = doca_devinfo_create_list(&dev_list, &nb_devs);

    if(err != DOCA_SUCCESS) {
        LOG("could not get device list: %s", doca_error_get_descr(err));
        return NULL;
    }

    for(uint32_t i = 0; i < nb_devs; ++i) {
        if(doca_compress_cap_task_decompress_deflate_is_supported(dev_list[i]) == DOCA_SUCCESS) {
            err = doca_dev_open(dev_list[i], &result);

            if(err == DOCA_SUCCESS) {
                goto cleanup;
            } else {
                LOG("could not open device: %s", doca_error_get_descr(err));
            }
        }
    }

    LOG("no compression device found");

cleanup:
    doca_devinfo_destroy_list(dev_list);
    return result;
}


struct doca_compress *open_decompress_context(
    struct doca_dev *dev,
    struct doca_pe *engine,
    struct app_state *state,
    doca_ctx_state_changed_callback_t state_changed_handler,
    doca_compress_task_decompress_deflate_completion_cb_t completion_handler,
    doca_compress_task_decompress_deflate_completion_cb_t error_handler
) {
    struct doca_compress *compress;
    struct doca_ctx *ctx;
    doca_error_t err;

    err = doca_compress_create(dev, &compress);
    if(err != DOCA_SUCCESS) {
        LOG("could not create context: %s", doca_error_get_descr(err));
        goto failure;
    }

    state->compress = compress;

    ctx = doca_compress_as_ctx(compress);
    err = doca_ctx_set_state_changed_cb(ctx, state_changed_handler);
    if(err != DOCA_SUCCESS) {
        LOG("could not set state-change callback: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    union doca_data ctx_user_data = { .ptr = state };
    err = doca_ctx_set_user_data(ctx, ctx_user_data);
    if(err != DOCA_SUCCESS) {
        LOG("could not set context user data: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_compress_task_decompress_deflate_set_conf(compress, completion_handler, error_handler, 16);
    if(err != DOCA_SUCCESS) {
        LOG("could not set task callbacks: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_pe_connect_ctx(engine, ctx);
    if(err != DOCA_SUCCESS) {
        LOG("could not connect to progress engine: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_ctx_start(ctx);
    if(err != DOCA_SUCCESS) {
        LOG("could not start context: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    return compress;

failure_cleanup:
    doca_compress_destroy(compress);
    state->compress = NULL;
failure:
    return NULL;    
}