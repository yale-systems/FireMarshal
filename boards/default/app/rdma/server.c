// server.c - RDMA "echo" server using SEND/RECV (RC QP)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

static void die(const char *m) { perror(m); exit(1); }

int main(int argc, char **argv) {
    const char *bind_ip = NULL;
    int port = 7471;
    int msg_size = 64;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:s:")) != -1) {
        if (opt == 'a') bind_ip = optarg;
        else if (opt == 'p') port = atoi(optarg);
        else if (opt == 's') msg_size = atoi(optarg);
    }
    if (!bind_ip) { fprintf(stderr, "Usage: %s -a <bind_ip> [-p port] [-s size]\n", argv[0]); return 2; }

    // --- RDMA CM: resolve and listen ---
    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) die("rdma_create_event_channel");

    struct rdma_cm_id *listen_id;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) die("rdma_create_id");

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) die("inet_pton");

    if (rdma_bind_addr(listen_id, (struct sockaddr*)&addr)) die("rdma_bind_addr");
    if (rdma_listen(listen_id, 1)) die("rdma_listen");

    fprintf(stdout, "Server listening on %s:%d (size=%d)\n", bind_ip, port, msg_size);

    // --- Accept an incoming connection ---
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev)) die("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_CONNECT_REQUEST) { fprintf(stderr,"Unexpected CM event %d\n", ev->event); exit(1); }

    struct rdma_cm_id *id = ev->id;
    struct ibv_context *ctx = id->verbs;
    rdma_ack_cm_event(ev);

    // --- Verbs resources ---
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

    // --- Buffers & MR ---
    void *rxbuf = aligned_alloc(64, msg_size);
    if (!rxbuf) die("aligned_alloc");
    memset(rxbuf, 0, msg_size);
    struct ibv_mr *mr = ibv_reg_mr(pd, rxbuf, msg_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) die("ibv_reg_mr");

    // post a few receives up front
    for (int i = 0; i < 64; i++) {
        struct ibv_sge sge = { .addr=(uintptr_t)rxbuf, .length=msg_size, .lkey=mr->lkey };
        struct ibv_recv_wr wr = { .wr_id=1, .sg_list=&sge, .num_sge=1 }, *bad;
        if (ibv_post_recv(id->qp, &wr, &bad)) die("ibv_post_recv");
    }

    // --- Accept ---
    struct rdma_conn_param cp = { .initiator_depth=1, .responder_resources=1, .rnr_retry_count=7 };
    if (rdma_accept(id, &cp)) die("rdma_accept");

    if (rdma_get_cm_event(ec, &ev)) die("rdma_get_cm_event");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) die("ESTABLISHED");
    rdma_ack_cm_event(ev);
    fprintf(stdout, "Connection established.\n");

    // --- Loop: receive one, echo it back ---
    while (1) {
        struct ibv_wc wc;
        int n;
        do { n = ibv_poll_cq(cq, 1, &wc); } while (n == 0);
        if (n < 0) die("ibv_poll_cq");
        if (wc.status != IBV_WC_SUCCESS) { fprintf(stderr,"WC error %d\n", wc.status); break; }

        if (wc.opcode == IBV_WC_RECV) {
            // echo the same bytes back using a SEND
            struct ibv_sge sge = { .addr=(uintptr_t)rxbuf, .length=msg_size, .lkey=mr->lkey };
            struct ibv_send_wr swr = {0}, *bad;
            swr.wr_id = 2;
            swr.opcode = IBV_WR_SEND;
            swr.sg_list = &sge;
            swr.num_sge = 1;
            swr.send_flags = IBV_SEND_SIGNALED;
            if (ibv_post_send(id->qp, &swr, &bad)) die("ibv_post_send");
        } else if (wc.opcode == IBV_WC_SEND) {
            // after echo, repost recv to keep pipeline full
            struct ibv_sge sge = { .addr=(uintptr_t)rxbuf, .length=msg_size, .lkey=mr->lkey };
            struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&sge, .num_sge=1 }, *bad;
            if (ibv_post_recv(id->qp, &rwr, &bad)) die("ibv_post_recv");
        }
    }

    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(rxbuf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(id);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    return 0;
}
