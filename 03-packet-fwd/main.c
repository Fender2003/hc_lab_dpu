#include "task3.h"
#include "eth_frame.h"

#include <doca_log.h>

void eth_rxq_managed_recv_callback(
    struct doca_eth_rxq_event_managed_recv *event,
    struct doca_buf *pkt,
    union doca_data user_data
) {
    // TODO: Process received packet.
}

struct doca_flow_pipe *create_fwd_pipe(struct doca_flow_port *ingress, struct doca_flow_pipe *rss_pipe) {
    struct doca_flow_match pipe_match = {};
    memset(pipe_match.outer.eth.src_mac, 0xff, 6);
    memset(pipe_match.outer.eth.dst_mac, 0xff, 6);
    pipe_match.outer.ip4.src_ip = 0xffffffff;
    pipe_match.outer.ip4.dst_ip = 0xffffffff;
    pipe_match.outer.udp.l4_port.src_port = 0xffff;

    struct doca_flow_actions pipe_actions = {};
    // set source/dest mac, ip address, and port changeable
    memset(pipe_actions.outer.eth.src_mac, 0xff, 6);
    memset(pipe_actions.outer.eth.dst_mac, 0xff, 6);
    pipe_actions.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    pipe_actions.outer.ip4.src_ip = 0xffffffff;
    pipe_actions.outer.ip4.dst_ip = 0xffffffff;
    pipe_actions.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
    pipe_actions.outer.udp.l4_port.src_port = 0xffff;
    pipe_actions.outer.udp.l4_port.dst_port = 0xffff;

    struct doca_flow_actions *pipe_actions_index[] = { &pipe_actions };

    struct doca_flow_fwd fwd_changeable = {
        .type = DOCA_FLOW_FWD_CHANGEABLE
    };

    struct doca_flow_fwd fwd_rss = {
        .type = DOCA_FLOW_FWD_PIPE,
        .next_pipe = rss_pipe
    };

    struct doca_flow_pipe *fwd_pipe = create_basic_pipe(
        ingress, "FWD_PIPE", false,
        &pipe_match,
        pipe_actions_index, ARRAY_SIZE(pipe_actions_index),
        &fwd_changeable, &fwd_rss);

    if(fwd_pipe == NULL) {
        LOG("Failed to create forwarding pipe");
    }

    return fwd_pipe;
    // SOLUTION_END
}

struct doca_flow_pipe *setup_udp_splitter(struct doca_flow_port *ingress, struct doca_flow_pipe *fwd_pipe) {
    doca_error_t err;

    struct doca_flow_match udp_match = {};
    udp_match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
    udp_match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    udp_match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
    udp_match.outer.udp.l4_port.dst_port = htobe16(UDP_PORT);

    struct doca_flow_target *kernel_target;
    err = doca_flow_get_target(DOCA_FLOW_TARGET_KERNEL, &kernel_target);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to retrieve kernel target: %s", doca_error_get_descr(err));
        goto failure;
    }

    struct doca_flow_fwd fwd_fwd = {
        .type = DOCA_FLOW_FWD_PIPE,
        .next_pipe = fwd_pipe
    };

    struct doca_flow_fwd fwd_kernel = {
        .type = DOCA_FLOW_FWD_TARGET,
        .target = kernel_target
    };

    return create_simple_splitter_pipe(ingress, "UDP_SPLITTER", false, &udp_match,&fwd_fwd, &fwd_kernel);

failure:
    return NULL;
}

struct doca_flow_pipe *setup_rss_pipe(struct doca_flow_port *ingress) {
    uint16_t rxq_queue_id = RSS_QUEUE_ID;
    struct doca_flow_fwd fwd_rss = {
        .type = DOCA_FLOW_FWD_RSS,
        .rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED,
        .rss = {
            .outer_flags = 0,
            .inner_flags = 0,
            .queues_array = &rxq_queue_id,
            .nr_queues = 1,
            .rss_hash_func = DOCA_FLOW_RSS_HASH_FUNCTION_TOEPLITZ
        }
    };

    struct doca_flow_fwd fwd_drop = {
        .type = DOCA_FLOW_FWD_DROP
    };

    struct doca_flow_match all_match = {};

    return create_simple_splitter_pipe(ingress, "RSS_PIPE", false, &all_match, &fwd_rss, &fwd_drop);
}

struct doca_flow_pipe *setup_root_pipe(struct doca_flow_port *ingress, struct doca_flow_pipe *splitter_pipe) {
    struct doca_flow_match all_match = {};

    struct doca_flow_fwd fwd = {
        .type = DOCA_FLOW_FWD_PIPE,
        .next_pipe = splitter_pipe
    };

    struct doca_flow_fwd fwd_miss = {
        .type = DOCA_FLOW_FWD_DROP
    };

    return create_simple_splitter_pipe(ingress, "ROOT_PIPE", true, &all_match, &fwd, &fwd_miss);
}

int main(void) {
    struct doca_log_backend *sdk_log;
    doca_error_t err;

    struct app_state app_state = {
        .rss = NULL,
        .ingress = NULL,
        .fwd_pipe = NULL,
        .shutting_down = false
    };

    doca_log_backend_create_standard();
    doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);

    init_flow_library();

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if(epoll_fd == -1) {
        LOG("could not create epoll file descriptor: %s\n", strerror(errno));
        goto cleanup_flow;
    }

    struct doca_pe *engine = open_progress_engine(epoll_fd);
    if(engine == NULL) {
        LOG("could not obtain progress engine");
        goto cleanup_epoll;
    }

    struct epoll_event ev_stdin = { EPOLLIN, { .fd = STDIN_FILENO }};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &ev_stdin) == -1)
    {
        LOG("Could not attach stdin to epoll fd");
        goto cleanup_pe;
    }

    struct doca_dev *in_dev = open_device("mlx5_2");
    if(in_dev == NULL) {
        LOG("could not open input device");
        goto cleanup_pe;
    }

    struct doca_dev *out_dev = open_device("mlx5_3");
    if(out_dev == NULL) {
        LOG("could not open output device");
        goto cleanup_in_dev;
    }

    // allocate packet buffer, align to cache line granularity (64 B)
    size_t packet_buffer_size = 8 << 20;
    void *packet_memory = malloc(packet_buffer_size + 63);
    void *packet_buffer = (void*)(((uintptr_t) packet_memory + 63) / 64 * 64);

    if(packet_memory == NULL) {
        LOG("could allocate packet buffer");
        goto cleanup_out_dev;
    }

    struct doca_mmap *packet_mmap = open_memory_map(packet_buffer, packet_buffer_size, in_dev, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if(packet_mmap == NULL) {
        LOG("could not obtain memory map for packet buffer");
        goto cleanup_packet_buffer;
    }    

    struct doca_eth_rxq *rss = open_eth_rxq_context(in_dev, engine, packet_mmap, packet_buffer_size, &app_state, eth_rxq_managed_recv_callback);
    if(rss == NULL) {
        LOG("could not obtain RSS context");
        goto cleanup_mmap;
    }

    struct doca_flow_port *ingress = open_flow_port(INGRESS_PORT_ID, in_dev, 8192);
    if(ingress == NULL) {
        LOG("could not open ingress port");
        goto cleanup_rss;
    }
    app_state.ingress = ingress;

    struct doca_flow_port *egress = open_flow_port(EGRESS_PORT_ID, out_dev, 8192);
    if(egress == NULL) {
        LOG("could not open egress port");
        goto cleanup_ingress;
    }

    err = doca_flow_port_pair(ingress, egress);
    if(err != DOCA_SUCCESS) {
        LOG("Could not pair ingress and egress: %s", doca_error_get_descr(err));
        goto cleanup_egress;
    }

    struct doca_flow_pipe *rss_pipe = setup_rss_pipe(ingress);
    if(rss_pipe == NULL) {
        LOG("Failed to create RSS pipe");
        goto cleanup_egress;
    }

    struct doca_flow_pipe *fwd_pipe = create_fwd_pipe(ingress, rss_pipe);
    if(fwd_pipe == NULL) {
        LOG("Failed to create fwd pipe");
        goto cleanup_egress;
    }
    app_state.fwd_pipe = fwd_pipe;

    struct doca_flow_pipe *udp_splitter_pipe = setup_udp_splitter(ingress, fwd_pipe);
    if(udp_splitter_pipe == NULL) {
        LOG("Failed to set up UDP splitter");
        goto cleanup_egress;
    }

    struct doca_flow_pipe *root_pipe = setup_root_pipe(ingress, udp_splitter_pipe);
    if(root_pipe == NULL) {
        LOG("Failed to create root pipe");
        goto cleanup_egress;
    }

    err = doca_flow_entries_process(ingress, PIPE_QUEUE_ID, 10000, 10);
    if(err != DOCA_SUCCESS) {
        LOG("Failed to process entries for the flow pipes: %s", doca_error_get_descr(err));
        goto cleanup_egress;
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

        nfd = epoll_wait(epoll_fd, &ep_event, 1, -1);

        if(nfd == -1) {
            LOG("epoll_wait failed: %s\n", strerror(errno));
            exit(-1);
        }
        
        if(nfd > 0 && ep_event.data.fd == STDIN_FILENO) {
            LOG("stopping");

            app_state.shutting_down = true;
            doca_flow_port_stop(ingress);
            doca_flow_port_stop(egress);
            doca_ctx_stop(doca_eth_rxq_as_doca_ctx(rss));
        } else {
            LOG("processing events");

            doca_pe_clear_notification(engine, 0);
            while(doca_pe_progress(engine) > 0) {
                // do nothing; doca_pe_progress calls event handlers
            }
        }
    }

cleanup_egress:
    if(!app_state.shutting_down) {
        doca_flow_port_stop(egress);
    }
cleanup_ingress:
    if(!app_state.shutting_down) {
        doca_flow_port_stop(ingress);
    }
cleanup_rss:
    doca_eth_rxq_destroy(rss);
cleanup_mmap:
    doca_mmap_destroy(packet_mmap);
cleanup_packet_buffer:
    free(packet_memory);
cleanup_out_dev:
    doca_dev_close(out_dev);
cleanup_in_dev:
    doca_dev_close(in_dev);
cleanup_pe:
    doca_pe_destroy(engine);
cleanup_epoll:
    close(epoll_fd);
cleanup_flow:
    doca_flow_destroy();
}
