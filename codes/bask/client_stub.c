#include "client_stub.h"

#include <linux/export.h>
#include <linux/wait.h>
#include <linux/inet.h>
#include "linux/dma-direction.h"
#include "linux/scatterlist.h"
#include "rdma/ib_verbs.h"
#include "rdma/rdma_cm.h"
#include "time_util.h"

#define PFX "ksm_rdma: "

DEBUG_EVENT_TIMER(rdma_send_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(rdma_send_time);

DEBUG_EVENT_TIMER(irq_switch_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(irq_switch_time);

DEBUG_EVENT_TIMER(rdma_recv_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(rdma_recv_time);

DEBUG_EVENT_TIMER(rdma_wait_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(rdma_wait_time);

DEBUG_EVENT_TIMER(total_memcmp_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(total_memcmp_time);

DEBUG_EVENT_TIMER(total_hash_time);
DEBUG_EVENT_TIMER_EXPORT_SYMBOL(total_hash_time);

#ifdef PRINT_TIME
	void print_time_and_reset(void) {
			PRINT_HDR();
			PRINT_TIMER(total_memcmp_time, "Total Memcmp Time");
			PRINT_TIMER(total_hash_time, "Total Hash Time");
			PRINT_TIMER(rdma_send_time, "RDMA Send Time");
			PRINT_TIMER(rdma_recv_time, "RDMA Recv Time");
			PRINT_TIMER(rdma_wait_time, "RDMA Wait Time");
			PRINT_TIMER(irq_switch_time, "Context switch Time");
	
			RESET_TIMER(total_memcmp_time);
			RESET_TIMER(total_hash_time);
			RESET_TIMER(rdma_send_time);
			RESET_TIMER(rdma_recv_time);
			RESET_TIMER(rdma_wait_time);
			RESET_TIMER(irq_switch_time);	
	}
#else
	void print_time_and_reset(void) {

	}
#endif

void debug_stop(void) {
	while (1) {
		msleep(1000);
	}
}

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
		pr_err("Unexpected CONNECT_REQUEST event\n");
		cb->state = KSM_ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		cb->state = KSM_CONNECTED;
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

int ksm_rdma_client_recv(struct ksm_cb *cb, struct ib_wc *wc) {
	switch (ksm_offload_mode) {
		case KSM_OFFLOAD:
			DEBUG_LOG("Recved result: scanned %d, merged %d\n", cb->result_desc.total_scanned_cnt, cb->result_desc.log_cnt);
			break;
		case SINGLE_OPERATION_OFFLOAD:
			DEBUG_LOG("Recved result: cmd %d, id %d\n", cb->single_op_result_rx.cmd, cb->single_op_result_rx.id);
			break;
		default:
			printk(KERN_ERR PFX "Invalid mode state %d\n", ksm_offload_mode);
			break;
	}

	return 0;
}

void ksm_rdma_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ksm_cb *cb = ctx;
	struct ib_wc wc;
	// const struct ib_recv_wr *bad_wr;
	int ret;
	memset(&wc, 0, sizeof(wc));

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
				       "wr_id %s(%Lx) status %d opcode %d vender_err %x\n\n",
					   ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		} else {
			DEBUG_LOG("cq completion with wr_id %s(%Lx) status %d opcode %d bytes %d\n",
				ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.status, wc.opcode, wc.byte_len);
		}

		if (wc.wr_id == WR_REG_MR) {
			DEBUG_LOG("IB_WC_REG_MR: %d", IB_WC_REG_MR);
			cb->state = KSM_MEM_REG_COMPLETE;
			wake_up_interruptible(&cb->sem);
			return;
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("IB_WC_SEND\n");
			cb->state = KSM_RDMA_SEND_COMPLETE;
			wake_up_interruptible(&cb->sem);

			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("IB_WC_RDMA_WRITE\n");
			
			cb->state = KSM_RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("IB_WC_RDMA_READ\n");
			
			cb->state = KSM_RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
		DEBUG_TIME_START(irq_switch_time);
			DEBUG_LOG("IB_WC_RECV\n");

			ret = ksm_rdma_client_recv(cb, &wc);

			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			// memset(&cb->result_desc, 0, sizeof(struct result_desc));
			// ret = ib_post_recv(cb->qp, &cb->result_recv_wr, &bad_wr);
			// if (ret) {
			// 	printk(KERN_ERR PFX "post recv error: %d\n", 
			// 	       ret);
			// 	goto error;
			// }
			cb->state = KSM_RDMA_RECV_COMPLETE;

			wake_up_interruptible(&cb->sem);
			break;
			
		case IB_WC_REG_MR:
			pr_info("IB_WC_REG_MR\n");
			cb->state = KSM_MEM_REG_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;
			
		case IB_WC_LOCAL_INV:
			DEBUG_LOG("IB_WC_LOCAL_INV\n");
			cb->state = KSM_MR_INVALIDATE_COMPLETE;
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

int ksm_cb_setup_client(struct ksm_cb* cb) {
	int ret = 0;
	cb->state = KSM_IDLE;

	init_waitqueue_head(&cb->sem);

	cb->addr_str = SERVER_IP;
	in4_pton(SERVER_IP, -1, cb->addr, -1, NULL);
	cb->addr_type = AF_INET;
	cb->port = htons(SERVER_PORT);

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

int ksm_rdma_bind_client(struct ksm_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;
	struct sockaddr_in *sin4;

	{
		memset(&sin, 0, sizeof(sin));
		sin4 = (struct sockaddr_in *) &sin;
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

	attr.cqe = MAX_SEND_WR + MAX_RECV_WR;
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
		init_attr.send_cq = cb->cq;
		init_attr.recv_cq = cb->cq;

		init_attr.cap.max_send_wr = MAX_SEND_WR;
		init_attr.cap.max_recv_wr = MAX_RECV_WR;
		
		/* For flush_qp() */
		init_attr.cap.max_send_wr++;
		init_attr.cap.max_recv_wr++;

		init_attr.cap.max_recv_sge = MAX_SGE;
		init_attr.cap.max_send_sge = MAX_SGE;
		init_attr.qp_type = IB_QPT_RC;

		init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
		
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

	cb->state = KSM_CONNECT_REQUEST;
	wait_event_interruptible(cb->sem, cb->state >= KSM_CONNECTED);
	if (cb->state == KSM_ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

int ksm_cb_setup_buffer(struct ksm_cb* cb) {
	cb->md_desc_dma_addr = ib_dma_map_single(cb->pd->device, &cb->md_desc_tx, sizeof(struct metadata_descriptor), DMA_BIDIRECTIONAL);
    if (ib_dma_mapping_error(cb->pd->device, cb->md_desc_dma_addr)) {
        pr_err("Failed to map single\n");
		return -1;
    }

    cb->result_dma_addr = ib_dma_map_single(cb->pd->device, &cb->result_desc, sizeof(struct result_desc), DMA_BIDIRECTIONAL);
    if (ib_dma_mapping_error(cb->pd->device, cb->result_dma_addr)) {
        pr_err("Failed to map single\n");
		ib_dma_unmap_single(cb->pd->device, cb->md_desc_dma_addr, sizeof(struct metadata_descriptor), DMA_BIDIRECTIONAL);
		return -1;
    }

	cb->single_op_desc_dma_addr =  ib_dma_map_single(cb->pd->device, &cb->single_op_desc_tx, sizeof(struct operation_descriptor), DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(cb->pd->device, cb->single_op_desc_dma_addr)) {
        pr_err("Failed to map single\n");
		return -1;
    }

	cb->single_op_result_dma_addr = ib_dma_map_single(cb->pd->device, &cb->single_op_result_rx, sizeof(struct operation_result), DMA_BIDIRECTIONAL);
	if (ib_dma_mapping_error(cb->pd->device, cb->single_op_desc_dma_addr)) {
        pr_err("Failed to map single\n");
		return -1;
    }

    cb->md_desc_sgl.addr = cb->md_desc_dma_addr;
    cb->md_desc_sgl.length = sizeof(cb->md_desc_tx);
    cb->md_desc_sgl.lkey = cb->pd->local_dma_lkey;

	cb->md_send_wr.wr_id = WR_SEND_METADATA;
	cb->md_send_wr.next = NULL;
    cb->md_send_wr.sg_list = &cb->md_desc_sgl;
    cb->md_send_wr.num_sge = 1;
    cb->md_send_wr.opcode = IB_WR_SEND;
    cb->md_send_wr.send_flags = IB_SEND_SIGNALED;

    cb->result_sgl.addr = cb->result_dma_addr;
    cb->result_sgl.length = sizeof(struct result_desc);
    cb->result_sgl.lkey = cb->pd->local_dma_lkey;

	cb->result_recv_wr.wr_id = WR_RECV_RESULT;
	cb->result_recv_wr.sg_list = &cb->result_sgl;
	cb->result_recv_wr.num_sge = 1;
	cb->result_recv_wr.next = NULL;

	cb->single_op_desc_sgl.addr = cb->single_op_desc_dma_addr;
	cb->single_op_desc_sgl.length = sizeof(cb->single_op_desc_tx);
	cb->single_op_desc_sgl.lkey = cb->pd->local_dma_lkey;

	cb->single_op_send_wr.wr_id = WR_SEND_SINGLE_RESULT;
	cb->single_op_send_wr.next = NULL;
	cb->single_op_send_wr.sg_list = &cb->single_op_desc_sgl;
	cb->single_op_send_wr.num_sge = 1;
	cb->single_op_send_wr.opcode = IB_WR_SEND;

	cb->single_op_result_sgl.addr = cb->single_op_result_dma_addr;
	cb->single_op_result_sgl.length = sizeof(struct operation_result);
	cb->single_op_result_sgl.lkey = cb->pd->local_dma_lkey;

	cb->single_op_recv_wr.wr_id = WR_RECV_SINGLE_RESULT;
	cb->single_op_recv_wr.sg_list = &cb->single_op_result_sgl;
	cb->single_op_recv_wr.num_sge = 1;
	cb->single_op_recv_wr.next = NULL;

	return 0;
}

void ksm_rdma_create_connection(struct ksm_cb* cb) {
	const struct ib_recv_wr *bad_wr;
	int ret;

	pr_info("Start Init\n");
	if (cb->tag != sizeof (struct ksm_cb)) {
		printk(KERN_ERR PFX "cb tag mismatch %d\n", cb->tag);
		return;
	}
	
	do {
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

		ret = ksm_cb_setup_buffer(cb);
		if (ret) {
			printk(KERN_ERR PFX "setup_buffer failed: %d\n", ret);
			goto err1;
		}

		switch (ksm_offload_mode) {
			case KSM_OFFLOAD:
				ret = ib_post_recv(cb->qp, &cb->result_recv_wr, &bad_wr);
				if (ret) {
					printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
					goto err1;
				}

				break;
			case SINGLE_OPERATION_OFFLOAD:
				ret = ib_post_recv(cb->qp, &cb->single_op_recv_wr, &bad_wr);
				if (ret) {
					printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
					goto err1;
				}
				break;
			default:
				printk(KERN_ERR PFX "Invalid operation mode: %d\n", ksm_offload_mode);
				goto err1;
				break;
		}


		ret = ksm_connect_client(cb);
		if (ret) {
			printk(KERN_ERR PFX "connect error %d\n", ret);
			rdma_disconnect(cb->cm_id);
			goto err1;
		} else {
			pr_info("Connect Done\n");
			return;
		}

err1:
		ib_destroy_qp(cb->qp);
		ib_destroy_cq(cb->cq);
		ib_dealloc_pd(cb->pd);
err0:
		rdma_destroy_id(cb->cm_id);

		msleep(30 * 1000);
	} while (1);

	pr_info("Unreachable code\n");
	kfree(cb);
}

int ksm_rdma_meta_send(struct ksm_cb* cb) {
	const struct ib_send_wr *bad_send_wr;

	int ret;

	if (!cb) {
		printk(KERN_ERR PFX "cb is NULL\n");
		return -1;
	}

	ret = ib_post_send(cb->qp, &cb->md_send_wr, &bad_send_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_send failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= KSM_RDMA_SEND_COMPLETE);
	if (cb->state != KSM_RDMA_SEND_COMPLETE) {
		if (cb->state == KSM_RDMA_RECV_COMPLETE) {
			return 0; // already received
		} else {
			printk(KERN_ERR PFX "wait for RDMA_SEND_COMPLETE state %d\n",
				cb->state);
			return -1;
		}

	}

	return 0;
}

struct result_table* ksm_rdma_result_recv(struct ksm_cb *cb, unsigned long* ksm_pages_scanned) {
	const struct ib_recv_wr *bad_wr;
	struct result_table* result_table;
	dma_addr_t addr;
	int dma_size, ret;

	struct ib_sge sge;
	struct ib_rdma_wr rdma_wr;

	int i, tables_cnt, this_size;
	struct ksm_event_log* entries;

	if (!cb) {
		printk(KERN_ERR PFX "cb is NULL\n");
		return NULL;
	}

	// if (cb->state != KSM_RDMA_SEND_COMPLETE) {
	// 	pr_err("Already received: %d\n", cb->state);
	// }

	wait_event_interruptible(cb->sem, cb->state >= KSM_RDMA_RECV_COMPLETE);
	if (cb->state != KSM_RDMA_RECV_COMPLETE) {
		printk(KERN_ERR PFX "wait for RECV_COMPLETE state %d\n",
			cb->state);
		return NULL;
	}

	result_table = kmalloc(sizeof(struct result_table), GFP_KERNEL);
	if (!result_table) {
		pr_err("Failed to allocate result_table\n");
		return NULL;
	}

	*ksm_pages_scanned += cb->result_desc.total_scanned_cnt;
	
	tables_cnt = (cb->result_desc.log_cnt + MAX_RESULT_TABLE_ENTRIES - 1) / MAX_RESULT_TABLE_ENTRIES;

	result_table->total_cnt = cb->result_desc.log_cnt;
	result_table->tables_cnt = tables_cnt;

	result_table->unmap_addrs = kmalloc(sizeof(dma_addr_t) * tables_cnt, GFP_KERNEL);
	if (!result_table->unmap_addrs) {
		pr_err("Failed to allocate result_table unmap_addrs\n");
		kfree(result_table);
		return NULL;
	}

	result_table->entry_tables = kmalloc(sizeof(struct ksm_event_log*) * tables_cnt, GFP_KERNEL);
	if (!result_table->entry_tables) {
		pr_err("Failed to allocate result_table tables\n");
		kfree(result_table->unmap_addrs);
		kfree(result_table);
		return NULL;
	}

	pr_info("[KSM] TABLES CNT: %d\n", tables_cnt);

	for (i = 0; i < tables_cnt; i++) {
		this_size = (i == tables_cnt - 1) ? cb->result_desc.log_cnt - i * MAX_RESULT_TABLE_ENTRIES : MAX_RESULT_TABLE_ENTRIES;

		DEBUG_LOG("Reading Table part %d with size %d\n", i, this_size);

		dma_size = sizeof(struct ksm_event_log) * this_size;

		// entries = kmalloc(dma_size, GFP_KERNEL);
		entries = ksm_rdma_huge_alloc();

		if (!entries) {
			pr_err("Failed to allocate result_table entries\n");
			kfree(result_table->unmap_addrs);
			kfree(result_table->entry_tables);
			kfree(result_table);
			
			return NULL;
		}
		
		addr = ib_dma_map_single(cb->pd->device, entries, dma_size, DMA_BIDIRECTIONAL);
		if (ib_dma_mapping_error(cb->pd->device, addr)) {
			pr_err("Failed to map single\n");
			kfree(entries);
			kfree(result_table->unmap_addrs);
			kfree(result_table->entry_tables);
			kfree(result_table);
			return NULL;
		}
		result_table->unmap_addrs[i] = addr;
		result_table->entry_tables[i] = entries;

		memset(&sge, 0, sizeof(sge));
		sge.addr = addr;
		sge.length = dma_size;
		sge.lkey = cb->pd->local_dma_lkey;

		memset(&rdma_wr, 0, sizeof(rdma_wr));
		rdma_wr.wr.wr_id = WR_READ_RESULT;
		rdma_wr.wr.sg_list = &sge;
		rdma_wr.wr.num_sge = 1;
		rdma_wr.wr.opcode = IB_WR_RDMA_READ;
		rdma_wr.wr.send_flags = IB_SEND_SIGNALED;

		rdma_wr.rkey = cb->result_desc.rkey;
		rdma_wr.remote_addr = cb->result_desc.result_table_addr + i * MAX_RESULT_TABLE_ENTRIES * sizeof(struct ksm_event_log);

		cb->state = KSM_RDMA_READ_WAIT;
		ret = ib_post_send(cb->qp, &rdma_wr.wr, NULL);
		if (ret) {
			pr_err("ib_post_send failed for read result %d\n", ret);
			ib_dma_unmap_single(cb->pd->device, addr, dma_size, DMA_BIDIRECTIONAL);
			kfree(entries);
			kfree(result_table->unmap_addrs);
			kfree(result_table->entry_tables);
			kfree(result_table);
			return NULL;
		}

		wait_event_interruptible(cb->sem, cb->state >= KSM_RDMA_READ_COMPLETE);
		if (cb->state != KSM_RDMA_READ_COMPLETE) {
			pr_err("Failed to wait for RDMA_READ_COMPLETE state %d\n", cb->state);
			ib_dma_unmap_single(cb->pd->device, addr, dma_size, DMA_BIDIRECTIONAL);
			kfree(entries);
			kfree(result_table->unmap_addrs);
			kfree(result_table->entry_tables);
			kfree(result_table);
			return NULL;
		}
	}

	memset(&cb->result_desc, 0, sizeof(struct result_desc));
	ret = ib_post_recv(cb->qp, &cb->result_recv_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post recv error: %d\n", 
		       ret);
	}

	pr_info("Received result table with %d merge trials\n", result_table->total_cnt);

	return result_table;
}

int ksm_rdma_reg_mr(struct ksm_cb* cb, struct ib_mr* mr, int access) {
	struct ib_reg_wr reg_wr;
	const struct ib_send_wr *bad_wr = NULL;
	int err, ret;
	struct ib_wc wc;

	memset(&reg_wr, 0, sizeof(reg_wr));

	reg_wr.wr.wr_id = WR_REG_MR;
	reg_wr.wr.send_flags = IB_SEND_SIGNALED;
	reg_wr.wr.opcode = IB_WR_REG_MR;
	reg_wr.mr = mr;

	reg_wr.key = mr->rkey;
	reg_wr.access = access;

	cb->state = KSM_MEM_REG_WAIT;
	err = ib_post_send(cb->qp, &reg_wr.wr, &bad_wr);
	if (err) {
		pr_err("ib_post_send failed %d\n", err);
		return err;
	}

	memset(&wc, 0, sizeof(wc));
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "reg mr cq completion failed with "
				       "wr_id %s(%Lx) status %d opcode %d vender_err %x\n\n",
					   ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				debug_stop();
			}
		} else {
			DEBUG_LOG("reg mr cq completion with wr_id %s(%Lx) status %d opcode %d bytes %d\n",
				ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.status, wc.opcode, wc.byte_len);
		}

		if (wc.wr_id == WR_REG_MR) {
			DEBUG_LOG("IB_WC_REG_MR: %d", IB_WC_REG_MR);
			cb->state = KSM_MEM_REG_COMPLETE;
			return err;
		} else {
			printk(KERN_ERR PFX "reg mr cq completion with unexpected wr_id %s(%Lx) status %d opcode %d vender_err %x\n\n",
				ksm_wr_tag_str(wc.wr_id), wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
			debug_stop();
		}
	}

	// wait_event_interruptible(cb->sem, cb->state >= KSM_MEM_REG_COMPLETE);
	// if (cb->state != KSM_MEM_REG_COMPLETE) {
	// 	pr_err("wait for MEM_REG_COMPLETE state %d\n", cb->state);
	// 	return -1;
	// }

	return err;
}

int ksm_rdma_invalidate_mr(struct ksm_cb* cb, struct ib_mr* mr) {
	struct ib_send_wr invl_wr;
	const struct ib_send_wr *bad_wr = NULL;
	int err;

	memset(&invl_wr, 0, sizeof(invl_wr));

	invl_wr.wr_id = WR_INVALIDATE_MR;
	// invl_wr.send_flags = IB_SEND_SIGNALED;
	invl_wr.opcode = IB_WR_LOCAL_INV;
	invl_wr.ex.invalidate_rkey = mr->rkey;

	cb->state = KSM_MR_INVALIDATE_WAIT;
	err = ib_post_send(cb->qp, &invl_wr, &bad_wr);
	if (err) {
		pr_err("ib_post_send failed %d\n", err);
		return err;
	}

	return err;
}


void ksm_rdma_print_timer(void) {
	// PRINT_TIMER(table_read_time, "Table Read and Parsing Time");
	// RESET_TIMER(table_read_time);
	return;
}

static int __init client_bridge_init(void)
{
	if (ksm_offload_mode == 0) 
		pr_info("client_bridge installed: Mode: No offload");
	else if (ksm_offload_mode == 1)
		pr_info("client_bridge installed: Mode: STYX");
	else
		pr_info("client_bridge installed: Mode: BASK");
//	DEBUG_LOG("client_bridge_init\n");
	
	return 0;
}

static void __exit client_bridge_exit(void)
{
	DEBUG_LOG("client_bridge_exit\n");
}


static struct scatterlist *styx_memcmp_sgt = NULL;
static struct ib_mr* styx_memcmp_mr = NULL;
static struct scatterlist *styx_hash_sgt = NULL;
static struct ib_mr* styx_hash_mr = NULL;

int ksm_rdma_styx_memcmp(struct ksm_cb* cb, struct page *page1, struct page *page2) {
	const struct ib_send_wr *bad_send_wr;
	const struct ib_recv_wr *bad_recv_wr;
	int nents, err;
	int result;

	DEBUG_TIME_START(total_memcmp_time);

	if (!styx_memcmp_sgt) {
		styx_memcmp_sgt = kzalloc(sizeof(struct scatterlist) * 2, GFP_KERNEL);
		if (!styx_memcmp_sgt) {
			pr_err("Failed to allocate sg_table\n");
			return -1;
		}
	}
	if (!styx_memcmp_mr) {
		styx_memcmp_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, 2);
		if (IS_ERR(styx_memcmp_mr)) {
			pr_err("Failed to allocated mr");
			return -1;
		}
	}

	DEBUG_TIME_START(rdma_send_time);

	memset(styx_memcmp_sgt, 0, sizeof(struct scatterlist) * 2);
	// sg_set_page(&styx_memcmp_sgt[0], page1, PAGE_SIZE, 0);
	// sg_set_page(&styx_memcmp_sgt[1], page2, PAGE_SIZE, 0);
	styx_memcmp_sgt[0].length = PAGE_SIZE;
	styx_memcmp_sgt[0].offset = 0;
	styx_memcmp_sgt[0].dma_address = page_to_phys(page1);
	styx_memcmp_sgt[0].dma_length = PAGE_SIZE;

	styx_memcmp_sgt[1].length = PAGE_SIZE;
	styx_memcmp_sgt[1].offset = 0;
	styx_memcmp_sgt[1].dma_address = page_to_phys(page2);
	styx_memcmp_sgt[1].dma_length = PAGE_SIZE;

	sg_mark_end(&styx_memcmp_sgt[1]);

	nents = 2;

	// nents = ib_dma_map_sg(styx_memcmp_mr->device, styx_memcmp_sgt, 2, DMA_BIDIRECTIONAL);
	// if (nents <= 0) {
	// 	pr_err("Failed to map sg_table %d\n", nents);
	// }

	err = ib_map_mr_sg(styx_memcmp_mr, styx_memcmp_sgt, nents, NULL, PAGE_SIZE);
	if (err != nents) {
		pr_err("ib_map_mr_sg failed %d vs %d\n", err, nents);
	}

	err = ksm_rdma_reg_mr(cb, styx_memcmp_mr, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ);
	if (err) {
		pr_err("Failed to register mr: %d\n", err);
	}

	cb->single_op_desc_tx.cmd = PAGE_COMPARE;
	cb->single_op_desc_tx.id = iteration++;
	cb->single_op_desc_tx.page_num = 2;
	cb->single_op_desc_tx.iova = styx_memcmp_mr->iova;
	cb->single_op_desc_tx.rkey = styx_memcmp_mr->rkey;

	cb->state = KSM_CONNECTED;
	err = ib_post_send(cb->qp, &cb->single_op_send_wr, &bad_send_wr);
	if (err) {
		printk(KERN_ERR PFX "ib_post_send failed: %d\n", err);
		debug_stop();
	}

	DEBUG_TIME_END(rdma_send_time);

	DEBUG_TIME_START(rdma_wait_time);
	wait_event_interruptible(cb->sem, cb->state >= KSM_RDMA_RECV_COMPLETE);
	DEBUG_TIME_END(rdma_wait_time);
	
	DEBUG_TIME_END(irq_switch_time);
	DEBUG_TIME_START(rdma_recv_time);
	if (cb->state != KSM_RDMA_RECV_COMPLETE) {
		printk(KERN_ERR PFX "wait for RECV_COMPLETE state %d\n",
			cb->state);
		debug_stop();
	}

	result = cb->single_op_result_rx.value;

	memset(&cb->single_op_result_rx, 0, sizeof(struct operation_result));
	err = ib_post_recv(cb->qp, &cb->single_op_recv_wr, &bad_recv_wr);
	if (err) {
		printk(KERN_ERR PFX "post recv error: %d\n", 
		       err);
	}

	ksm_rdma_invalidate_mr(cb, styx_memcmp_mr);

	// pr_info("Operation id %llu -> Result: %d", iteration - 1, result);

	// ib_dma_unmap_sg(styx_memcmp_mr->device, styx_memcmp_sgt, nents, DMA_BIDIRECTIONAL);

	DEBUG_TIME_END(rdma_recv_time);

	DEBUG_TIME_END(total_memcmp_time);

	if (iteration % 100000 == 0) {
		print_time_and_reset();
	}

	return result;
}

unsigned long long ksm_rdma_styx_hash(struct ksm_cb* cb, struct page *page) {
	const struct ib_send_wr *bad_send_wr;
	const struct ib_recv_wr *bad_recv_wr;
	int nents, err;
	unsigned long long result;

	DEBUG_TIME_START(total_hash_time);

	if (!styx_hash_sgt) {
		styx_hash_sgt = kzalloc(sizeof(struct scatterlist), GFP_KERNEL);
		if (!styx_hash_sgt) {
			pr_err("Failed to allocate sg_table\n");
			return -1;
		}
	}
	if (!styx_hash_mr) {
		styx_hash_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, 1);
		if (IS_ERR(styx_hash_mr)) {
			pr_err("Failed to allocated mr");
			return -1;
		}
	}

	DEBUG_TIME_START(rdma_send_time);

	memset(styx_hash_sgt, 0, sizeof(struct scatterlist));
	// sg_set_page(&styx_hash_sgt[0], page, PAGE_SIZE, 0);
	sg_mark_end(&styx_hash_sgt[0]);

	// nents = ib_dma_map_sg(styx_hash_mr->device, styx_hash_sgt, 1, DMA_BIDIRECTIONAL);
	// if (nents <= 0) {
	// 	pr_err("Failed to map sg_table %d\n", nents);
	// }

	styx_hash_sgt[0].length = PAGE_SIZE;
	styx_hash_sgt[0].offset = 0;
	styx_hash_sgt[0].dma_address = page_to_phys(page);
	styx_hash_sgt[0].dma_length = PAGE_SIZE;
	nents = 1;

	err = ib_map_mr_sg(styx_hash_mr, styx_hash_sgt, nents, NULL, PAGE_SIZE);
	if (err != nents) {
		pr_err("ib_map_mr_sg failed %d vs %d\n", err, nents);
		debug_stop();
	}

	err = ksm_rdma_reg_mr(cb, styx_hash_mr, IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ);
	if (err) {
		pr_err("Failed to register mr: %d\n", err);
	}

	cb->single_op_desc_tx.cmd = PAGE_HASH;
	cb->single_op_desc_tx.id = iteration++;
	cb->single_op_desc_tx.page_num = 1;
	cb->single_op_desc_tx.iova = styx_hash_mr->iova;
	cb->single_op_desc_tx.rkey = styx_hash_mr->rkey;

	cb->state = KSM_CONNECTED;
	err = ib_post_send(cb->qp, &cb->single_op_send_wr, &bad_send_wr);
	if (err) {
		printk(KERN_ERR PFX "ib_post_send failed: %d\n", err);
		debug_stop();
	}

	DEBUG_TIME_END(rdma_send_time);

	DEBUG_TIME_START(rdma_wait_time);
	wait_event_interruptible(cb->sem, cb->state >= KSM_RDMA_RECV_COMPLETE);
	DEBUG_TIME_END(rdma_wait_time);

	DEBUG_TIME_END(irq_switch_time);
	DEBUG_TIME_START(rdma_recv_time);

	if (cb->state != KSM_RDMA_RECV_COMPLETE) {
		printk(KERN_ERR PFX "wait for RECV_COMPLETE state %d\n",
			cb->state);
		debug_stop();
	}

	result = cb->single_op_result_rx.value;

	memset(&cb->single_op_result_rx, 0, sizeof(struct operation_result));
	err = ib_post_recv(cb->qp, &cb->single_op_recv_wr, &bad_recv_wr);
	if (err) {
		printk(KERN_ERR PFX "post recv error: %d\n", 
		       err);
	}

	ksm_rdma_invalidate_mr(cb, styx_hash_mr);
	// pr_info("Operation id %llu -> Result: %llx", iteration - 1, result);

	// ib_dma_unmap_sg(styx_hash_mr->device, styx_hash_sgt, nents, DMA_BIDIRECTIONAL);

	DEBUG_TIME_END(rdma_recv_time);


	DEBUG_TIME_END(total_hash_time);

	if (iteration % 100000 == 0) {
		print_time_and_reset();
	}

	return result;
}

module_init(client_bridge_init);
module_exit(client_bridge_exit);

MODULE_AUTHOR("Somebody");
MODULE_DESCRIPTION("RDMA client stub");
MODULE_LICENSE("GPL");

module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");

EXPORT_SYMBOL(ksm_rdma_create_connection);
EXPORT_SYMBOL(ksm_rdma_meta_send);
EXPORT_SYMBOL(ksm_rdma_result_recv);
EXPORT_SYMBOL(ksm_rdma_reg_mr);

EXPORT_SYMBOL(ksm_rdma_styx_memcmp);
EXPORT_SYMBOL(ksm_rdma_styx_hash);
EXPORT_SYMBOL(ksm_offload_mode);

EXPORT_SYMBOL(ksm_rdma_huge_alloc_init);
EXPORT_SYMBOL(ksm_rdma_huge_alloc);
EXPORT_SYMBOL(ksm_rdma_huge_dealloc);
