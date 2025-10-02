// client.c - RDMA ping client: sends SENDs, waits echo, measures RTT
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

static void die(const char *m) { perror(m); exit(1); }
static inline uint64_t nsec_now(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec;
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
    if (!server_ip) { fprintf(stderr, "Usage: %s -a <server_ip> [-p port] [-s size] [-n iters]\n", argv[0]); return 2; }

    // --- RDMA CM: resolve route/connect ---
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) die("rdma_create_event_channel");

    struct rdma_cm_id *id;
    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) die("rdma_create_id");

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) die("inet_pton");

    if (rdma_resolve_addr(id, NULL, (struct sockaddr*)&dst, 2000)) die("rdma_resolve_addr");
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev)) die("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ADDR_RESOLVED) die("ADDR_RESOLVED");
    rdma_ack_cm_event(ev);

    if (rdma_resolve_route(id, 2000)) die("rdma_resolve_route");
    if (rdma_get_cm_event(ec, &ev)) die("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ROUTE_RESOLVED) die("ROUTE_RESOLVED");
    rdma_ack_cm_event(ev);

    // --- Verbs resources ---
    struct ibv_context *ctx = id->verbs;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) die("ibv_alloc_pd");
    struct ibv_cq *cq = ibv_create_cq(ctx, 256, NULL, NULL, 0);
    if (!cq) die("ibv_create_cq");

    struct ibv_qp_init_attr qp_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = { .max_send_wr = 128, .max_recv_wr = 128, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    if (rdma_create_qp(id, pd, &qp_attr)) die("rdma_create_qp");

    // Buffers & MR
    void *buf = aligned_alloc(64, msg_size);
    if (!buf) die("aligned_alloc");
    memset(buf, 0xab, msg_size);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, msg_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) die("ibv_reg_mr");

    // Pre-post a receive for echo
    struct ibv_sge rsge = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
    struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&rsge, .num_sge=1 }, *rbad;
    if (ibv_post_recv(id->qp, &rwr, &rbad)) die("ibv_post_recv");

    struct rdma_conn_param cp = { .initiator_depth=1, .responder_resources=1, .retry_count=7 };
    if (rdma_connect(id, &cp)) die("rdma_connect");

    if (rdma_get_cm_event(ec, &ev)) die("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) die("ESTABLISHED");
    rdma_ack_cm_event(ev);
    fprintf(stdout, "Connected. size=%d, iters=%d\n", msg_size, iters);

    // Warmup
    for (int i = 0; i < 100; i++) {
        struct ibv_sge s = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
        struct ibv_send_wr sw = {0}, *sbad;
        sw.wr_id=2; sw.opcode=IBV_WR_SEND; sw.sg_list=&s; sw.num_sge=1; sw.send_flags=IBV_SEND_SIGNALED;
        if (ibv_post_send(id->qp, &sw, &sbad)) die("ibv_post_send");
        struct ibv_wc wc;
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS) die("warmup SEND wc");
        // wait echo recv
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) die("warmup RECV wc");
        // repost recv
        if (ibv_post_recv(id->qp, &rwr, &rbad)) die("ibv_post_recv");
    }

    // Timed loop
    uint64_t sum_ns = 0, min_ns = UINT64_MAX, max_ns = 0;
    for (int i = 0; i < iters; i++) {
        // SEND
        struct ibv_sge sge = { .addr=(uintptr_t)buf, .length=msg_size, .lkey=mr->lkey };
        struct ibv_send_wr swr = {0}, *bad;
        swr.wr_id = 10;
        swr.opcode = IBV_WR_SEND;
        swr.sg_list = &sge;
        swr.num_sge = 1;
        swr.send_flags = IBV_SEND_SIGNALED;

        uint64_t t0 = nsec_now();
        if (ibv_post_send(id->qp, &swr, &bad)) die("ibv_post_send");

        // Wait for SEND completion (optional but keeps pacing tidy)
        struct ibv_wc wc;
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS) die("SEND wc");

        // Wait for echo RECV
        while (ibv_poll_cq(cq, 1, &wc) == 0) {}
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) die("RECV wc");
        uint64_t t1 = nsec_now();

        uint64_t rtt = t1 - t0;
        sum_ns += rtt;
        if (rtt < min_ns) min_ns = rtt;
        if (rtt > max_ns) max_ns = rtt;

        // repost recv for next round
        if (ibv_post_recv(id->qp, &rwr, &rbad)) die("ibv_post_recv");
    }

    double avg_us = (double)sum_ns / iters / 1000.0;
    printf("RDMA SEND/RECV ping-pong: avg=%.3f us  min=%.3f us  max=%.3f us  (n=%d, size=%d)\n",
           avg_us, min_ns/1000.0, max_ns/1000.0, iters, msg_size);

    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(id);
    rdma_destroy_event_channel(ec);
    return 0;
}
