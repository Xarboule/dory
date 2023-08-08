#ifndef BYPASS_H
#define BYPASS_H

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

/* Error Macro*/
#define rdma_error(msg, args...)                                               \
    do                                                                         \
    {                                                                          \
        fprintf(stderr, "%s : %d : ERROR : " msg, __FILE__, __LINE__, ##args); \
    } while (0);

#define ACN_RDMA_DEBUG

#ifdef ACN_RDMA_DEBUG
/* Debug Macro */
#define debug(msg, args...)            \
    do                                 \
    {                                  \
        printf("DEBUG: " msg, ##args); \
    } while (0);

#else

#define debug(msg, args...)

#endif /* ACN_RDMA_DEBUG */

/* Capacity of the completion queue (CQ) */
#define CQ_CAPACITY (1000)
/* MAX SGE capacity */
#define MAX_SGE (20)
/* MAX work requests */
#define MAX_WR (100)
/* Default port where the RDMA server is listening */
#define DEFAULT_RDMA_PORT (20886)
#define BIG_BUFFER_SIZE (1LU * 1024 * 1024)
#define SMALL_BUFFER_SIZE (1LU * 1024 * 1024)


namespace bypass{
/*
 * We use attribute so that compiler does not step in and try to pad the structure.
 * We use this structure to exchange information between the server and the client.
 *
 * For details see: http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
 */
struct __attribute((packed)) rdma_buffer_attr
{
    uint64_t address;
    uint32_t length;
    union stag
    {
        /* if we send, we call it local stags */
        uint32_t local_stag;
        /* if we receive, we call it remote stag */
        uint32_t remote_stag;
    } stag;
};

struct connection
{
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *cm_event_channel;
    struct ibv_pd *pd;
    struct ibv_comp_channel *io_completion_channel;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;

    struct sockaddr_in server_sockaddr;
    struct rdma_buffer_attr remote_buffer_info;
};

int process_rdma_cm_event(struct rdma_event_channel *echannel,
                          enum rdma_cm_event_type expected_event,
                          struct rdma_cm_event **cm_event)
{
    int ret = 1;
    ret = rdma_get_cm_event(echannel, cm_event);
    if (ret)
    {
        rdma_error("Failed to retrieve a cm event, errno: %d \n", -errno);
        return -errno;
    }
    /* lets see, if it was a good event */
    if (0 != (*cm_event)->status)
    {
        rdma_error("CM event has non zero status: %d\n", (*cm_event)->status);
        ret = -((*cm_event)->status);
        /* important, we acknowledge the event */
        rdma_ack_cm_event(*cm_event);
        return ret;
    }
    /* if it was a good event, was it of the expected type */
    if ((*cm_event)->event != expected_event)
    {
        rdma_error("Received event %s, TODO: handle!\n",
                   rdma_event_str((*cm_event)->event));
        /* important, we acknowledge the event */
        rdma_ack_cm_event(*cm_event);
        return -1; // unexpected event :(
    }
    debug("A new %s type event is received \n", rdma_event_str((*cm_event)->event));
    /* The caller must acknowledge the event */
    return ret;
}

struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd,
                                    void *addr, uint32_t length,
                                    unsigned int permission)
{
    struct ibv_mr *mr = NULL;
    if (!pd)
    {
        rdma_error("Protection domain is NULL, ignoring \n");
        return NULL;
    }
    mr = ibv_reg_mr(pd, addr, length, permission);
    if (!mr)
    {
        rdma_error("Failed to create mr on buffer, errno: %d \n", -errno);
        return NULL;
    }
    debug("Registered: %p , len: %u , stag: 0x%x \n",
          mr->addr,
          (unsigned int)mr->length,
          mr->lkey);
    return mr;
}

struct ibv_mr *rdma_buffer_alloc(struct ibv_pd *pd, uint32_t size,
                                 unsigned int permission)
{
    struct ibv_mr *mr = NULL;
    if (!pd)
    {
        rdma_error("Protection domain is NULL \n");
        return NULL;
    }
    void *buf = calloc(1, size);
    if (!buf)
    {
        rdma_error("failed to allocate buffer, -ENOMEM\n");
        return NULL;
    }
    debug("Buffer allocated: %p , len: %u \n", buf, size);
    mr = rdma_buffer_register(pd, buf, size, permission);
    if (!mr)
    {
        free(buf);
    }
    return mr;
}

/* Dirty legacy code to communicate the RDMA buffer location */
void *getLocalSetup(struct ibv_mr *local_mr)
{
    // max (2) + direct_pmem (1) + dest_size (1) + Addr (8) + length (8) + key (4) = 24
    void *privateData = malloc(24);
    memset(privateData, 0, 24);

    // printf("\n============ Local setup ===============\n");
    // printf("===== LOCAL ADDRESS : %ld\n", (uint64_t)local_mr->addr);
    // printf("===== LOCAL KEY : %d\n\n", local_mr->lkey);

    uint64_t addr = (uint64_t)local_mr->addr;
    uint32_t lkey = local_mr->lkey;

    memcpy(privateData + 4, (void *)&addr, 8);  // Address
    memcpy(privateData + 20, (void *)&lkey, 4); // Key

    return privateData;
}

void setRemoteSetup(struct rdma_buffer_attr *dst, const void *network_data)
{
    // 4 Bytes of offset to get the address
    memcpy(&dst->address, network_data + 4, 8);
    dst->length = BIG_BUFFER_SIZE;

    // 20 Bytes of offset to get KEY
    memcpy(&dst->stag.local_stag, network_data + 20, 4);
}

int bypass_client_connect(struct sockaddr_in *server_sockaddr,
                          struct ibv_context *context,
                          struct ibv_pd *pd,
                          struct ibv_mr *mr,
                          struct ibv_cq *cq,
                          struct ibv_comp_channel *io_completion_channel,
                          struct connection *c)
{
    int ret = 0;
    struct rdma_cm_event *cm_event = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct rdma_conn_param conn_param;

    c->server_sockaddr = *server_sockaddr;
    c->pd = pd;
    c->mr = mr;
    c->cq = cq;
    c->io_completion_channel = io_completion_channel;

    debug("Trying to connect to server at : %s port: %d \n",
          inet_ntoa(c->server_sockaddr.sin_addr),
          ntohs(c->server_sockaddr.sin_port));

    /*  Open a channel used to report asynchronous communication event */
    c->cm_event_channel = rdma_create_event_channel();
    if (!c->cm_event_channel)
    {
        rdma_error("Creating cm event channel failed, errno: %d \n", -errno);
        exit(-1);
    }
    debug("RDMA CM event channel is created at : %p \n", c->cm_event_channel);

    /* rdma_cm_id is the connection identifier (like socket) which is used
     * to define an RDMA connection. */
    ret = rdma_create_id(c->cm_event_channel, &c->cm_id,
                         NULL,
                         RDMA_PS_TCP);
    if (ret)
    {
        rdma_error("Creating cm id failed with errno: %d \n", -errno);
        exit(-1);
    }

    /* Resolve destination and optional source addresses from IP addresses  to
     * an RDMA address.  If successful, the specified rdma_cm_id will be bound
     * to a local device. */
    ret = rdma_resolve_addr(c->cm_id, NULL, (struct sockaddr *)&c->server_sockaddr, 2000);
    if (ret)
    {
        rdma_error("Failed to resolve address, errno: %d \n", -errno);
        exit(-1);
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
    ret = process_rdma_cm_event(c->cm_event_channel,
                                RDMA_CM_EVENT_ADDR_RESOLVED,
                                &cm_event);
    if (ret)
    {
        rdma_error("Failed to receive a valid event, ret = %d \n", ret);
        exit(-1);
    }

    /* we ack the event */
    ret = rdma_ack_cm_event(cm_event);
    if (ret)
    {
        rdma_error("Failed to acknowledge the CM event, errno: %d\n", -errno);
        exit(-1);
    }
    debug("RDMA address is resolved \n");

    /* At that point (after ack-ing the RDMA_CM_EVENT_ADDR_RESOLVED event), it seems
     * that the ibv_context has been created by rdma_cm in cm_client_id->verbs. We
     * replace it with our own context, which appears to be working. */
    c->cm_id->verbs = context;

    /* Resolves an RDMA route to the destination address in order to
     * establish a connection */
    ret = rdma_resolve_route(c->cm_id, 2000);
    if (ret)
    {
        rdma_error("Failed to resolve route, erno: %d \n", -errno);
        exit(-1);
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");

    ret = process_rdma_cm_event(c->cm_event_channel,
                                RDMA_CM_EVENT_ROUTE_RESOLVED,
                                &cm_event);
    if (ret)
    {
        rdma_error("Failed to receive a valid event, ret = %d \n", ret);
        exit(-1);
    }

    /* we ack the event */
    ret = rdma_ack_cm_event(cm_event);
    if (ret)
    {
        rdma_error("Failed to acknowledge the CM event, errno: %d \n", -errno);
        exit(-1);
    }

    /* Now the last step, set up the queue pair (send, recv) queues and their capacity.
     * The capacity here is define statically but this can be probed from the
     * device. We just use a small number as defined in rdma_common.h */
    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
    qp_init_attr.cap.max_recv_wr = MAX_WR;   /* Maximum receive posting capacity */
    qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
    qp_init_attr.cap.max_send_wr = MAX_WR;   /* Maximum send posting capacity */
    qp_init_attr.qp_type = IBV_QPT_RC;       /* QP type, RC = Reliable connection */
    /* We use same completion queue, but one can use different queues */
    qp_init_attr.recv_cq = c->cq; /* Where should I notify for receive completion operations */
    qp_init_attr.send_cq = c->cq; /* Where should I notify for send completion operations */
    /*Lets create a QP */
    ret = rdma_create_qp(c->cm_id /* which connection id */,
                         c->pd /* which protection domain*/,
                         &qp_init_attr /* Initial attributes */);
    if (ret)
    {
        rdma_error("Failed to create QP, errno: %d \n", -errno);
        exit(-1);
    }
    c->qp = c->cm_id->qp;
    debug("QP created at %p \n", c->qp);

    bzero(&conn_param, sizeof(conn_param));
    conn_param.initiator_depth = 16;
    conn_param.responder_resources = 16;
    conn_param.private_data = getLocalSetup(c->mr);
    conn_param.private_data_len = 24;
    conn_param.retry_count = 1; // if fail, then how many times to retry
    ret = rdma_connect(c->cm_id, &conn_param);
    if (ret)
    {
        rdma_error("Failed to connect to remote host , errno: %d\n", -errno);
        return -errno;
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");

    ret = process_rdma_cm_event(c->cm_event_channel,
                                RDMA_CM_EVENT_ESTABLISHED,
                                &cm_event);
    if (ret)
    {
        rdma_error("Failed to get cm event, ret = %d \n", ret);
        return ret;
    }
    setRemoteSetup(&c->remote_buffer_info, cm_event->param.conn.private_data); // dirty hack, where to READ/WRITE is sent by the server on connect

    ret = rdma_ack_cm_event(cm_event);
    if (ret)
    {
        rdma_error("Failed to acknowledge cm event, errno: %d\n",
                   -errno);
        return -errno;
    }
    debug("The client is connected successfully \n");

    return 0;
}

int bypass_client_broadcast(struct connection *c,
                            struct ibv_send_wr *wr,
                            struct ibv_send_wr **bad_wr)
{
    if (ibv_post_send(c->qp, wr, bad_wr)) {
        rdma_error("Error, ibv_post_send() failed\n");
        return -1;
    }
    return 0;
}

/* BLOCKING */
int bypass_server_start(struct sockaddr_in *server_sockaddr,
                        struct ibv_context *context,
                        struct ibv_pd *pd,
                        struct ibv_mr *mr,
                        struct ibv_cq *cq,
                        struct ibv_comp_channel *io_completion_channel,
                        struct connection *c)
{
    int ret = 0;
    struct rdma_cm_event *cm_event = NULL;
    struct rdma_cm_id *cm_server_id; // Temporary cm_id for listening puposes (not linked to a connection)

    c->server_sockaddr = *server_sockaddr;
    c->pd = pd;
    c->mr = mr;
    c->cq = cq;
    c->io_completion_channel = io_completion_channel;

    /*  Open a channel used to report asynchronous communication event */
    c->cm_event_channel = rdma_create_event_channel();
    if (!c->cm_event_channel)
    {
        rdma_error("Creating cm event channel failed with errno : (%d)", -errno);
        return -errno;
    }
    debug("RDMA CM event channel is created successfully at %p \n",
          c->cm_event_channel);

    /* rdma_cm_id is the connection identifier (like socket) which is used
     * to define an RDMA connection.
     */
    ret = rdma_create_id(c->cm_event_channel, &cm_server_id, NULL, RDMA_PS_TCP);
    if (ret)
    {
        rdma_error("Creating server cm id failed with errno: %d ", -errno);
        return -errno;
    }
    debug("A RDMA connection id for the server is created \n");

    /* Explicit binding of rdma cm id to the socket credentials */
    ret = rdma_bind_addr(cm_server_id, (struct sockaddr *)&c->server_sockaddr);
    if (ret)
    {
        rdma_error("Failed to bind server address, errno: %d \n", -errno);
        return -errno;
    }
    debug("Server RDMA CM id is successfully binded \n");

    /* Now we start to listen on the passed IP and port. However unlike
     * normal TCP listen, this is a non-blocking call. When a new client is
     * connected, a new connection management (CM) event is generated on the
     * RDMA CM event channel from where the listening id was created. Here we
     * have only one channel, so it is easy. */
    ret = rdma_listen(cm_server_id, 8); /* backlog = 8 clients, same as TCP, see man listen*/
    if (ret)
    {
        rdma_error("rdma_listen failed to listen on server address, errno: %d ", -errno);
        return -errno;
    }
    printf("Server is listening successfully at: %s , port: %d \n",
           inet_ntoa(c->server_sockaddr.sin_addr),
           ntohs(c->server_sockaddr.sin_port));

    /* now, we expect a client to connect and generate a RDMA_CM_EVNET_CONNECT_REQUEST
     * We wait (block) on the connection management event channel for
     * the connect event.
     */
    ret = process_rdma_cm_event(c->cm_event_channel,
                                RDMA_CM_EVENT_CONNECT_REQUEST,
                                &cm_event);
    if (ret)
    {
        rdma_error("Failed to get a valid cm event, ret = %d \n", ret);
        return ret;
    }

    c->cm_id = cm_event->id; // cm id corresponding to the newly established connection
    if (!c->cm_id)
    {
        rdma_error("Client id is still NULL \n");
        return -EINVAL;
    }

    /* now we acknowledge the event. Acknowledging the event free the resources
     * associated with the event structure. Hence any reference to the event
     * must be made before acknowledgment. Like, we have already saved the
     * client id from "id" field before acknowledging the event.
     */
    ret = rdma_ack_cm_event(cm_event);
    if (ret)
    {
        rdma_error("Failed to acknowledge the cm event errno: %d \n", -errno);
        return -errno;
    }
    debug("A new RDMA client connection id is stored at %p\n", c->cm_id);

    c->cm_id->verbs = context;

    /* Now the last step, set up the queue pair (send, recv) queues and their capacity.
     * The capacity here is define statically but this can be probed from the
     * device. We just use a small number as defined in rdma_common.h */
    struct ibv_qp_init_attr qp_init_attr;
    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
    qp_init_attr.cap.max_recv_wr = MAX_WR;   /* Maximum receive posting capacity */
    qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
    qp_init_attr.cap.max_send_wr = MAX_WR;   /* Maximum send posting capacity */
    qp_init_attr.qp_type = IBV_QPT_RC;       /* QP type, RC = Reliable connection */
    /* We use same completion queue, but one can use different queues */
    qp_init_attr.recv_cq = c->cq; /* Where should I notify for receive completion operations */
    qp_init_attr.send_cq = c->cq; /* Where should I notify for send completion operations */
    /*Lets create a QP */
    ret = rdma_create_qp(c->cm_id /* which connection id */,
                         c->pd /* which protection domain*/,
                         &qp_init_attr /* Initial attributes */);
    if (ret)
    {
        rdma_error("Failed to create QP due to errno: %d\n", -errno);
        return -errno;
    }

    /* Save the reference for handy typing but is not required */
    c->qp = c->cm_id->qp;
    debug("Client QP created at %p\n", c->qp);

    /* Now we accept the connection. Recall we have not accepted the connection
     * yet because we have to do lots of resource pre-allocation */
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    /* this tell how many outstanding requests can we handle */
    conn_param.initiator_depth = 16; /* For this exercise, we put a small number here */
    /* This tell how many outstanding requests we expect other side to handle */
    conn_param.responder_resources = 16; /* For this exercise, we put a small number */
    /* Dirty hack to communicate the READ/WRITE RDMA buffer */
    conn_param.private_data = getLocalSetup(c->mr);
    conn_param.private_data_len = 24;
    // cm_client_id is set in start_rdma_server, to the first client that connected.
    ret = rdma_accept(c->cm_id, &conn_param);
    if (ret)
    {
        rdma_error("Failed to accept the connection, errno: %d \n", -errno);
        return -errno;
    }

    /* Waiting for the RDMA_CM_EVENT_ESTABLISHED event. */
    ret = process_rdma_cm_event(c->cm_event_channel,
                                RDMA_CM_EVENT_ESTABLISHED,
                                &cm_event);
    if (ret)
    {
        rdma_error("Failed to get a valid cm event, ret = %d \n", ret);
        return ret;
    }

    ret = rdma_ack_cm_event(cm_event);
    if (ret)
    {
        rdma_error("Failed to acknowledge the cm event errno: %d \n", -errno);
        return -errno;
    }
    debug("CONNECTION ESTABLISHED.\n");

    return ret;
}
} //namespace bypass
#endif