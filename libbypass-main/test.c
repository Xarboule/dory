#include <getopt.h>

#include "bypass.h"

/* Code acknowledgment: rping.c from librdmacm/examples */
int get_addr(char *dst, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret = -1;
	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		rdma_error("getaddrinfo failed - invalid hostname or IP address\n");
		return ret;
	}
	memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	freeaddrinfo(res);
	return ret;
}

int create_ibv_context(struct ibv_context **ctx) {
    int ret = ibv_fork_init();
    if (ret) {
        rdma_error("Failed to init libibverbs.\n");
        return ret;
    }

    int device_count = 0;
    struct ibv_device** dev_list = ibv_get_device_list(&device_count);
    if (!dev_list) {
        rdma_error("Failed to get device list.\n");
        return -1;
    }
    debug("Found %d rdma devices.\n", device_count);

    if (device_count == 0) {
        rdma_error("No rdma device found.\n");
        return -1;
    }

    debug("Using device '%s'.\n", ibv_get_device_name(dev_list[0]));
    *ctx = ibv_open_device(dev_list[0]);
    if (!*ctx) {
        rdma_error("Failed to open device '%s'\n", ibv_get_device_name(dev_list[0]));
        return -1;
    }

    ibv_free_device_list(dev_list);

    debug("Context created successfully.\n");

    return 0;
}

void usage()
{
	printf("Usage:\n");
	printf("test [-s] [-a <server_addr>]\n");
	printf("  -s: server mode (default is client)\n");
	printf("(default port is %d)\n", DEFAULT_RDMA_PORT);
	exit(1);
}

int main(int argc, char **argv) {
    int ret, option;
    struct sockaddr_in server_sockaddr;
    int is_server = 0;

	while ((option = getopt(argc, argv, "sa:")) != -1) {
		switch (option) {
			case 's':
				is_server = 1;
				break;
			case 'a':
                ret = get_addr(optarg, (struct sockaddr *) &server_sockaddr);
                if (ret) {
                    rdma_error("Invalid IP \n");
                    exit(ret);
                }
				break;
			default:
				usage();
				break;
		}
	}

    server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);

    struct ibv_context *context = NULL;
    ret = create_ibv_context(&context); // cm_client_id->verbs
    if (ret) {
        rdma_error("Failed to create context.\n");
        exit(ret);
    }

    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) { rdma_error("Failed to alloc pd, errno: %d \n", -errno); exit(-1); }
    debug("pd allocated at %p \n", pd);

    struct ibv_comp_channel *io_completion_channel = ibv_create_comp_channel(context);
    if (!io_completion_channel) { rdma_error("Failed to create IO completion event channel, errno: %d\n", -errno); exit(-1); }
    debug("completion event channel created at : %p \n", io_completion_channel);

    struct ibv_cq *client_cq = ibv_create_cq(context /* which device*/,
                                             CQ_CAPACITY /* maximum capacity*/,
                                             NULL /* user context, not used here */,
                                             io_completion_channel /* which IO completion channel */,
                                             0 /* signaling vector, not used here*/);
    if (!client_cq) { rdma_error("Failed to create CQ, errno: %d \n", -errno); exit(-1); }
    debug("CQ created at %p with %d elements \n", client_cq, client_cq->cqe);

    if (ibv_req_notify_cq(client_cq, 0)) { rdma_error("Failed to request notifications, errno: %d\n", -errno); exit(-1); }

    struct ibv_mr *mr = rdma_buffer_alloc(pd, SMALL_BUFFER_SIZE,
                                            (IBV_ACCESS_REMOTE_READ |
                                             IBV_ACCESS_LOCAL_WRITE | // Must be set when REMOTE_WRITE is set.
                                             IBV_ACCESS_REMOTE_WRITE));

    struct connection c;
    if (is_server) {
        bypass_server_start(&server_sockaddr, context, pd, mr, client_cq, io_completion_channel, &c);
    } else {
        bypass_client_connect(&server_sockaddr, context, pd, mr, client_cq, io_completion_channel, &c);
    }
}