#include "task2.h"

struct doca_dev *open_device(char const *ibdev_name) {
    struct doca_dev *result = NULL;
    struct doca_devinfo **dev_list;
    uint32_t nb_devs;
    doca_error_t err;

    err = doca_devinfo_create_list(&dev_list, &nb_devs);

    if(err != DOCA_SUCCESS) {
        LOG("could not get device list: %s\n", doca_error_get_descr(err));
        return NULL;
    }

    for(uint32_t i = 0; i < nb_devs; ++i) {
        char buf[DOCA_DEVINFO_IBDEV_NAME_SIZE];

        err = doca_devinfo_get_ibdev_name(dev_list[i], buf, sizeof buf);
        if(err != DOCA_SUCCESS) {
            continue;
        }

        if(
            strcmp(ibdev_name, buf) == 0 &&
            doca_eth_rxq_cap_is_type_supported(
                dev_list[i],
                DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL,
                DOCA_ETH_RXQ_DATA_PATH_TYPE_CPU
            ) == DOCA_SUCCESS
        ) {
            err = doca_dev_open(dev_list[i], &result);

            if(err == DOCA_SUCCESS) {
                goto cleanup;
            } else {
                LOG("could not open device: %s\n", doca_error_get_descr(err));
            }
        }
    }

    LOG("no eth-capable device with ibdev name %s found: %s", ibdev_name, doca_error_get_descr(err));

cleanup:
    doca_devinfo_destroy_list(dev_list);
    return result;
}

struct doca_pe *open_progress_engine(int epoll_fd) {
    struct doca_pe *engine;
    doca_error_t err;

    err = doca_pe_create(&engine);
    if(err != DOCA_SUCCESS) {
        LOG("could not create progress engine: %s\n", doca_error_get_descr(err));
        goto failure;
    }

    doca_event_handle_t event_handle;
    err = doca_pe_get_notification_handle(engine, &event_handle);
    if(err != DOCA_SUCCESS) {
        LOG("could not obtain notification handle: %s\n", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    struct epoll_event events_in = { EPOLLIN, { .fd = event_handle }};
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_handle, &events_in) != 0) {
        LOG("could not attach to epoll handle: %s\n", strerror(errno));
        goto failure_cleanup;
    }

    return engine;

failure_cleanup:
    doca_pe_destroy(engine);
failure:
    return NULL;
}

struct doca_mmap *open_memory_map(uint8_t *base, size_t size, struct doca_dev *dev, uint32_t permissions) {
    struct doca_mmap *map;
    doca_error_t err;

    err = doca_mmap_create(&map);
    if(err != DOCA_SUCCESS) {
        LOG("could not create memory map: %s\n", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_mmap_set_memrange(map, base, size);
    if(err != DOCA_SUCCESS) {
        LOG("could not set memory range: %s\n", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_mmap_set_permissions(map, permissions);
    if(err != DOCA_SUCCESS) {
        LOG("could not set permissions: %s\n", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_mmap_add_dev(map, dev);
    if(err != DOCA_SUCCESS) {
        LOG("could not add device: %s\n", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_mmap_start(map);
    if(err != DOCA_SUCCESS) {
        LOG("could not start memory map: %s\n", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    return map;

failure_cleanup:
    doca_mmap_destroy(map);
failure:
    return NULL;
}

bool init_flow_library(void) {
    struct doca_flow_cfg *flow_lib;
    doca_error_t err;

    err = doca_flow_cfg_create(&flow_lib);
    if(err != DOCA_SUCCESS){
        LOG("Failed to create config object: %s", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_flow_cfg_set_pipe_queues(flow_lib, 1);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set pipe queues: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    err = doca_flow_cfg_set_mode_args(flow_lib, "vnf,hws,isolated");
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set mode: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }


    err = doca_flow_cfg_set_nr_counters(flow_lib, 1 << 19);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set counters: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }


    err = doca_flow_init(flow_lib);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to init flow library: %s", doca_error_get_descr(err));
        goto failure_cleanup;
    }

    doca_flow_cfg_destroy(flow_lib);
    return true;

failure_cleanup:
    doca_flow_cfg_destroy(flow_lib);
failure:
    return false;
}

struct doca_flow_port *open_ingress_port(uint16_t port_id, struct doca_dev *dev) {
    struct doca_flow_port_cfg *port_cfg;
    struct doca_flow_port *port = NULL;
    doca_error_t err;

    err = doca_flow_port_cfg_create(&port_cfg);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to create config object: %s", doca_error_get_descr(err));
        goto end;
    }

    err = doca_flow_port_cfg_set_port_id(port_cfg, port_id);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set port id: %s", doca_error_get_descr(err));
        goto cleanup;
    }

    err = doca_flow_port_cfg_set_dev(port_cfg, dev);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set device: %s", doca_error_get_descr(err));
        goto cleanup;
    }

    err = doca_flow_port_start(port_cfg, &port);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to start ingress port: %s", doca_error_get_descr(err));
        goto cleanup;
    }

cleanup:
    doca_flow_port_cfg_destroy(port_cfg);
end:
    return port;
}

static void eth_rxq_state_changed_callback(
    union doca_data user_data,
    struct doca_ctx *ctx,
    enum doca_ctx_states prev_state,
    enum doca_ctx_states next_state
) {
    (void) ctx;
    (void) prev_state;

    if(next_state == DOCA_CTX_STATE_RUNNING) {
        struct app_state *state = user_data.ptr;
        doca_error_t err;
        
        err = doca_eth_rxq_apply_queue_id(state->rss, RSS_QUEUE_ID);
        if(err != DOCA_SUCCESS) {
            LOG("Failed to apply queue id: %s", doca_error_get_descr(err));
            doca_ctx_stop(doca_eth_rxq_as_doca_ctx(state->rss));
        }
    }
}

struct doca_eth_rxq *open_eth_rxq_context(
    struct doca_dev *dev,
    struct doca_pe *engine,
    struct doca_mmap *packet_mmap,
    size_t packet_buffer_size,
    struct app_state *state,
    doca_eth_rxq_event_managed_recv_handler_cb_t recv_handler
) {
    struct doca_eth_rxq *eth_rxq = NULL;
    doca_error_t err;

    err = doca_eth_rxq_create(dev, 256, 1600, &eth_rxq);
    if(err != DOCA_SUCCESS)  {
        LOG("Failed to create context object: %s", doca_error_get_descr(err));
        goto error;
    }

    state->rss = eth_rxq;

    err = doca_eth_rxq_set_type(eth_rxq, DOCA_ETH_RXQ_TYPE_MANAGED_MEMPOOL);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set context type: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    err = doca_eth_rxq_set_pkt_buf(eth_rxq, packet_mmap, 0, packet_buffer_size);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to attach packet buffer: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    union doca_data event_user_data = { .ptr = state };
    err = doca_eth_rxq_event_managed_recv_register(eth_rxq, event_user_data, recv_handler, recv_handler);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to configure recv callback function: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    struct doca_ctx *base_ctx = doca_eth_rxq_as_doca_ctx(eth_rxq);
    err = doca_ctx_set_user_data(base_ctx, event_user_data);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to attach app state: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    err = doca_ctx_set_state_changed_cb(base_ctx, eth_rxq_state_changed_callback);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to configure state-changed callback function: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    err = doca_pe_connect_ctx(engine, base_ctx);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to connect context to progress engine: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    err = doca_ctx_start(base_ctx);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to start context: %s", doca_error_get_descr(err));
        goto cleanup_error;
    }

    return eth_rxq;

cleanup_error:
    doca_eth_rxq_destroy(eth_rxq);
    state->rss = NULL;
error:
    return NULL;
}

struct doca_flow_pipe *create_basic_pipe(
    struct doca_flow_port *ingress,
    char const *name,
    bool is_root,
    struct doca_flow_match *pipe_match,
    struct doca_flow_match *pipe_match_mask,
    struct doca_flow_fwd *fwd_on_success,
    struct doca_flow_fwd *fwd_on_failure
) {
    struct doca_flow_pipe_cfg *pipe_cfg;
    struct doca_flow_pipe *pipe = NULL;
    doca_error_t err;

    err = doca_flow_pipe_cfg_create(&pipe_cfg, ingress);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to create pipe configuration object: %s", doca_error_get_descr(err));
        goto failure;
    }

    err = doca_flow_pipe_cfg_set_name(pipe_cfg, name);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set pipe name: %s", doca_error_get_descr(err));
        goto cleanup_cfg;
    }

    err = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set pipe type: %s", doca_error_get_descr(err));
        goto cleanup_cfg;
    }

    err = doca_flow_pipe_cfg_set_is_root(pipe_cfg, is_root);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to mark pipe as root or non-root pipe: %s", doca_error_get_descr(err));
        goto cleanup_cfg;
    }

    err = doca_flow_pipe_cfg_set_match(pipe_cfg, pipe_match, pipe_match_mask);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to set pipe entry match criteria: %s", doca_error_get_descr(err));
        goto cleanup_cfg;
    }

    err = doca_flow_pipe_create(pipe_cfg, fwd_on_success, fwd_on_failure, &pipe);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to create pipe: %s", doca_error_get_descr(err));
        goto cleanup_cfg;
    }

cleanup_cfg:
    doca_flow_pipe_cfg_destroy(pipe_cfg);
failure:
    return pipe;
}

void dump_packet(struct doca_buf *pkt) {
    uint8_t const *raw;
    size_t len;
    doca_error_t err;

    err = doca_buf_get_data(pkt, (void**) &raw);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to get packet data while dumping: %s", doca_error_get_descr(err));
        return;
    }

    err = doca_buf_get_data_len(pkt, &len);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to get packet length while dumping: %s", doca_error_get_descr(err));
        return;
    }

    for(size_t i = 0; i < len; ++i) {
        printf("%02x", raw[i]);
        if(i % 16 == 15) {
            putchar('\n');
        } else if(i % 2 == 1) {
            putchar(' ');
        }
    }
    putchar('\n');    
}
