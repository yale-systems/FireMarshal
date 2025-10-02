// client_ts.c — RDMA ping client with NIC device-clock timestamps (no CQ-ex helpers)
// Works on older rdma-core/libibverbs where ibv_wc_read_* are missing.
//
// How it works:
// - Regular CQ + ibv_poll_cq for SEND/RECV completions
// - On each completion, read NIC clock with ibv_read_clock(ctx, &ns)
// - tx_hw_ns = device clock at SEND CQE observation
// - rx_hw_ns = device clock at RECV CQE observation
// - Prints per-iter timestamps and their difference (same device clock domain)
//
// Build: gcc -O2 -Wall client_ts.c -lrdmacm -libverbs -o client_ts

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

static void diex(const char *m) { fprintf(stderr, "%s: %s\n", m, strerror(errno)); exit(1); }

static inline uint64_t host_nanos(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

static int read_device_nanos(struct ibv_context *ctx, uint64_t *out) {
#ifdef IBV_DEVICE_CLOCK_INFO
    if (ibv_read_clock(ctx, out) == 0) return 0;
#endif
    return -1;
}

typedef struct {
    uint64_t idx;
    uint32_t size;
    uint64_t rx_ns;  // HW if available, else PHC now
    uint64_t tx_ns;  // PHC right after tx_burst(1)
} row_t;

static row_t  *g_rows = NULL;
static size_t  g_rows_sz = 0, g_rows_cap = 0;

static inline void rows_reserve(size_t need) {
    if (need <= g_rows_cap) return;
    size_t ncap = g_rows_cap ? g_rows_cap*2 : 16384;
    while (ncap < need) ncap *= 2;
    row_t *n = (row_t*)realloc(g_rows, ncap*sizeof(row_t));
    if (!n) { perror("realloc"); exit(2); }
    g_rows = n; g_rows_cap = ncap;
}
static inline void rows_push(row_t r) {
    if (g_rows_sz + 1 > g_rows_cap) rows_reserve(g_rows_sz + 1);
    g_rows[g_rows_sz++] = r;
}

static void write_csv_and_cleanup(uint32_t msg_size) {
    char fname[128];
    snprintf(fname, sizeof(fname), "out-rdma-%u.csv", msg_size);
    FILE *fp = fopen(fname, "w");
    if (!fp) { perror("fopen csv"); return; }
    fprintf(fp, "pkt_index,size,rx_time_ns,tx_time_ns,delta_ns\n");
    for (size_t i = 0; i < g_rows_sz; i++) {
        uint64_t d = (g_rows[i].rx_ns > g_rows[i].tx_ns) ? (g_rows[i].rx_ns - g_rows[i].tx_ns) : 0ULL;
        fprintf(fp, "%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                g_rows[i].idx, g_rows[i].size, g_rows[i].rx_ns, g_rows[i].tx_ns, d);
    }
    fclose(fp);
    printf("Wrote %zu rows to %s\n", g_rows_sz, fname);
}

int main(int argc, char **argv) {
    const char *server_ip = NULL;
    int port = 7471;
    int msg_size = 64;
    int iters = 10000;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:s:n:")) != -1) {
        if (opt == 'a') server_ip = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 's') msg_size = atoi(optarg);
        else if (opt == 'n') iters = atoi(optarg);
    }
    if (!server_ip) {
        fprintf(stderr, "Usage: %s -a <server_ip> [-p port] [-s size] [-n iters]\n", argv[0]);
        return 2;
    }

    uint64_t hw_rx_ts[iters];
    uint64_t hw_tx_ts[iters];

    // --- RDMA CM: resolve route/connect ---
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) diex("rdma_create_event_channel");

    struct rdma_cm_id *id = NULL;
    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) diex("rdma_create_id");

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) diex("inet_pton");

    if (rdma_resolve_addr(id, NULL, (struct sockaddr*)&dst, 2000)) diex("rdma_resolve_addr");
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev)) diex("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ADDR_RESOLVED) { fprintf(stderr, "Expected ADDR_RESOLVED\n"); exit(1); }
    rdma_ack_cm_event(ev);

    if (rdma_resolve_route(id, 2000)) diex("rdma_resolve_route");
    if (rdma_get_cm_event(ec, &ev)) diex("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ROUTE_RESOLVED) { fprintf(stderr, "Expected ROUTE_RESOLVED\n"); exit(1); }
    rdma_ack_cm_event(ev);

    // --- Verbs resources ---
    struct ibv_context *ctx = id->verbs;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) diex("ibv_alloc_pd");

    struct ibv_cq *cq = ibv_create_cq(ctx, 256, NULL, NULL, 0);
    if (!cq) diex("ibv_create_cq");

    struct ibv_qp_init_attr qp_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = { .max_send_wr = 128, .max_recv_wr = 128, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    if (rdma_create_qp(id, pd, &qp_attr)) diex("rdma_create_qp");

    // Buffers & MR
    void *buf = NULL;
    if (posix_memalign(&buf, 64, msg_size) != 0 || !buf) diex("posix_memalign");
    memset(buf, 0xab, msg_size);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, msg_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) diex("ibv_reg_mr");

    // Pre-post a RECV for echo
    struct ibv_sge rsge = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
    struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&rsge, .num_sge=1 }, *rbad = NULL;
    if (ibv_post_recv(id->qp, &rwr, &rbad)) diex("ibv_post_recv");

    struct rdma_conn_param cp = { .initiator_depth=1, .responder_resources=1, .retry_count=7 };
    if (rdma_connect(id, &cp)) diex("rdma_connect");

    if (rdma_get_cm_event(ec, &ev)) diex("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) { fprintf(stderr, "Expected ESTABLISHED\n"); exit(1); }
    rdma_ack_cm_event(ev);
    fprintf(stdout, "Connected. size=%d, iters=%d\n", msg_size, iters);

    // Check device clock support (optional)
    int have_dev_clock = 0;
#ifdef IBV_DEVICE_ATTR_EX_VERBS_CONTEXT
    // Older/newer headers differ; simplest is to just try ibv_read_clock once.
#endif
    {
        uint64_t tmp;
        if (read_device_nanos(ctx, &tmp) == 0) have_dev_clock = 1;
    }
    if (!have_dev_clock) {
        fprintf(stderr, "WARNING: ibv_read_clock() not available — falling back to host CLOCK_MONOTONIC_RAW.\n");
    }

    // Warmup
    for (int i = 0; i < 100; i++) {
        struct ibv_sge s = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
        struct ibv_send_wr sw = {0}, *sbad = NULL;
        sw.wr_id=2; sw.opcode=IBV_WR_SEND; sw.sg_list=&s; sw.num_sge=1; sw.send_flags=IBV_SEND_SIGNALED;
        if (ibv_post_send(id->qp, &sw, &sbad)) diex("ibv_post_send (warmup)");

        struct ibv_wc wc;
        // SEND completion
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_SEND) { fprintf(stderr,"Warmup SEND wc err %d\n", wc.status); exit(1); }
        // RECV completion
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) { fprintf(stderr,"Warmup RECV wc err %d\n", wc.status); exit(1); }

        // Repost recv
        if (ibv_post_recv(id->qp, &rwr, &rbad)) diex("ibv_post_recv (warmup)");
    }

    // Timed loop with device-clock timestamps
    uint64_t sum_ns = 0, min_ns = ~0ULL, max_ns = 0;

    // printf("# iter, tx_hw_ns, rx_hw_ns, rx_minus_tx_ns\n");

    for (int i = 0; i < iters; i++) {
        // SEND
        struct ibv_sge sge = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
        struct ibv_send_wr swr = {0}, *bad = NULL;
        swr.wr_id = 10;
        swr.opcode = IBV_WR_SEND;
        swr.sg_list = &sge;
        swr.num_sge = 1;
        swr.send_flags = IBV_SEND_SIGNALED;

        if (ibv_post_send(id->qp, &swr, &bad)) diex("ibv_post_send");

        // Wait for SEND completion, then read NIC clock
        struct ibv_wc wc;
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_SEND) { fprintf(stderr,"SEND wc err %d\n", wc.status); exit(1); }

        uint64_t tx_hw_ns = 0, rx_hw_ns = 0;
        if (have_dev_clock) read_device_nanos(ctx, &tx_hw_ns); else tx_hw_ns = host_nanos();

        // Wait for RECV completion, then read NIC clock
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) { fprintf(stderr,"RECV wc err %d\n", wc.status); exit(1); }

        if (have_dev_clock) read_device_nanos(ctx, &rx_hw_ns); else rx_hw_ns = host_nanos();

        // Device-clock RTT (ns). If fallback, it's host-clock RTT instead.
        uint64_t rtt = (rx_hw_ns >= tx_hw_ns) ? (rx_hw_ns - tx_hw_ns)
                                              : (uint64_t)(~0ULL) - (tx_hw_ns - rx_hw_ns) + 1ULL;

        if (rtt < min_ns) min_ns = rtt;
        if (rtt > max_ns) max_ns = rtt;
        sum_ns += rtt;

        // printf("%d, %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n", i, tx_hw_ns, rx_hw_ns, rtt);

        // Store row
        row_t r = { .idx = i, .size = msg_size, .rx_ns = rx_hw_ns, .tx_ns = tx_hw_ns };
        rows_push(r);

        // Repost RECV
        if (ibv_post_recv(id->qp, &rwr, &rbad)) diex("ibv_post_recv");
    }

    double avg_us = (double)sum_ns / iters / 1000.0;
    printf("RDMA ping (device clock): avg=%.3f us  min=%.3f us  max=%.3f us  (n=%d, size=%d)\n",
           avg_us, min_ns/1000.0, max_ns/1000.0, iters, msg_size);

    // Cleanup
    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(id);
    rdma_destroy_event_channel(ec);

    write_csv_and_cleanup(msg_size);
    free(g_rows);

    return 0;
}
