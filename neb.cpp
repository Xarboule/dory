#include "neb.hpp"
#include "hrd.hpp"

NonEquivocatingBroadcast::~NonEquivocatingBroadcast() {
  // TODO(Kristian): clean all CQs
  running = false;

  hrd_ctrl_blk_destroy(cb);
  delete cb;

  for (size_t i = 0; i < num_proc; i++) {
    if (i == lgid) continue;

    free(bcst_qps[i]);
    free(repl_qps[i]);
  }

  free(bcst_qps);
  free(repl_qps);

  hrd_close_memcached();
}

NonEquivocatingBroadcast::NonEquivocatingBroadcast(size_t lgid, size_t num_proc)
    : lgid(lgid), num_proc(num_proc), running(true) {
  printf("neb: Begin control path");

  // TODO(Kristian): use builder pattern
  struct hrd_conn_config_t conn_config;
  memset(&conn_config, 0, sizeof(hrd_conn_config_t));
  conn_config.max_rd_atomic = 16;
  conn_config.sq_depth = kHrdSQDepth;
  conn_config.num_qps = num_proc;
  conn_config.use_uc = 0;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  bcst_qps = (hrd_qp_attr_t **)malloc(num_proc * sizeof(hrd_qp_attr_t));
  repl_qps = (hrd_qp_attr_t **)malloc(num_proc * sizeof(hrd_qp_attr_t));

  cb =
      hrd_ctrl_blk_init(lgid, ib_port_index, kHrdInvalidNUMANode, &conn_config);

  // Announce the QPs
  for (int i = 0; i < num_proc; i++) {
    if (i == num_proc) continue;

    char srv_name[kHrdQPNameSize];
    sprintf(srv_name, "broadcast-%d-%zu", i, lgid);
    hrd_publish_conn_qp(cb, i * 2, srv_name);
    printf("neb: Node %zu published broadcast slot for node %d\n", lgid, i);

    sprintf(srv_name, "replay-%zu-%d", lgid, i);
    hrd_publish_conn_qp(cb, i * 2 + 1, srv_name);
    printf("neb: Node %zu published replay slot for node %d\n", lgid, i);
  }

  for (int i = 0; i < num_proc; i++) {
    if (i == lgid) continue;

    char clt_name[kHrdQPNameSize];
    hrd_qp_attr_t *clt_qp = nullptr;

    // connect to broadcast
    sprintf(clt_name, "broadcast-%zu-%d", lgid, i);
    printf("neb: Looking for %s server\n", clt_name);

    while (clt_qp == nullptr) {
      clt_qp = hrd_get_published_qp(clt_name);
      if (clt_qp == nullptr) usleep(200000);
    }

    printf("neb: Server %s found server! Connecting..\n", clt_qp->name);
    hrd_connect_qp(cb, i * 2, clt_qp);
    // This garbles the server's qp_attr - which is safe
    hrd_publish_ready(clt_qp->name);
    bcst_qps[i] = clt_qp;
    printf("neb: Server %s READY\n", clt_qp->name);

    // Connect to replay
    sprintf(clt_name, "replay-%d-%zu", i, lgid);
    printf("neb: Looking for %s server\n", clt_name);

    clt_qp = nullptr;
    while (clt_qp == nullptr) {
      clt_qp = hrd_get_published_qp(clt_name);
      if (clt_qp == nullptr) usleep(200000);
    }

    printf("neb: Server %s found server! Connecting..\n", clt_qp->name);
    hrd_connect_qp(cb, i * 2 + 1, clt_qp);
    // This garbles the server's qp_attr - which is safe
    hrd_publish_ready(clt_qp->name);
    repl_qps[i] = clt_qp;
    printf("neb: Server %s READY\n", clt_qp->name);
  }

  // Wait till qps are ready
  for (int i = 0; i < num_proc; i++) {
    if (i == lgid) continue;

    char clt_name[kHrdQPNameSize];

    sprintf(clt_name, "broadcast-%d-%zu", i, lgid);
    hrd_wait_till_ready(clt_name);

    sprintf(clt_name, "replay-%zu-%d", lgid, i);
    hrd_wait_till_ready(clt_name);
  }

  printf("neb: Broadcast and replay connections established\n");

  std::thread([=] { run_poller(); }).detach();
}

int NonEquivocatingBroadcast::broadcast(size_t m_id, size_t val) {
  // TODO(Kristian): Fixme
  char text[kHrdQPNameSize];
  sprintf(text, "Hello From %zu", lgid);

  struct neb_msg_t msg = {
      .id = m_id, .data = (void *)&text, .len = sizeof(text)};

  auto *own_buf = reinterpret_cast<volatile uint8_t *>(cb->conn_buf[lgid]);

  size_t msg_size = msg.marshall((uint8_t *)own_buf);

  // Broadcast: write to every "broadcast-self-x" qp
  for (int i = 0; i < num_proc; i++) {
    if (i == lgid) continue;

    const size_t offset = 256;

    struct ibv_sge sg;
    struct ibv_send_wr wr;
    // struct ibv_wc wc;
    struct ibv_send_wr *bad_wr = nullptr;

    memset(&sg, 0, sizeof(sg));
    sg.length = msg_size;
    sg.addr = reinterpret_cast<uint64_t>(own_buf);
    sg.lkey = cb->conn_buf_mr[lgid]->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 0;
    wr.sg_list = &sg;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.next = nullptr;
    // wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = bcst_qps[i]->buf_addr;
    wr.wr.rdma.rkey = bcst_qps[i]->rkey;

    printf("main: Write over broadcast QP to %d\n", i);

    if (ibv_post_send(cb->conn_qp[i * 2], &wr, &bad_wr)) {
      fprintf(stderr, "Error, ibv_post_send() failed\n");
      return -1;
    }

    if (bad_wr != nullptr) {
      printf("bad_wr is set!\n");
    }

    // hrd_poll_cq(cb->conn_cq[i * 2], 1, &wc);
  }
}

void NonEquivocatingBroadcast::run_poller() {
  printf("main: poller thread running\n");

  while (running) {
    for (int i = 0; i < num_proc; i++) {
      if (i == lgid) continue;

      const size_t offset = 1024;

      auto *bcast_buf =
          reinterpret_cast<volatile uint8_t *>(cb->conn_buf[i * 2]);

      neb_msg_t msg;

      msg.unmarshall((uint8_t *)bcast_buf);

      // TODO(Kristian): add more checks like matching next id etc.
      if (msg.id != 0) {
        printf("main: bcast from %d = %s\n", i, (char *)msg.data);

        auto *repl_buf =
            reinterpret_cast<volatile uint8_t *>(cb->conn_buf[i * 2 + 1]);

        memcpy((void *)&repl_buf[i * msg.size()], (void *)bcast_buf,
               msg.size());

        printf("main: copying to replay-buffer\n");

        // read replay slots for origin i
        for (int j = 0; j < num_proc; j++) {
          if (j == lgid || j == i) {
            continue;
          }

          struct ibv_sge sg;
          struct ibv_send_wr wr;
          // struct ibv_wc wc;
          struct ibv_send_wr *bad_wr = nullptr;

          memset(&sg, 0, sizeof(sg));
          sg.addr =
              reinterpret_cast<uint64_t>(cb->conn_buf[i * 2 + 1]) +
              (offset + (i * num_proc + j) * msg.size()) * sizeof(uint8_t);
          sg.length = msg.size();
          sg.lkey = cb->conn_buf_mr[i * 2 + 1]->lkey;

          memset(&wr, 0, sizeof(wr));
          // wr.wr_id      = 0;
          wr.sg_list = &sg;
          wr.num_sge = 1;
          wr.opcode = IBV_WR_RDMA_READ;
          // wr.send_flags = IBV_SEND_SIGNALED;
          wr.wr.rdma.remote_addr = repl_qps[j]->buf_addr + i * msg.size();
          wr.wr.rdma.rkey = repl_qps[j]->rkey;

          printf("main: Posting replay read for %d at %d\n", i, j);

          if (ibv_post_send(cb->conn_qp[j * 2 + 1], &wr, &bad_wr)) {
            fprintf(stderr, "Error, ibv_post_send() failed\n");
            // TODO(Kristian): properly handle
            return;
          }

          // hrd_poll_cq(cb->conn_cq[j * 2 + 1], 1, &wc);

          if (bad_wr != nullptr) {
            printf("bad_wr is set!\n");
          }

          neb_msg_t r_msg;

          r_msg.unmarshall(
              (uint8_t *)&repl_buf[(offset + (i * num_proc + j) * msg.size())]);
          printf("main: replay entry for %d at %d = %s\n", i, j,
                 (char *)r_msg.data);
        }
      }
    }

    sleep(1);
  }

  printf("neb: poller thread finishing\n");
}