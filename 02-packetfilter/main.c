#include "eth_frame.h"
#include "task2.h"

void eth_rxq_managed_recv_callback(
    struct doca_eth_rxq_event_managed_recv *event,
    struct doca_buf *pkt,
    union doca_data user_data
) {
    // TODO: Process received packet.
}

struct doca_flow_pipe *setup_splitter_pipe(struct doca_flow_port *ingress) {
    // TODO: Create pipe to filter for payload packets. Direct payload packets to RSS and
    //       other packets to the OS kernel.
    return NULL;
}

struct doca_flow_pipe *setup_root_pipe(struct doca_flow_port *ingress, struct doca_flow_pipe *filter_pipe) {
    doca_error_t err;

    struct doca_flow_match all_match = {};

    struct doca_flow_fwd fwd_to_filter = {
        .type = DOCA_FLOW_FWD_PIPE,
        .next_pipe = filter_pipe
    };

    struct doca_flow_fwd fwd_drop = {
        .type = DOCA_FLOW_FWD_DROP
    };

    struct doca_flow_pipe *root_pipe = create_basic_pipe(ingress, "ROOT_PIPE", true, &all_match, NULL, &fwd_to_filter, &fwd_drop);
    if(root_pipe == NULL) {
        LOG("Failed to create root pipe");
        goto failure;
    }

    err = doca_flow_pipe_basic_add_entry(PIPE_QUEUE_ID, root_pipe, &all_match, 0, NULL, NULL, &fwd_to_filter, 0, NULL, NULL);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to create root pipe entry: %s", doca_error_get_descr(err));
        goto cleanup_pipe;
    }

    return root_pipe;

cleanup_pipe:
    doca_flow_pipe_destroy(root_pipe);
failure:
    return NULL;
}

int main(void) {
    struct doca_log_backend *sdk_log;
    doca_error_t err;

    doca_log_backend_create_standard();
    doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if(epoll_fd == -1) {
        LOG("could not create epoll file descriptor: %s\n", strerror(errno));
        goto end;
    }

    struct doca_pe *engine = open_progress_engine(epoll_fd);
    if(engine == NULL) {
        LOG("could not obtain progress engine");
        goto cleanup_epoll;
    }

    struct doca_dev *eth_dev = open_device("mlx5_2");
    if(eth_dev == NULL) {
        LOG("could not obtain ethernet device");
        goto cleanup_pe;
    }

    // allocate packet buffer, align to cache line granularity (64 B)
    size_t packet_buffer_size = 8 << 20;
    void *packet_memory = malloc(packet_buffer_size + 63);
    void *packet_buffer = (void*)(((uintptr_t) packet_memory + 63) / 64 * 64);

    if(packet_memory == NULL) {
        LOG("could allocate packet buffer");
        goto cleanup_dev;
    }

    struct doca_mmap *packet_mmap = open_memory_map(packet_buffer, packet_buffer_size, eth_dev, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if(packet_mmap == NULL) {
        LOG("could not obtain memory map for packet buffer");
        goto cleanup_packet_buffer;
    }

    struct app_state app_state = {
        .rss = NULL,
        // TODO: Extend as necessary
    };

    struct doca_eth_rxq *rss = open_eth_rxq_context(eth_dev, engine, packet_mmap, packet_buffer_size, &app_state, eth_rxq_managed_recv_callback);
    if(rss == NULL) {
        LOG("could not obtain RSS context");
        goto cleanup_mmap;
    }
    app_state.rss = rss;

    if(!init_flow_library()) {
        LOG("could not initialize DOCA Flow library");
        goto cleanup_rss;
    }

    struct doca_flow_port *ingress = open_ingress_port(INGRESS_PORT_ID, eth_dev);
    if(ingress == NULL) {
        LOG("could not open ingress port");
        goto cleanup_flow;
    }
    app_state.ingress = ingress;

    struct doca_flow_pipe *filter_pipe = setup_splitter_pipe(ingress);
    if(filter_pipe == NULL) {
        LOG("could not create filter pipe");
        goto cleanup_ingress;
    }

    struct doca_flow_pipe *root_pipe = setup_root_pipe(ingress, filter_pipe);
    if(root_pipe == NULL) {
        LOG("could not create root pipe");
        goto cleanup_ingress;
    }

    err = doca_flow_entries_process(ingress, PIPE_QUEUE_ID, 10000, 10);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to process entries for the flow pipes: %s", doca_error_get_descr(err));
        goto cleanup_ingress;
    }

    struct epoll_event ep_event = { 0, { 0 } };
    int nfd;

    for(;;) {
        enum doca_ctx_states ctx_state;

        err = doca_ctx_get_state(doca_eth_rxq_as_doca_ctx(rss), &ctx_state);
        if(err != DOCA_SUCCESS) {
            LOG("could not obtain context state: %s\n", doca_error_get_descr(err));
            break;
        }

        if(ctx_state == DOCA_CTX_STATE_IDLE) {
            // regular loop condition
            break;
        }

        doca_pe_request_notification(engine);
        nfd = epoll_wait(epoll_fd, &ep_event, 1, 100);

        if(nfd == -1) {
            LOG("epoll_wait failed: %s\n", strerror(errno));
            goto cleanup_rss;
        }

        doca_pe_clear_notification(engine, 0);
        while(doca_pe_progress(engine) > 0) {
            // do nothing; doca_pe_progress calls event handlers
        }
    }

cleanup_ingress:
// TODO: Clean up ingress port if necessary. How do you know if it's necessary here?
//       Hint: You can extend the app_state to communicate what has happened elsewhere.
    doca_flow_port_stop(ingress);
cleanup_flow:
    doca_flow_destroy();
cleanup_rss:
    doca_eth_rxq_destroy(rss);
cleanup_mmap:
    doca_mmap_destroy(packet_mmap);
cleanup_packet_buffer:
    free(packet_memory);
cleanup_dev:
    doca_dev_close(eth_dev);
cleanup_pe:
    doca_pe_destroy(engine);
cleanup_epoll:
    close(epoll_fd);
end:
}
