// server.c - RDMA "echo" server (RC QP) that logs RX/TX timestamps and writes CSV on Ctrl-C
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <time.h>

static void die(const char *m) { perror(m); exit(1); }

/* ---------- signal handling ---------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static void install_sig_handlers(void) {
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ---------- device/host clock helpers ---------- */
static inline uint64_t host_nanos(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
// put near your includes:
#include <dlfcn.h>

// Replace your read_device_nanos() with this version:
static int read_device_nanos(struct ibv_context *ctx, uint64_t *out) {
    // 1) Try ibv_read_clock if present at runtime
    typedef int (*ibv_read_clock_fn)(struct ibv_context*, uint64_t*);
    static ibv_read_clock_fn p_ibv_read_clock = NULL;
    static int tried_ibv = 0;
    if (!tried_ibv) {
        p_ibv_read_clock = (ibv_read_clock_fn)dlsym(RTLD_DEFAULT, "ibv_read_clock");
        tried_ibv = 1;
    }
    if (p_ibv_read_clock) {
        if (p_ibv_read_clock(ctx, out) == 0) return 0;
    }

    // 2) Try mlx5dv_read_clock (Mellanox provider)
    typedef int (*mlx5dv_read_clock_fn)(struct ibv_context*, uint64_t*);
    static mlx5dv_read_clock_fn p_mlx5dv_read_clock = NULL;
    static int tried_mlx5 = 0;
    if (!tried_mlx5) {
        p_mlx5dv_read_clock = (mlx5dv_read_clock_fn)dlsym(RTLD_DEFAULT, "mlx5dv_read_clock");
        tried_mlx5 = 1;
    }
    if (p_mlx5dv_read_clock) {
        if (p_mlx5dv_read_clock(ctx, out) == 0) return 0;
    }

    // 3) No device clock available
    return -1;
}


/* ---------- row storage & CSV ---------- */
typedef struct {
    uint64_t idx;
    uint32_t size;
    uint64_t rx_ns;
    uint64_t tx_ns;
} row_t;

static row_t  *g_rows = NULL;
static size_t  g_rows_sz = 0, g_rows_cap = 0;

static void rows_reserve(size_t need) {
    if (need <= g_rows_cap) return;
    size_t ncap = g_rows_cap ? g_rows_cap * 2 : 16384;
    while (ncap < need) ncap *= 2;
    row_t *n = (row_t*)realloc(g_rows, ncap * sizeof(row_t));
    if (!n) { perror("realloc rows"); exit(2); }
    g_rows = n; g_rows_cap = ncap;
}
static inline void rows_push(row_t r) {
    if (g_rows_sz + 1 > g_rows_cap) rows_reserve(g_rows_sz + 1);
    g_rows[g_rows_sz++] = r;
}
static void write_csv(uint32_t msg_size, int port) {
    char fname[128];
    snprintf(fname, sizeof(fname), "out-rdma-server-%u-%u.csv", msg_size, port);
    FILE *fp = fopen(fname, "w");
    if (!fp) { perror("fopen csv"); return; }
    fprintf(fp, "pkt_index,size,rx_time_ns,tx_time_ns,delta_ns\n");
    for (size_t i = 0; i < g_rows_sz; i++) {
        uint64_t d = (g_rows[i].tx_ns >= g_rows[i].rx_ns) ? (g_rows[i].tx_ns - g_rows[i].rx_ns) : 0ULL;
        fprintf(fp, "%" PRIu64 ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                g_rows[i].idx, g_rows[i].size, g_rows[i].rx_ns, g_rows[i].tx_ns, d);
    }
    fclose(fp);
    printf("Wrote %zu rows to %s\n", g_rows_sz, fname);
}

/* ---------- simple FIFO for RX timestamps to pair with SEND CQEs ---------- */
static uint64_t *rxq = NULL;
static size_t rxq_cap = 0, rxq_head = 0, rxq_size = 0;

static void rxq_grow_and_compact(void) {
    size_t newcap = rxq_cap ? rxq_cap * 2 : 4096;
    uint64_t *n = (uint64_t*)malloc(newcap * sizeof(uint64_t));
    if (!n) { perror("malloc rxq"); exit(2); }
    // compact live window to start
    for (size_t i = 0; i < rxq_size; i++) n[i] = rxq[rxq_head + i];
    free(rxq);
    rxq = n; rxq_cap = newcap; rxq_head = 0;
}
static inline void rxq_push(uint64_t ts) {
    if (!rxq_cap || rxq_head + rxq_size >= rxq_cap) rxq_grow_and_compact();
    rxq[rxq_head + rxq_size] = ts;
    rxq_size++;
}
static inline int rxq_pop(uint64_t *out) {
    if (rxq_size == 0) return 0;
    *out = rxq[rxq_head++];
    rxq_size--;
    // optional compact to avoid head creeping forever
    if (rxq_head > 65536 && rxq_size < (rxq_cap / 4)) rxq_grow_and_compact();
    return 1;
}

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

    install_sig_handlers();

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
    struct ibv_cq *cq = ibv_create_cq(ctx, 512, NULL, NULL, 0);
    if (!cq) die("ibv_create_cq");

    struct ibv_qp_init_attr qp_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = { .max_send_wr = 256, .max_recv_wr = 256, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    if (rdma_create_qp(id, pd, &qp_attr)) die("rdma_create_qp");

    // --- Buffers & MR ---
    void *rxbuf = aligned_alloc(64, msg_size);
    if (!rxbuf) die("aligned_alloc");
    memset(rxbuf, 0, msg_size);
    struct ibv_mr *mr = ibv_reg_mr(pd, rxbuf, msg_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) die("ibv_reg_mr");

    // Prepost a bunch of receives
    for (int i = 0; i < 256; i++) {
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
    // fprintf(stdout, "Connection established. Press Ctrl-C to stop and dump CSV.\n");

    // For timestamps
    int have_dev_clock = 0; {
        uint64_t tmp;
        if (read_device_nanos(ctx, &tmp) == 0) have_dev_clock = 1;
        if (!have_dev_clock) fprintf(stderr, "WARNING: ibv_read_clock not available; using host clock.\n");
    }

    uint64_t pkt_idx = 0;

    // --- Poll loop ---
    while (!g_stop) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) die("ibv_poll_cq");
        if (n == 0) continue;

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "WC error %d\n", wc.status);
            break;
        }

        if (wc.opcode == IBV_WC_RECV) {
            // Timestamp RX immediately
            uint64_t rx_ns = 0;
            if (have_dev_clock) read_device_nanos(ctx, &rx_ns); else rx_ns = host_nanos();
            rxq_push(rx_ns);

            // Echo back same bytes
            struct ibv_sge sge = { .addr=(uintptr_t)rxbuf, .length=msg_size, .lkey=mr->lkey };
            struct ibv_send_wr swr = {0}, *bad;
            swr.wr_id = 2;
            swr.opcode = IBV_WR_SEND;
            swr.sg_list = &sge;
            swr.num_sge = 1;
            swr.send_flags = IBV_SEND_SIGNALED;
            if (ibv_post_send(id->qp, &swr, &bad)) die("ibv_post_send");
        } else if (wc.opcode == IBV_WC_SEND) {
            // SEND completion → pair with the earliest RX timestamp
            uint64_t tx_ns = 0;
            if (have_dev_clock) read_device_nanos(ctx, &tx_ns); else tx_ns = host_nanos();

            uint64_t rx_ns = 0;
            if (!rxq_pop(&rx_ns)) {
                // Shouldn't happen, but guard anyway
                rx_ns = tx_ns;
            }

            row_t r = { .idx = pkt_idx++, .size = (uint32_t)msg_size, .rx_ns = rx_ns, .tx_ns = tx_ns };
            rows_push(r);

            // Repost a RECV to keep pipeline full
            struct ibv_sge sge = { .addr=(uintptr_t)rxbuf, .length=msg_size, .lkey=mr->lkey };
            struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&sge, .num_sge=1 }, *bad;
            if (ibv_post_recv(id->qp, &rwr, &bad)) die("ibv_post_recv");
        }
    }

    // --- dump CSV on signal/exit ---
    write_csv((uint32_t)msg_size, port);

    // Cleanup
    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(rxbuf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(id);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);

    free(g_rows);
    free(rxq);
    return 0;
}
