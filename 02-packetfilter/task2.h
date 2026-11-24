#pragma once

#include <stdint.h>
#include <stdio.h>

#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_flow.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include <sys/epoll.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LOG(fmt, ...) fprintf(stderr, "[%s] " fmt "\n", __func__ __VA_OPT__(,) __VA_ARGS__)

#define PIPE_QUEUE_ID 0
#define INGRESS_PORT_ID 0
#define RSS_QUEUE_ID 0

struct app_state {
    struct doca_eth_rxq *rss;
    struct doca_flow_port *ingress;

    // TODO: Extend as necessary
};

struct doca_dev *open_device(char const *ibdev_name);

struct doca_pe *open_progress_engine(int epoll_fd);
struct doca_mmap *open_memory_map(uint8_t *base, size_t size, struct doca_dev *dev, uint32_t permissions);
bool init_flow_library(void);
struct doca_flow_port *open_ingress_port(uint16_t port_id, struct doca_dev *dev);
struct doca_eth_rxq *open_eth_rxq_context(
    struct doca_dev *dev,
    struct doca_pe *engine,
    struct doca_mmap *packet_mmap,
    size_t packet_buffer_size,
    struct app_state *state,
    doca_eth_rxq_event_managed_recv_handler_cb_t recv_handler
);

struct doca_flow_pipe *create_basic_pipe(
    struct doca_flow_port *ingress,
    char const *name,
    bool is_root,
    struct doca_flow_match *pipe_match,
    struct doca_flow_match *pipe_match_mask,
    struct doca_flow_fwd *fwd_on_success,
    struct doca_flow_fwd *fwd_on_failure
);

void dump_packet(struct doca_buf *pkt);
