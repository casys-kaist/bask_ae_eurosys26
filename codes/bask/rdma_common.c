#include "rdma_common.h"

#define PFX "rcommon: "

#define KSM_RDMA_ADDR "192.168.14.116"
#define KSM_RDMA_PORT 10103

int ksm_rdma_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct ksm_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = KSM_ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = KSM_ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = KSM_CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!cb->server) {
			cb->state = KSM_CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = KSM_ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = KSM_ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		cb->state = KSM_ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

int ksm_rdma_server_recv(struct ksm_cb *cb, struct ib_wc *wc) {
	pr_info("ksm_rdma_server_recv called\n");
	return 0;
}

int ksm_rdma_client_recv(struct ksm_cb *cb, struct ib_wc *wc) {
	pr_info("ksm_rdma_client_recv called\n");
	return 0;
}

void ksm_rdma_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ksm_cb *cb = ctx;
	struct ib_wc wc;
	const struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == KSM_ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			pr_info("IB_WC_SEND\n");

			break;

		case IB_WC_RDMA_WRITE:
			pr_info("IB_WC_RDMA_WRITE\n");
			
			cb->state = KSM_RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			pr_info("IB_WC_RDMA_READ\n");
			
			cb->state = KSM_RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			pr_info("IB_WC_RECV\n");

			ret = cb->server ? ksm_rdma_server_recv(cb, &wc) :
						ksm_rdma_client_recv(cb, &wc);

			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = KSM_ERROR;
	wake_up_interruptible(&cb->sem);
}

int ksm_cb_setup_server(struct ksm_cb* cb) {
	int ret = 0;
	cb->server = -1;
	cb->state = KSM_IDLE;

	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);

	cb->addr_str = KSM_RDMA_ADDR;
	in4_pton(KSM_RDMA_ADDR, -1, cb->addr, -1, NULL);
	cb->addr_type = AF_INET;
	cb->port = htons(KSM_RDMA_PORT);
	cb->server = 1;

	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(&init_net, ksm_rdma_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);
out:
	return ret;
}

int ksm_cb_setup_client(struct ksm_cb* cb) {
	int ret = 0;
	cb->server = 0;
	cb->state = KSM_IDLE;

	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);

	cb->addr_str = KSM_RDMA_ADDR;
	in4_pton(KSM_RDMA_ADDR, -1, cb->addr, -1, NULL);
	cb->addr_type = AF_INET;
	cb->port = htons(KSM_RDMA_PORT);

	cb->cm_id = rdma_create_id(&init_net, ksm_rdma_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);
out:
	return ret;
}

int ksm_rdma_bind_server(struct ksm_cb* cb) {
	struct sockaddr_storage sin;
	int ret;

	{
		memset(&sin, 0, sizeof(sin));
		struct sockaddr_in *sin4 = (struct sockaddr_in *) &sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	}

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= KSM_CONNECT_REQUEST);
	if (cb->state != KSM_CONNECT_REQUEST) {
		printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	return 0;
}

int ksm_rdma_bind_client(struct ksm_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	{
		memset(&sin, 0, sizeof(sin));
		struct sockaddr_in *sin4 = (struct sockaddr_in *) &sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	}

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= KSM_ROUTE_RESOLVED);
	if (cb->state != KSM_ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

int ksm_cb_setup_qp(struct ksm_cb* cb, struct rdma_cm_id* cm_id) {
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * 2;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, ksm_rdma_cq_event_handler, NULL,
			      cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	if (ret) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		goto err2;
	}
	
	// create queue pair
	{
		struct ib_qp_init_attr init_attr;
		int ret;

		memset(&init_attr, 0, sizeof(init_attr));
		init_attr.cap.max_send_wr = cb->txdepth;
		init_attr.cap.max_recv_wr = 2;

		/* For flush_qp() */
		init_attr.cap.max_send_wr++;
		init_attr.cap.max_recv_wr++;

		init_attr.cap.max_recv_sge = 1;
		init_attr.cap.max_send_sge = 1;
		init_attr.qp_type = IB_QPT_RC;
		init_attr.send_cq = cb->cq;
		init_attr.recv_cq = cb->cq;
		init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

		if (cb->server) {
			ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
			if (!ret)
				cb->qp = cb->child_cm_id->qp;
		} else {
			ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
			if (!ret)
				cb->qp = cb->cm_id->qp;
		}
	}

	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

int ksm_rdma_accept(struct ksm_cb* cb) {
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= KSM_CONNECTED);
	if (cb->state == KSM_ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", 
			cb->state);
		return -1;
	}
	
	return 0;
}

int ksm_connect_client(struct ksm_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= KSM_CONNECTED);
	if (cb->state == KSM_ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

int ksm_rdma_server_thread(void* arg) {
	const struct ib_recv_wr *bad_wr;
	int ret;

	struct ksm_cb* cb = (struct ksm_cb*) kzalloc(sizeof(*cb), GFP_KERNEL);

	pr_info("Start Init\n");

	ret = ksm_cb_setup_server(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_setup_server failed: %d\n", ret);
		goto err0;
	}

	ret = ksm_rdma_bind_server(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_bind_server failed: %d\n", ret);
		goto err0;
	}

	pr_info("Bind Done\n");
	
	ret = ksm_cb_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	pr_info("QP Setup Done\n");

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err1;
	}

	pr_info("Ready to recv\n");

	ret = ksm_rdma_accept(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err1;
	}

	pr_info("Accept Done\n");

	wait_event_interruptible(cb->sem, cb->state >= KSM_ERROR);

	rdma_disconnect(cb->child_cm_id);
err1:
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
err0:
	rdma_destroy_id(cb->cm_id);
	kfree(cb);
	return 0;
}

int ksm_rdma_client_thread(void* arg) {
	const struct ib_recv_wr *bad_wr;
	int ret;

	struct ksm_cb* cb = (struct ksm_cb*) kzalloc(sizeof(*cb), GFP_KERNEL);

	pr_info("Start Init\n");

	ret = ksm_cb_setup_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_setup_server failed: %d\n", ret);
		goto err0;
	}

	ret = ksm_rdma_bind_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_bind_server failed: %d\n", ret);
		goto err0;
	}

	ret = ksm_cb_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err1;
	}

	ret = ksm_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err1;
	}

	pr_info("Connect Done\n");
	
	while (!signal_pending(current)) {
        // This sleep can be interrupted by signals
        if (msleep_interruptible(1000))
            break;  // Sleep interrupted by a signal
        pr_info("mymodule: still sleeping, press Ctrl+C in userspace insmod...\n");
    }

	rdma_disconnect(cb->cm_id);
err1:
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
err0:
	rdma_destroy_id(cb->cm_id);
	kfree(cb);
	return 0;
}

void ksm_rdma_create_connection(struct ksm_cb* cb) {
	const struct ib_recv_wr *bad_wr;
	int ret;

	pr_info("Start Init\n");

	ret = ksm_cb_setup_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_setup_server failed: %d\n", ret);
		goto err0;
	}

	ret = ksm_rdma_bind_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "ksm_cb_bind_server failed: %d\n", ret);
		goto err0;
	}

	ret = ksm_cb_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err1;
	}

	ret = ksm_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	pr_info("Connect Done\n");
	return;

err2:
	rdma_disconnect(cb->cm_id);
err1:
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
err0:
	rdma_destroy_id(cb->cm_id);
	kfree(cb);
}

// extern int (*mlx_ib_dma_map_sgtable_attrs)(struct ib_device *dev,
// 					   struct sg_table *sgt,
// 					   enum dma_data_direction direction,
// 					   unsigned long dma_attrs);
// extern struct ib_mr *(*mlx_ib_alloc_mr)(struct ib_pd *pd, enum ib_mr_type mr_type,
// 			  u32 max_num_sg);
// extern int (*mlx_ib_map_mr_sg)(struct ib_mr *mr, struct scatterlist *sg, int sg_nents,
// 		 unsigned int *sg_offset, unsigned int page_size);
// extern int (*mlx_ib_dereg_mr)(struct ib_mr *mr);
// extern void (*mlx_ib_dma_unmap_sgtable_attrs)(struct ib_device *dev,
// 					      struct sg_table *sgt,
// 					      enum dma_data_direction direction,
// 					      unsigned long dma_attrs);
// extern u64 (*mlx_ib_dma_map_page)(struct ib_device *dev,
// 				  struct page *page,
// 				  unsigned long offset,
// 				  size_t size,
// 					 enum dma_data_direction direction);
// extern void (*mlx_ib_dma_unmap_page)(struct ib_device *dev,
// 				     u64 addr, size_t size,
// 				     enum dma_data_direction direction);

int mlx_ib_dma_map_page(struct ib_device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction direction) {
	return ib_dma_map_page(dev, page, offset, size, direction);
}

void mlx_ib_dma_unmap_page(struct ib_device *dev, u64 addr, size_t size, enum dma_data_direction direction) {
	ib_dma_unmap_page(dev, addr, size, direction);
}

int mlx_ib_dma_map_sgtable_attrs(struct ib_device *dev, struct sg_table *sgt, enum dma_data_direction direction, unsigned long dma_attrs) {
	return ib_dma_map_sgtable_attrs(dev, sgt, direction, dma_attrs);
}

void mlx_ib_dma_unmap_sgtable_attrs(struct ib_device *dev, struct sg_table *sgt, enum dma_data_direction direction, unsigned long dma_attrs) {
	ib_dma_unmap_sgtable_attrs(dev, sgt, direction, dma_attrs);
}

struct ib_mr *mlx_ib_alloc_mr(struct ib_pd *pd, enum ib_mr_type mr_type, u32 max_num_sg) {
	return ib_alloc_mr(pd, mr_type, max_num_sg);
}

int mlx_ib_map_mr_sg(struct ib_mr *mr, struct scatterlist *sg, int sg_nents, unsigned int *sg_offset, unsigned int page_size) {
	return ib_map_mr_sg(mr, sg, sg_nents, sg_offset, page_size);
}

int mlx_ib_dereg_mr(struct ib_mr *mr) {
	return ib_dereg_mr(mr);
}

int mlx_ib_dma_map_sg(struct ib_device *dev,
				struct scatterlist *sg, int nents,
				enum dma_data_direction direction) {
	return ib_dma_map_sg(dev, sg, nents, direction);
}

void mlx_ib_dma_unmap_sg(struct ib_device *dev,
				  struct scatterlist *sg, int nents,
				  enum dma_data_direction direction) {
	ib_dma_unmap_sg(dev, sg, nents, direction);
}
