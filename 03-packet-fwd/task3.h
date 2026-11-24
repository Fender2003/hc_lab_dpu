#pragma once

#include <stdint.h>
#include <stdio.h>

#include <endian.h>

#include <doca_buf.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_flow.h>
#include <doca_mmap.h>
#include <doca_types.h>

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof (arr)[0])
#define LOG(fmt, ...) do { \
    fprintf(stderr, "[%s] " fmt "\n", __func__ __VA_OPT__(,) __VA_ARGS__); \
    fflush(stderr); \
} while(false)

#define PIPE_QUEUE_ID 0
#define RSS_QUEUE_ID 0
#define INGRESS_PORT_ID 0
#define EGRESS_PORT_ID 1
#define UDP_PORT 1234

static inline doca_be32_t be_ipv4_addr(
    uint32_t a,
    uint32_t b,
    uint32_t c,
    uint32_t d
) {
    return htobe32((a << 24) | (b << 16) | (c << 8) | d);
}

struct app_state {
    struct doca_eth_rxq *rss;
    struct doca_flow_port *ingress;
    struct doca_flow_pipe *fwd_pipe;
    bool shutting_down;
};

struct doca_pe *open_progress_engine(int epoll_fd);
struct doca_mmap *open_memory_map(uint8_t *base, size_t size, struct doca_dev *dev, uint32_t permissions);
struct doca_dev *open_device(char const *ibdev_name);

struct doca_eth_rxq *open_eth_rxq_context(
    struct doca_dev *dev,
    struct doca_pe *engine,
    struct doca_mmap *packet_mmap,
    size_t packet_buffer_size,
    struct app_state *state,
    doca_eth_rxq_event_managed_recv_handler_cb_t recv_handler
);

bool init_flow_library(void);
struct doca_flow_port *open_flow_port(uint16_t port_id, struct doca_dev *dev, uint32_t actions_mem_size);

struct doca_flow_pipe *create_basic_pipe(
    struct doca_flow_port *ingress,
    char const *name,
    bool is_root,
    struct doca_flow_match *pipe_match,
    struct doca_flow_actions *const *actions,
    size_t nr_actions,
    struct doca_flow_fwd *fwd_on_success,
    struct doca_flow_fwd *fwd_on_failure
);

struct doca_flow_pipe *create_simple_splitter_pipe(
    struct doca_flow_port *ingress,
    char const *name,
    bool is_root,
    struct doca_flow_match *pipe_match,
    struct doca_flow_fwd *fwd_on_success,
    struct doca_flow_fwd *fwd_on_failure
);
