/*
 * Copyright (C) 2008-2009 QUALCOMM Incorporated.
 */

//FIXME: most allocations need not be GFP_ATOMIC
/* FIXME: management of mutexes */
/* FIXME: msm_pmem_region_lookup return values */
/* FIXME: way too many copy to/from user */
/* FIXME: does region->active mean free */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <mach/board.h>

#include <linux/fs.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/android_pmem.h>
#include <linux/poll.h>
#include <media/msm_camera.h>
#include <mach/camera.h>

#define ERR_USER_COPY(to) pr_err("%s: copy %s user\n", \
				__func__, ((to) ? "to" : "from"))
#define ERR_COPY_FROM_USER() ERR_USER_COPY(0)
#define ERR_COPY_TO_USER() ERR_USER_COPY(1)

static struct class *msm_class;
static dev_t msm_devno;
static LIST_HEAD(msm_sensors);

#define __CONTAINS(r, v, l, field) ({				\
	typeof(r) __r = r;					\
	typeof(v) __v = v;					\
	typeof(v) __e = __v + l;				\
	int res = __v >= __r->field &&				\
		__e <= __r->field + __r->len;			\
	res;							\
})

#define CONTAINS(r1, r2, field) ({				\
	typeof(r2) __r2 = r2;					\
	__CONTAINS(r1, __r2->field, __r2->len, field);		\
})

#define IN_RANGE(r, v, field) ({				\
	typeof(r) __r = r;					\
	typeof(v) __vv = v;					\
	int res = ((__vv >= __r->field) &&			\
		(__vv < (__r->field + __r->len)));		\
	res;							\
})

#define OVERLAPS(r1, r2, field) ({				\
	typeof(r1) __r1 = r1;					\
	typeof(r2) __r2 = r2;					\
	typeof(__r2->field) __v = __r2->field;			\
	typeof(__v) __e = __v + __r2->len - 1;			\
	int res = (IN_RANGE(__r1, __v, field) ||		\
		   IN_RANGE(__r1, __e, field));                 \
	res;							\
})

static int check_overlap(struct hlist_head *ptype,
			  unsigned long paddr,
			  unsigned long len)
{
	struct msm_pmem_region *region;
	struct msm_pmem_region t = { .paddr = paddr, .len = len };
	struct hlist_node *node;

	hlist_for_each_entry(region, node, ptype, list) {
		if (CONTAINS(region, &t, paddr) || CONTAINS(&t, region, paddr) ||
		    OVERLAPS(region, &t, paddr)) {
			printk(KERN_ERR
				" region (PHYS %p len %ld)"
				" clashes with registered region"
				" (paddr %p len %ld)\n",
				(void *)t.paddr, t.len,
				(void *)region->paddr, region->len);
			return -1;
		}
	}

	return 0;
}

static int msm_pmem_table_add(struct hlist_head *ptype,
	struct msm_pmem_info_t *info)
{
	struct file *file;
	unsigned long paddr;
	unsigned long vstart;
	unsigned long len;
	int rc;
	struct msm_pmem_region *region;

	rc = get_pmem_file(info->fd, &paddr, &vstart, &len, &file);
	if (rc < 0) {
		pr_err("msm_pmem_table_add: get_pmem_file fd %d error %d\n",
			info->fd, rc);
		return rc;
	}

	if (check_overlap(ptype, paddr, len) < 0)
		return -EINVAL;

	CDBG("__msm_register_pmem: type = %d, paddr = 0x%lx, vaddr = 0x%lx\n",
		info->type, paddr, (unsigned long)info->vaddr);

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return -ENOMEM;

	INIT_HLIST_NODE(&region->list);

	region->type = info->type;
	region->vaddr = info->vaddr;
	region->paddr = paddr;
	region->len = len;
	region->file = file;
	region->y_off = info->y_off;
	region->cbcr_off = info->cbcr_off;
	region->fd = info->fd;
	region->active = info->active;

	hlist_add_head(&(region->list), ptype);

	return 0;
}

/* return of 0 means failure */
static uint8_t msm_pmem_region_lookup(struct hlist_head *ptype,
	enum msm_pmem_t type, struct msm_pmem_region *reg, uint8_t maxcount)
{
	struct msm_pmem_region *region;
	struct msm_pmem_region *regptr;
	struct hlist_node *node, *n;

	uint8_t rc = 0;

	regptr = reg;

	hlist_for_each_entry_safe(region, node, n, ptype, list) {
		if (region->type == type && region->active) {
			*regptr = *region;
			rc += 1;
			if (rc >= maxcount)
				break;
			regptr++;
		}
	}

	return rc;
}

static unsigned long msm_pmem_frame_ptov_lookup(unsigned long pyaddr,
	unsigned long pcbcraddr, uint32_t *yoff, uint32_t *cbcroff, int *fd,
	struct msm_device_t *msm)
{
	struct msm_pmem_region *region;
	struct hlist_node *node, *n;
	unsigned long rc = 0;

	hlist_for_each_entry_safe(region, node, n, &msm->sync.frame, list) {
		if (pyaddr == (region->paddr + region->y_off) &&
				pcbcraddr == (region->paddr +
					      region->cbcr_off) &&
				region->active) {
			/* offset since we could pass vaddr inside
			 * a registerd pmem buffer */
			rc = (unsigned long)(region->vaddr);
			*yoff = region->y_off;
			*cbcroff = region->cbcr_off;
			*fd = region->fd;
			region->active = 0;

			return rc;
		}
	}

	return 0;
}

static unsigned long msm_pmem_stats_ptov_lookup(unsigned long addr, int *fd,
	struct msm_device_t *msm)
{
	struct msm_pmem_region *region;
	struct hlist_node *node, *n;

	hlist_for_each_entry_safe(region, node, n, &msm->sync.stats, list) {
		if (addr == region->paddr && region->active) {
			/* offset since we could pass vaddr inside a
			 * registered pmem buffer */
			*fd = region->fd;
			region->active = 0;
			return (unsigned long)(region->vaddr);
		}
	}

	return 0;
}

static unsigned long msm_pmem_frame_vtop_lookup(unsigned long buffer,
	uint32_t yoff, uint32_t cbcroff, int fd, struct msm_device_t *msm)
{
	struct msm_pmem_region *region;
	struct hlist_node *node, *n;

	hlist_for_each_entry_safe(region,
		node, n, &msm->sync.frame, list) {
		if (((unsigned long)(region->vaddr) == buffer) &&
				(region->y_off == yoff) &&
				(region->cbcr_off == cbcroff) &&
				(region->fd == fd) &&
				(region->active == 0)) {

			region->active = 1;
			return region->paddr;
		}
	}

	return 0;
}

static unsigned long msm_pmem_stats_vtop_lookup(unsigned long buffer,
	int fd, struct msm_device_t *msm)
{
	struct msm_pmem_region *region;
	struct hlist_node *node, *n;

	hlist_for_each_entry_safe(region, node, n, &msm->sync.stats, list) {
		if (((unsigned long)(region->vaddr) == buffer) &&
				(region->fd == fd) && region->active == 0) {
			region->active = 1;
			return region->paddr;
		}
	}

	return 0;
}

static int __msm_pmem_table_del(struct msm_pmem_info_t *pinfo,
	struct msm_device_t *msm)
{
	int rc = 0;
	struct msm_pmem_region *region;
	struct hlist_node *node, *n;

	switch (pinfo->type) {
	case MSM_PMEM_OUTPUT1:
	case MSM_PMEM_OUTPUT2:
	case MSM_PMEM_THUMBAIL:
	case MSM_PMEM_MAINIMG:
	case MSM_PMEM_RAW_MAINIMG:
		hlist_for_each_entry_safe(region, node, n,
			&msm->sync.frame, list) {

			if (pinfo->type == region->type &&
					pinfo->vaddr == region->vaddr &&
					pinfo->fd == region->fd) {
				hlist_del(node);
				put_pmem_file(region->file);
				kfree(region);
			}
		}
		break;

	case MSM_PMEM_AEC_AWB:
	case MSM_PMEM_AF:
		hlist_for_each_entry_safe(region, node, n,
			&msm->sync.stats, list) {

			if (pinfo->type == region->type &&
					pinfo->vaddr == region->vaddr &&
					pinfo->fd == region->fd) {
				hlist_del(node);
				put_pmem_file(region->file);
				kfree(region);
			}
		}
		break;

	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int msm_pmem_table_del(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_pmem_info_t info;

	if (copy_from_user(&info, arg, sizeof(info))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	return __msm_pmem_table_del(&info, msm);
}

static int __msm_get_frame(struct msm_frame_t *frame,
	struct msm_device_t *msm)
{
	unsigned long flags;
	int rc = 0;

	struct msm_queue_cmd_t *qcmd = NULL;
	struct msm_vfe_phy_info *pphy;

	spin_lock_irqsave(&msm->sync.prev_frame_q_lock, flags);

	if (!list_empty(&msm->sync.prev_frame_q)) {
		qcmd = list_first_entry(&msm->sync.prev_frame_q,
			struct msm_queue_cmd_t, list);
		list_del(&qcmd->list);
	}

	spin_unlock_irqrestore(&msm->sync.prev_frame_q_lock, flags);

	if (!qcmd) {
		pr_err("%s: no preview frame.\n", __func__);
		return -EAGAIN;
	}

	pphy = (struct msm_vfe_phy_info *)(qcmd->command);

	frame->buffer =
		msm_pmem_frame_ptov_lookup(pphy->y_phy,
			pphy->cbcr_phy, &(frame->y_off),
			&(frame->cbcr_off), &(frame->fd), msm);
	if (!frame->buffer) {
		pr_err("%s: cannot get frame, invalid lookup address "
			"y=%x cbcr=%x offset=%d\n",
			__FUNCTION__,
			pphy->y_phy,
			pphy->cbcr_phy,
			frame->y_off);
		rc = -EINVAL;
	}

	CDBG("__msm_get_frame: y=0x%x, cbcr=0x%x, qcmd=0x%x, virt_addr=0x%x\n",
		pphy->y_phy, pphy->cbcr_phy, (int) qcmd, (int) frame->buffer);

	kfree(qcmd->command);
	kfree(qcmd);
	return rc;
}

static int msm_get_frame(void __user *arg,
	struct msm_device_t *msm)
{
	int rc = 0;
	struct msm_frame_t frame;

	if (copy_from_user(&frame,
				arg,
				sizeof(struct msm_frame_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	rc = __msm_get_frame(&frame, msm);
	if (rc < 0)
		return rc;

	if (msm->croplen) {
		if (frame.croplen > msm->croplen) {
			pr_err("msm_get_frame: invalid frame croplen %d\n",
				frame.croplen);
			return -EINVAL;
		}

		if (copy_to_user((void *)frame.cropinfo,
				msm->cropinfo,
				msm->croplen)) {
			ERR_COPY_TO_USER();
			return -EFAULT;
		}
	}

	if (copy_to_user((void *)arg,
				&frame, sizeof(struct msm_frame_t))) {
		ERR_COPY_TO_USER();
		rc = -EFAULT;
	}

	CDBG("Got frame!!!\n");

	return rc;
}

static int msm_enable_vfe(void __user *arg,
	struct msm_device_t *msm)
{
	int rc = -EIO;
	struct camera_enable_cmd_t cfg;

	if (copy_from_user(&cfg,
			arg,
			sizeof(struct camera_enable_cmd_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	if (msm->vfefn.vfe_enable)
		rc = msm->vfefn.vfe_enable(&cfg);

	CDBG("msm_enable_vfe: returned rc = %d\n", rc);
	return rc;
}

static int msm_disable_vfe(void __user *arg,
	struct msm_device_t *msm)
{
	int rc = -EIO;
	struct camera_enable_cmd_t cfg;

	if (copy_from_user(&cfg,
			arg,
			sizeof(struct camera_enable_cmd_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	if (msm->vfefn.vfe_disable)
		rc = msm->vfefn.vfe_disable(&cfg, NULL);

	CDBG("msm_disable_vfe: returned rc = %d\n", rc);
	return rc;
}

static int msm_control(void __user *arg,
	struct msm_device_t *msm)
{
	unsigned long flags;
	int timeout;
	int rc = 0;

	struct msm_ctrl_cmd_t ctrlcmd_t;
	struct msm_ctrl_cmd_t *ctrlcmd;
	struct msm_queue_cmd_t *qcmd = NULL;

	if (copy_from_user(&ctrlcmd_t,
				arg,
				sizeof(struct msm_ctrl_cmd_t))) {
		ERR_COPY_FROM_USER();
		rc = -EFAULT;
		goto end;
	}

	ctrlcmd = kmalloc(sizeof(struct msm_ctrl_cmd_t), GFP_ATOMIC);
	if (!ctrlcmd) {
		pr_err("msm_control: cannot allocate buffer ctrlcmd\n");
		rc = -ENOMEM;
		goto end;
	}

	ctrlcmd->value = kmalloc(ctrlcmd_t.length, GFP_ATOMIC);
	if (!ctrlcmd->value) {
		pr_err("msm_control: cannot allocate buffer ctrlcmd->value\n");
		rc = -ENOMEM;
		goto no_mem;
	}

	if (copy_from_user(ctrlcmd->value,
				ctrlcmd_t.value,
				ctrlcmd_t.length)) {
		ERR_COPY_FROM_USER();
		rc = -EFAULT;
		goto fail;
	}

	ctrlcmd->type = ctrlcmd_t.type;
	ctrlcmd->length = ctrlcmd_t.length;

	qcmd = kmalloc(sizeof(struct msm_queue_cmd_t), GFP_ATOMIC);
	if (!qcmd) {
		rc = -ENOMEM;
		goto fail;
	}

	spin_lock_irqsave(&msm->sync.msg_event_queue_lock, flags);
	qcmd->type = MSM_CAM_Q_CTRL;
	qcmd->command = ctrlcmd;
	list_add_tail(&qcmd->list, &msm->sync.msg_event_queue);
	/* wake up config thread */
	wake_up(&msm->sync.msg_event_wait);
	spin_unlock_irqrestore(&msm->sync.msg_event_queue_lock, flags);

	/* wait for config status */
	timeout = (int)ctrlcmd_t.timeout_ms;
	CDBG("msm_control, timeout = %d\n", timeout);
	if (timeout > 0) {
		rc = wait_event_timeout(msm->sync.ctrl_status_wait,
					!list_empty_careful(
						&msm->sync.ctrl_status_queue),
					msecs_to_jiffies(timeout));

		CDBG("msm_control: rc = %d\n", rc);

		if (rc == 0) {
			CDBG("msm_control: timed out\n");
			rc = -ETIMEDOUT;
			goto fail;
		}
	} else
		rc = wait_event_interruptible(msm->sync.ctrl_status_wait,
			!list_empty_careful(&msm->sync.ctrl_status_queue));

	if (rc < 0) {
		pr_err("msm_control: wait_event error %d\n", rc);
		rc = -EAGAIN;
		goto fail;
	}

	/* control command status is ready */
	spin_lock_irqsave(&msm->sync.ctrl_status_lock, flags);
	if (!list_empty(&msm->sync.ctrl_status_queue)) {
		qcmd = list_first_entry(&msm->sync.ctrl_status_queue,
			struct msm_queue_cmd_t, list);
		list_del(&qcmd->list);
	}
	spin_unlock_irqrestore(&msm->sync.ctrl_status_lock, flags);

	if (!qcmd->command) {
		ctrlcmd_t.type = 0xFFFF;
		ctrlcmd_t.length = 0xFFFF;
		ctrlcmd_t.status = 0xFFFF;
	} else {
		CDBG("msm_control: length = %d\n",
			((struct msm_ctrl_cmd_t *)(qcmd->command))->length);
		ctrlcmd_t.type =
			((struct msm_ctrl_cmd_t *)(qcmd->command))->type;

		ctrlcmd_t.length =
			((struct msm_ctrl_cmd_t *)(qcmd->command))->length;

		ctrlcmd_t.status =
			((struct msm_ctrl_cmd_t *)(qcmd->command))->status;

		if (ctrlcmd_t.length > 0) {
			if (copy_to_user(ctrlcmd_t.value,
					((struct msm_ctrl_cmd_t *)
						(qcmd->command))->value,
					((struct msm_ctrl_cmd_t *)
						(qcmd->command))->length)) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
				goto end;
			}

			kfree(((struct msm_ctrl_cmd_t *)
				(qcmd->command))->value);
		}

		if (copy_to_user((void *)arg,
				&ctrlcmd_t,
				sizeof(struct msm_ctrl_cmd_t))) {
			ERR_COPY_TO_USER();
			rc = -EFAULT;
			goto end;
		}
	}

	goto end;

fail:
	kfree(ctrlcmd->value);

no_mem:
	kfree(ctrlcmd);

end:
	if (qcmd) {
		kfree(qcmd->command);
		kfree(qcmd);
	}

	CDBG("msm_control: end rc = %d\n", rc);
	return rc;
}

static int msm_stats_pending(struct msm_device_t *msm)
{
	int yes = 0;

	struct msm_queue_cmd_t *qcmd = NULL;

	if (!list_empty(&msm->sync.msg_event_queue)) {
		qcmd = list_first_entry(&msm->sync.msg_event_queue,
			struct msm_queue_cmd_t, list);
		yes = (qcmd->type == MSM_CAM_Q_CTRL) ||
			(qcmd->type  == MSM_CAM_Q_VFE_EVT) ||
			(qcmd->type  == MSM_CAM_Q_VFE_MSG) ||
			(qcmd->type  == MSM_CAM_Q_V4L2_REQ);
	}

	CDBG("msm_stats_pending: %d\n", yes);
	return yes;
}

static int msm_get_stats(void __user *arg,
	struct msm_device_t *msm)
{
	unsigned long flags;
	int           timeout;
	long          rc = 0;

	struct msm_stats_event_ctrl se;

	struct msm_queue_cmd_t *qcmd = NULL;
	struct msm_ctrl_cmd_t  *ctrl = NULL;
	struct msm_vfe_resp_t  *data = NULL;
	struct msm_stats_buf_t stats;

	if (copy_from_user(&se, arg,
			sizeof(struct msm_stats_event_ctrl))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	timeout = (int)se.timeout_ms;

	if (timeout > 0) {
		rc =
			wait_event_timeout(
				msm->sync.msg_event_wait,
				msm_stats_pending(msm),
				msecs_to_jiffies(timeout));

		if (rc == 0) {
			pr_err("msm_get_stats, timeout\n");
			return -ETIMEDOUT;
		}
	} else {
		rc = wait_event_interruptible(msm->sync.msg_event_wait,
			msm_stats_pending(msm));
	}

	if (rc < 0) {
		pr_err("msm_get_stats, rc = %ld\n", rc);
		return -ERESTARTSYS;
	}

	spin_lock_irqsave(&msm->sync.msg_event_queue_lock, flags);
	if (!list_empty(&msm->sync.msg_event_queue)) {
		qcmd = list_first_entry(&msm->sync.msg_event_queue,
				struct msm_queue_cmd_t, list);
		list_del(&qcmd->list);
	}
	spin_unlock_irqrestore(&msm->sync.msg_event_queue_lock, flags);

	CDBG("=== received from DSP === %d\n", qcmd->type);

	switch (qcmd->type) {
	case MSM_CAM_Q_VFE_EVT:
	case MSM_CAM_Q_VFE_MSG:
		data = (struct msm_vfe_resp_t *)(qcmd->command);

		/* adsp event and message */
		se.resptype = MSM_CAM_RESP_STAT_EVT_MSG;

		/* 0 - msg from aDSP, 1 - event from mARM */
		se.stats_event.type   = data->evt_msg.type;
		se.stats_event.msg_id = data->evt_msg.msg_id;
		se.stats_event.len    = data->evt_msg.len;

		CDBG("msm_get_stats, qcmd->type = %d\n", qcmd->type);
		CDBG("length = %d\n", se.stats_event.len);
		CDBG("msg_id = %d\n", se.stats_event.msg_id);

		if ((data->type == VFE_MSG_STATS_AF) ||
				(data->type == VFE_MSG_STATS_WE)) {

			stats.buffer =
			msm_pmem_stats_ptov_lookup(data->phy.sbuf_phy,
				&(stats.fd), msm);
			if (!stats.buffer) {
				pr_err("%s: msm_pmem_stats_ptov_lookup failed\n",
					__FUNCTION__);
				rc = -EINVAL;
				goto failure;
			}

			if (copy_to_user((void *)(se.stats_event.data),
					&stats,
					sizeof(struct msm_stats_buf_t))) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
				goto failure;
			}
		} else if ((data->evt_msg.len > 0) &&
				(data->type == VFE_MSG_GENERAL)) {
			if (copy_to_user((void *)(se.stats_event.data),
					data->evt_msg.data,
					data->evt_msg.len)) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
			}
		} else if (data->type == VFE_MSG_OUTPUT1 ||
			data->type == VFE_MSG_OUTPUT2) {
			if (copy_to_user((void *)(se.stats_event.data),
					data->extdata,
					data->extlen)) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
			}
		} else if (data->type == VFE_MSG_SNAPSHOT) {

			uint8_t pp_en = msm->pict_pp;
			struct msm_postproc_t buf;
			struct msm_pmem_region region;

			if (pp_en) {
				buf.fmnum =
					msm_pmem_region_lookup(
						&msm->sync.frame,
						MSM_PMEM_MAINIMG,
						&region, 1);

				if (buf.fmnum == 1) {
					buf.fmain.buffer =
					(unsigned long)region.vaddr;

					buf.fmain.y_off  = region.y_off;
					buf.fmain.cbcr_off = region.cbcr_off;
					buf.fmain.fd = region.fd;
				} else {
					buf.fmnum =
					msm_pmem_region_lookup(
						&msm->sync.frame,
						MSM_PMEM_RAW_MAINIMG,
						&region, 1);

					if (buf.fmnum == 1) {
						buf.fmain.path =
							MSM_FRAME_PREV_2;
						buf.fmain.buffer =
						(unsigned long)region.vaddr;

						buf.fmain.fd = region.fd;
					}
					else {
						pr_err("%s: pmem region lookup failed\n",
							__FUNCTION__);
						rc = -EINVAL;
					}
				}

				if (copy_to_user((void *)(se.stats_event.data),
						&buf,
						sizeof(struct
							msm_postproc_t))) {
					ERR_COPY_TO_USER();
					rc = -EFAULT;
					goto failure;
				}
			}
			CDBG("SNAPSHOT copy_to_user!\n");
		}
		break;

	case MSM_CAM_Q_CTRL:{
		/* control command from control thread */
		ctrl = (struct msm_ctrl_cmd_t *)(qcmd->command);

		CDBG("msm_get_stats, qcmd->type = %d\n", qcmd->type);
		CDBG("length = %d\n", ctrl->length);

		if (ctrl->length > 0) {
			if (copy_to_user((void *)(se.ctrl_cmd.value),
						ctrl->value,
						ctrl->length)) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
				goto failure;
			}
		}

		se.resptype = MSM_CAM_RESP_CTRL;

		/* what to control */
		se.ctrl_cmd.type = ctrl->type;
		se.ctrl_cmd.length = ctrl->length;
	} /* MSM_CAM_Q_CTRL */
		break;

	case MSM_CAM_Q_V4L2_REQ: {
		/* control command from v4l2 client */
		ctrl = (struct msm_ctrl_cmd_t *)(qcmd->command);

		CDBG("msm_get_stats, qcmd->type = %d\n", qcmd->type);
		CDBG("length = %d\n", ctrl->length);

		if (ctrl->length > 0) {
			if (copy_to_user((void *)(se.ctrl_cmd.value),
					ctrl->value, ctrl->length)) {
				ERR_COPY_TO_USER();
				rc = -EFAULT;
				goto failure;
			}
		}

		/* 2 tells config thread this is v4l2 request */
		se.resptype = MSM_CAM_RESP_V4L2;

		/* what to control */
		se.ctrl_cmd.type   = ctrl->type;
		se.ctrl_cmd.length = ctrl->length;
	} /* MSM_CAM_Q_V4L2_REQ */
		break;

	default:
		rc = -EFAULT;
		goto failure;
	} /* switch qcmd->type */

	if (copy_to_user((void *)arg, &se, sizeof(se))) {
		ERR_COPY_TO_USER();
		rc = -EFAULT;
	}

failure:
	if (qcmd) {

		if (qcmd->type == MSM_CAM_Q_VFE_MSG)
			kfree(((struct msm_vfe_resp_t *)
				(qcmd->command))->evt_msg.data);

		kfree(qcmd->command);
		kfree(qcmd);
	}

	CDBG("msm_get_stats: end rc = %ld\n", rc);
	return rc;
}

static int msm_ctrl_cmd_done(void __user *arg,
	struct msm_device_t *msm)
{
	unsigned long flags;
	int rc = 0;

	struct msm_ctrl_cmd_t ctrlcmd_t;
	struct msm_ctrl_cmd_t *ctrlcmd;
	struct msm_queue_cmd_t *qcmd = NULL;

	if (copy_from_user(&ctrlcmd_t,
				arg,
				sizeof(struct msm_ctrl_cmd_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	ctrlcmd = kzalloc(sizeof(struct msm_ctrl_cmd_t), GFP_ATOMIC);
	if (!ctrlcmd) {
		rc = -ENOMEM;
		goto end;
	}

	if (ctrlcmd_t.length > 0) {
		ctrlcmd->value = kmalloc(ctrlcmd_t.length, GFP_ATOMIC);
		if (!ctrlcmd->value) {
			rc = -ENOMEM;
			goto no_mem;
		}

		if (copy_from_user(ctrlcmd->value,
					(void *)ctrlcmd_t.value,
					ctrlcmd_t.length)) {
			ERR_COPY_FROM_USER();
			rc = -EFAULT;
			goto fail;
		}
	} else
		ctrlcmd->value = NULL;

	ctrlcmd->type = ctrlcmd_t.type;
	ctrlcmd->length = ctrlcmd_t.length;
	ctrlcmd->status = ctrlcmd_t.status;

	qcmd = kmalloc(sizeof(*qcmd), GFP_ATOMIC);
	if (!qcmd) {
		rc = -ENOMEM;
		goto fail;
	}

	qcmd->command = (void *)ctrlcmd;

	goto end;

fail:
	kfree(ctrlcmd->value);
no_mem:
	kfree(ctrlcmd);
end:
	CDBG("msm_ctrl_cmd_done: end rc = %d\n", rc);
	if (rc == 0) {
		/* wake up control thread */
		spin_lock_irqsave(&msm->sync.ctrl_status_lock, flags);
		list_add_tail(&qcmd->list, &msm->sync.ctrl_status_queue);
		spin_unlock_irqrestore(&msm->sync.ctrl_status_lock, flags);
		wake_up(&msm->sync.ctrl_status_wait);
	}

	return rc;
}

static int msm_config_vfe(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_vfe_cfg_cmd_t cfgcmd;
	struct msm_pmem_region region[8];
	struct axidata_t axi_data;
	void *data = NULL;
	int rc = -EIO;

	memset(&axi_data, 0, sizeof(axi_data));

	if (copy_from_user(&cfgcmd, arg, sizeof(cfgcmd))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	switch(cfgcmd.cmd_type) {
	case CMD_STATS_ENABLE:
		axi_data.bufnum1 =
			msm_pmem_region_lookup(&msm->sync.stats,
					MSM_PMEM_AEC_AWB, &region[0],
					NUM_WB_EXP_STAT_OUTPUT_BUFFERS);
		if (!axi_data.bufnum1) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		axi_data.region = &region[0];
		data = &axi_data;
		break;
	case CMD_STATS_AF_ENABLE:
		axi_data.bufnum1 =
			msm_pmem_region_lookup(&msm->sync.stats,
					MSM_PMEM_AF, &region[0],
					NUM_AF_STAT_OUTPUT_BUFFERS);
		if (!axi_data.bufnum1) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		axi_data.region = &region[0];
		data = &axi_data;
		break;
	case CMD_GENERAL:
	case CMD_STATS_DISABLE:
		break;
	default:
		pr_err("%s: unknown command type %d\n",
			__FUNCTION__, cfgcmd.cmd_type);
		return -EINVAL;
	}


	if (msm->vfefn.vfe_config)
		rc = msm->vfefn.vfe_config(&cfgcmd, data);

	return rc;
}

static int msm_frame_axi_cfg(struct msm_vfe_cfg_cmd_t *cfgcmd,
	struct msm_device_t *msm)
{
	int rc = -EIO;
	struct axidata_t axi_data;
	void *data = &axi_data;
	struct msm_pmem_region region[8];
	enum msm_pmem_t mtype;

	memset(&axi_data, 0, sizeof(axi_data));

	switch (cfgcmd->cmd_type) {
	case CMD_AXI_CFG_OUT1:
		mtype = MSM_PMEM_OUTPUT1;
		axi_data.bufnum1 =
			msm_pmem_region_lookup(&msm->sync.frame, mtype,
				&region[0], 8);
		if (!axi_data.bufnum1) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		break;

	case CMD_AXI_CFG_OUT2:
		mtype = MSM_PMEM_OUTPUT2;
		axi_data.bufnum2 =
			msm_pmem_region_lookup(&msm->sync.frame, mtype,
				&region[0], 8);
		if (!axi_data.bufnum2) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		break;

	case CMD_AXI_CFG_SNAP_O1_AND_O2:
		mtype = MSM_PMEM_THUMBAIL;
		axi_data.bufnum1 =
			msm_pmem_region_lookup(&msm->sync.frame, mtype,
				&region[0], 8);
		if (!axi_data.bufnum1) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}

		mtype = MSM_PMEM_MAINIMG;
		axi_data.bufnum2 =
			msm_pmem_region_lookup(&msm->sync.frame, mtype,
				&region[axi_data.bufnum1], 8);
		if (!axi_data.bufnum2) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		break;

	case CMD_RAW_PICT_AXI_CFG:
		mtype = MSM_PMEM_RAW_MAINIMG;
		axi_data.bufnum2 =
			msm_pmem_region_lookup(&msm->sync.frame, mtype,
				&region[0], 8);
		if (!axi_data.bufnum2) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		break;

	case CMD_GENERAL:
		data = NULL;
		break;

	default:
		pr_err("%s: unknown command type %d\n",
			__FUNCTION__, cfgcmd->cmd_type);
		return -EINVAL;
	}

	axi_data.region = &region[0];

	/* send the AXI configuration command to driver */
	if (msm->vfefn.vfe_config)
		rc = msm->vfefn.vfe_config(cfgcmd, data);

	return rc;
}

static int msm_get_sensor_info(void __user *arg,
	struct msm_device_t *msm)
{
	int rc = 0;
	struct msm_camsensor_info_t info;
	struct msm_camera_sensor_info *sdata;
	struct msm_camera_device_platform_data *pdata;

	if (copy_from_user(&info,
			arg,
			sizeof(struct msm_camsensor_info_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	sdata = msm->pdev->dev.platform_data;
	pdata = sdata->pdata;
	CDBG("sensor_name %s\n", sdata->sensor_name);

	memcpy(&info.name[0],
		sdata->sensor_name,
		MAX_SENSOR_NAME);
	info.flash_enabled = sdata->flash_type != MSM_CAMERA_FLASH_NONE;

	/* copy back to user space */
	if (copy_to_user((void *)arg,
			&info,
			sizeof(struct msm_camsensor_info_t))) {
		ERR_COPY_TO_USER();
		rc = -EFAULT;
	}

	return rc;
}

static int __msm_put_frame_buf(struct msm_frame_t *pb,
	struct msm_device_t *msm)
{
	unsigned long pphy;
	struct msm_vfe_cfg_cmd_t cfgcmd;

	int rc = -EIO;

	pphy = msm_pmem_frame_vtop_lookup(pb->buffer,
		pb->y_off, pb->cbcr_off, pb->fd, msm);

	if (pphy != 0) {
		CDBG("rel: vaddr = 0x%lx, paddr = 0x%lx\n",
			pb->buffer, pphy);
		cfgcmd.cmd_type = CMD_FRAME_BUF_RELEASE;
		cfgcmd.value    = (void *)pb;
		if (msm->vfefn.vfe_config)
			rc = msm->vfefn.vfe_config(&cfgcmd, &pphy);
	} else {
		pr_err("%s: msm_pmem_frame_vtop_lookup failed\n",
			__FUNCTION__);
		rc = -EINVAL;
	}

	return rc;
}

static int msm_put_frame_buffer(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_frame_t buf_t;

	if (copy_from_user(&buf_t,
				arg,
				sizeof(struct msm_frame_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	return __msm_put_frame_buf(&buf_t, msm);
}

static int __msm_register_pmem(struct msm_pmem_info_t *pinfo,
	struct msm_device_t *msm)
{
	int rc = 0;

	switch (pinfo->type) {
	case MSM_PMEM_OUTPUT1:
	case MSM_PMEM_OUTPUT2:
	case MSM_PMEM_THUMBAIL:
	case MSM_PMEM_MAINIMG:
	case MSM_PMEM_RAW_MAINIMG:
		rc = msm_pmem_table_add(&msm->sync.frame, pinfo);
		break;

	case MSM_PMEM_AEC_AWB:
	case MSM_PMEM_AF:
		rc = msm_pmem_table_add(&msm->sync.stats, pinfo);
		break;

	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int msm_register_pmem(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_pmem_info_t info;

	if (copy_from_user(&info, arg, sizeof(info))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	return __msm_register_pmem(&info, msm);
}

static int msm_stats_axi_cfg(struct msm_vfe_cfg_cmd_t *cfgcmd,
	struct msm_device_t *msm)
{
	int rc = -EIO;
	struct axidata_t axi_data;
	void *data = &axi_data;

	struct msm_pmem_region region[3];
	enum msm_pmem_t mtype = MSM_PMEM_MAX;

	memset(&axi_data, 0, sizeof(axi_data));

	switch (cfgcmd->cmd_type) {
	case CMD_STATS_AXI_CFG:
		mtype = MSM_PMEM_AEC_AWB;
		break;
	case CMD_STATS_AF_AXI_CFG:
		mtype = MSM_PMEM_AF;
		break;
	case CMD_GENERAL:
		data = NULL;
		break;
	default:
		pr_err("%s: unknown command type %d\n",
			__FUNCTION__, cfgcmd->cmd_type);
		return -EINVAL;
	}

	if (cfgcmd->cmd_type != CMD_GENERAL) {
		axi_data.bufnum1 =
			msm_pmem_region_lookup(&msm->sync.stats, mtype,
				&region[0], NUM_WB_EXP_STAT_OUTPUT_BUFFERS);
		if (!axi_data.bufnum1) {
			pr_err("%s: pmem region lookup error\n", __FUNCTION__);
			return -EINVAL;
		}
		axi_data.region = &region[0];
	}

	/* send the AEC/AWB STATS configuration command to driver */
	if (msm->vfefn.vfe_config)
		rc = msm->vfefn.vfe_config(cfgcmd, &axi_data);

	return rc;
}

static int msm_put_stats_buffer(void __user *arg,
	struct msm_device_t *msm)
{
	int rc = -EIO;

	struct msm_stats_buf_t buf;
	unsigned long pphy;
	struct msm_vfe_cfg_cmd_t cfgcmd;

	if (copy_from_user(&buf, arg,
				sizeof(struct msm_stats_buf_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	CDBG("msm_put_stats_buffer\n");
	pphy = msm_pmem_stats_vtop_lookup(buf.buffer, buf.fd, msm);

	if (pphy != 0) {
		if (buf.type == STAT_AEAW)
			cfgcmd.cmd_type = CMD_STATS_BUF_RELEASE;
		else if (buf.type == STAT_AF)
			cfgcmd.cmd_type = CMD_STATS_AF_BUF_RELEASE;
		else {
			pr_err("%s: invalid buf type %d\n",
				__FUNCTION__,
				buf.type);
			rc = -EINVAL;
			goto put_done;
		}

		cfgcmd.value = (void *)&buf;

		if (msm->vfefn.vfe_config) {
			rc = msm->vfefn.vfe_config(&cfgcmd, &pphy);
			if (rc < 0)
				pr_err("msm_put_stats_buffer: "\
					"vfe_config err %d\n", rc);
		} else
			pr_err("msm_put_stats_buffer: vfe_config is NULL\n");
	} else {
		pr_err("msm_put_stats_buffer: NULL physical address\n");
		rc = -EINVAL;
	}

put_done:
	return rc;
}

static int msm_axi_config(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_vfe_cfg_cmd_t cfgcmd;

	if (copy_from_user(&cfgcmd, arg, sizeof(cfgcmd))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	switch (cfgcmd.cmd_type) {
	case CMD_AXI_CFG_OUT1:
	case CMD_AXI_CFG_OUT2:
	case CMD_AXI_CFG_SNAP_O1_AND_O2:
	case CMD_RAW_PICT_AXI_CFG:
		return msm_frame_axi_cfg(&cfgcmd, msm);

	case CMD_STATS_AXI_CFG:
	case CMD_STATS_AF_AXI_CFG:
		return msm_stats_axi_cfg(&cfgcmd, msm);

	default:
		pr_err("%s: unknown command type %d\n",
			__FUNCTION__,
			cfgcmd.cmd_type);
		return -EINVAL;
	}

	return 0;
}

static int msm_camera_pict_pending(struct msm_device_t *msm)
{
	int yes = 0;
	if (!list_empty_careful(&msm->sync.pict_frame_q)) {
		struct msm_queue_cmd_t *qcmd =
			list_first_entry(&msm->sync.pict_frame_q,
				struct msm_queue_cmd_t, list);
		yes = (qcmd->type == MSM_CAM_Q_VFE_MSG);
	}

	CDBG("msm_camera_pict_pending: %d\n", yes);
	return yes;
}

static int __msm_get_pic(struct msm_ctrl_cmd_t *ctrl,
	struct msm_device_t *msm)
{
	unsigned long flags;
	int rc = 0;
	int tm;

	struct msm_queue_cmd_t *qcmd = NULL;

	tm = (int)ctrl->timeout_ms;

	if (tm > 0) {
		rc =
			wait_event_timeout(
				msm->sync.pict_frame_wait,
				msm_camera_pict_pending(msm),
				msecs_to_jiffies(tm));

		if (rc == 0) {
			CDBG("msm_camera_get_picture, tm\n");
			return -ETIMEDOUT;
		}
	} else
		rc = wait_event_interruptible(
					msm->sync.pict_frame_wait,
					msm_camera_pict_pending(msm));

	if (rc < 0) {
		pr_err("msm_camera_get_picture, rc = %d\n", rc);
		return -ERESTARTSYS;
	}

	spin_lock_irqsave(&msm->sync.pict_frame_q_lock, flags);
	if (!list_empty(&msm->sync.pict_frame_q)) {
		qcmd = list_first_entry(&msm->sync.pict_frame_q,
			struct msm_queue_cmd_t, list);
		list_del(&qcmd->list);
	}
	spin_unlock_irqrestore(&msm->sync.pict_frame_q_lock, flags);

	if (qcmd->command != NULL) {
		ctrl->type =
		((struct msm_ctrl_cmd_t *)(qcmd->command))->type;

		ctrl->status =
		((struct msm_ctrl_cmd_t *)(qcmd->command))->status;

		kfree(qcmd->command);
	} else {
		ctrl->type = 0xFFFF;
		ctrl->status = 0xFFFF;
	}

	kfree(qcmd);

	return rc;
}

static int msm_get_pic(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_ctrl_cmd_t ctrlcmd_t;
	int rc;

	if (copy_from_user(&ctrlcmd_t,
				arg,
				sizeof(struct msm_ctrl_cmd_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	rc = __msm_get_pic(&ctrlcmd_t, msm);
	if (rc < 0)
		return rc;

	if (msm->croplen) {
		if (ctrlcmd_t.length < msm->croplen) {
			pr_err("msm_get_pic: invalid len %d\n",
				ctrlcmd_t.length);
			return -EINVAL;
		}
		if (copy_to_user(ctrlcmd_t.value,
				msm->cropinfo,
				msm->croplen)) {
			ERR_COPY_TO_USER();
			return -EFAULT;
		}
	}

	if (copy_to_user((void *)arg,
		&ctrlcmd_t,
		sizeof(struct msm_ctrl_cmd_t))) {
		ERR_COPY_TO_USER();
		return -EFAULT;
	}
	return 0;
}

static int msm_set_crop(void __user *arg,
	struct msm_device_t *msm)
{
	struct crop_info_t crop;

	if (copy_from_user(&crop,
				arg,
				sizeof(struct crop_info_t))) {
		ERR_COPY_FROM_USER();
		return -EFAULT;
	}

	if (!msm->croplen) {
		msm->cropinfo = kmalloc(crop.len, GFP_KERNEL);

		if (!msm->cropinfo)
			return -ENOMEM;
	} else if (msm->croplen < crop.len)
		return -EINVAL;

	if (copy_from_user(msm->cropinfo,
				crop.info,
				crop.len)) {
		ERR_COPY_FROM_USER();
		kfree(msm->cropinfo);
		return -EFAULT;
	}

	msm->croplen = crop.len;

	return 0;
}

static int msm_pict_pp_done(void __user *arg,
	struct msm_device_t *msm)
{
	struct msm_ctrl_cmd_t ctrlcmd_t;
	struct msm_ctrl_cmd_t *ctrlcmd = NULL;
	struct msm_queue_cmd_t *qcmd = NULL;
	unsigned long flags;
	int rc = 0;

	uint8_t pp_en = msm->pict_pp;

	if (!pp_en)
		return -EINVAL;

	if (copy_from_user(&ctrlcmd_t,
				arg,
				sizeof(struct msm_ctrl_cmd_t))) {
		ERR_COPY_FROM_USER();
		rc = -EFAULT;
		goto pp_done;
	}

	qcmd =
		kmalloc(sizeof(struct msm_queue_cmd_t),
						GFP_ATOMIC);
	if (!qcmd) {
		rc = -ENOMEM;
		goto pp_fail;
	}

	ctrlcmd = kzalloc(sizeof(struct msm_ctrl_cmd_t), GFP_ATOMIC);
	if (!ctrlcmd) {
		rc = -ENOMEM;
		goto pp_done;
	}

	ctrlcmd->type = ctrlcmd_t.type;
	ctrlcmd->status = ctrlcmd_t.status;

pp_done:
	qcmd->type = MSM_CAM_Q_VFE_MSG;
	qcmd->command = ctrlcmd;

	spin_lock_irqsave(&msm->sync.pict_frame_q_lock, flags);
	list_add_tail(&qcmd->list, &msm->sync.pict_frame_q);
	spin_unlock_irqrestore(&msm->sync.pict_frame_q_lock, flags);
	wake_up(&msm->sync.pict_frame_wait);

pp_fail:
	return rc;
}

static long msm_ioctl_common(struct msm_device_t *pmsm,
				unsigned int cmd,
				void __user *argp)
{
	CDBG("msm_ioctl_common\n");
	switch (cmd) {
	case MSM_CAM_IOCTL_REGISTER_PMEM:
		return msm_register_pmem(argp, pmsm);
	case MSM_CAM_IOCTL_UNREGISTER_PMEM:
		return msm_pmem_table_del(argp, pmsm);
	default:
		return -EINVAL;
	}
}

static long msm_ioctl_config(struct file *filep, unsigned int cmd,
	unsigned long arg)
{
	int rc = -EINVAL;
	void __user *argp = (void __user *)arg;
	struct msm_device_t *pmsm = filep->private_data;

	CDBG("!!! msm_ioctl !!!, pmsm = %p, cmd = %d\n", pmsm, _IOC_NR(cmd));
	mutex_lock(&pmsm->lock);

	switch (cmd) {
	case MSM_CAM_IOCTL_GET_SENSOR_INFO:
		rc = msm_get_sensor_info(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_CONFIG_VFE:
		/* Coming from config thread for update */
		rc = msm_config_vfe(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_GET_STATS:
		/* Coming from config thread wait
		 * for vfe statistics and control requests */
		rc = msm_get_stats(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_ENABLE_VFE:
		/* This request comes from control thread:
		 * enable either QCAMTASK or VFETASK */
		rc = msm_enable_vfe(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_DISABLE_VFE:
		/* This request comes from control thread:
		 * disable either QCAMTASK or VFETASK */
		rc = msm_disable_vfe(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_CTRL_CMD_DONE:
		/* Config thread notifies the result of contrl command */
		rc = msm_ctrl_cmd_done(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_VFE_APPS_RESET:
		msm_camio_vfe_blk_reset();
		rc = 0;
		break;

	case MSM_CAM_IOCTL_RELEASE_STATS_BUFFER:
		rc = msm_put_stats_buffer(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_AXI_CONFIG:
		rc = msm_axi_config(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_SET_CROP:
		rc = msm_set_crop(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_PICT_PP: {
		uint8_t enable;
		if (copy_from_user(&enable, argp, sizeof(enable))) {
			ERR_COPY_FROM_USER();
			rc = -EFAULT;
		} else {
			pmsm->pict_pp = enable;
			rc = 0;
		}
		break;
	}

	case MSM_CAM_IOCTL_PICT_PP_DONE:
		rc = msm_pict_pp_done(argp, pmsm);
		break;

	case MSM_CAM_IOCTL_SENSOR_IO_CFG:
		rc = pmsm->sctrl.s_config(argp);
		break;

	case MSM_CAM_IOCTL_FLASH_LED_CFG: {
		uint32_t led_state;
		if (copy_from_user(&led_state, argp, sizeof(led_state))) {
			ERR_COPY_FROM_USER();
			rc = -EFAULT;
		} else
			rc = msm_camera_flash_set_led_state(led_state);
		break;
	}

	default:
		rc = msm_ioctl_common(pmsm, cmd, argp);
		break;
	}

	mutex_unlock(&pmsm->lock);
	CDBG("!!! msm_ioctl !!!, pmsm = %p, cmd = %d DONE\n",
			pmsm, _IOC_NR(cmd));
	return rc;
}

static long msm_ioctl_frame(struct file *filep, unsigned int cmd,
	unsigned long arg)
{
	int rc = -EINVAL;
	void __user *argp = (void __user *)arg;
	struct msm_device_t *pmsm = filep->private_data;

	mutex_lock(&pmsm->lock);

	switch (cmd) {
	case MSM_CAM_IOCTL_GETFRAME:
		/* Coming from frame thread to get frame
		 * after SELECT is done */
		rc = msm_get_frame(argp, pmsm);
		break;
	case MSM_CAM_IOCTL_RELEASE_FRAME_BUFFER:
		rc = msm_put_frame_buffer(argp, pmsm);
		break;
	default:
		break;
	}

	mutex_unlock(&pmsm->lock);
	return rc;
}


static long msm_ioctl_control(struct file *filep, unsigned int cmd,
	unsigned long arg)
{
	int rc = -EINVAL;
	void __user *argp = (void __user *)arg;
	struct msm_device_t *pmsm = filep->private_data;

	mutex_lock(&pmsm->lock);

	switch (cmd) {
	case MSM_CAM_IOCTL_CTRL_COMMAND:
		/* Coming from control thread, may need to wait for
		* command status */
		rc = msm_control(argp, pmsm);
		break;
	case MSM_CAM_IOCTL_GET_PICTURE:
		rc = msm_get_pic(argp, pmsm);
		break;
	default:
		rc = msm_ioctl_common(pmsm, cmd, argp);
		break;
	}

	mutex_unlock(&pmsm->lock);
	return rc;
}

static int msm_release_common(struct msm_device_t *pmsm)
{
	struct msm_pmem_region *region;
	struct hlist_node *hnode;
	struct hlist_node *n;
	struct msm_queue_cmd_t *qcmd = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pmsm->sync.pict_frame_q_lock, flags);
	while (msm_camera_pict_pending(pmsm)) {
		/* The list is guaranteed not to be empty. */
		qcmd = list_first_entry(&pmsm->sync.pict_frame_q,
			struct msm_queue_cmd_t, list);
		list_del(&qcmd->list);
		kfree(qcmd->command);
		kfree(qcmd);
	};
	spin_unlock_irqrestore(&pmsm->sync.pict_frame_q_lock, flags);

	pmsm->opencnt--;

	if (!pmsm->opencnt) {
		/* need to clean up
		 * system resource */
		if (pmsm->vfefn.vfe_release)
			pmsm->vfefn.vfe_release(pmsm->pdev);

		if (pmsm->croplen) {
			kfree(pmsm->cropinfo);
			pmsm->croplen = 0;
		}

		hlist_for_each_entry_safe(region, hnode, n,
			&pmsm->sync.frame, list) {

			hlist_del(hnode);
			put_pmem_file(region->file);
			kfree(region);
		}

		hlist_for_each_entry_safe(region, hnode, n,
			&pmsm->sync.stats, list) {

			hlist_del(hnode);
			put_pmem_file(region->file);
			kfree(region);
		}

		spin_lock_irqsave(&pmsm->sync.ctrl_status_lock, flags);
		while (!list_empty(&pmsm->sync.ctrl_status_queue)) {
			/* List is guaranteed not to be empty */
			qcmd = list_first_entry(&pmsm->sync.ctrl_status_queue,
				struct msm_queue_cmd_t, list);
			list_del(&qcmd->list);
			if (qcmd->type == MSM_CAM_Q_VFE_MSG)
				kfree(((struct msm_vfe_resp_t *)
					(qcmd->command))->evt_msg.data);
			kfree(qcmd->command);
			kfree(qcmd);
		};
		spin_unlock_irqrestore(&pmsm->sync.ctrl_status_lock, flags);

		spin_lock_irqsave(&pmsm->sync.msg_event_queue_lock, flags);
		while (msm_stats_pending(pmsm)) {
			qcmd = list_first_entry(&pmsm->sync.msg_event_queue,
				struct msm_queue_cmd_t, list);
			list_del(&qcmd->list);
			kfree(qcmd->command);
			kfree(qcmd);
		};
		spin_unlock_irqrestore(&pmsm->sync.msg_event_queue_lock, flags);

		spin_lock_irqsave(&pmsm->sync.pict_frame_q_lock, flags);
		while (msm_camera_pict_pending(pmsm)) {
			qcmd = list_first_entry(&pmsm->sync.pict_frame_q,
				struct msm_queue_cmd_t, list);
			list_del(&qcmd->list);
			kfree(qcmd->command);
			kfree(qcmd);
		};
		spin_unlock_irqrestore(&pmsm->sync.pict_frame_q_lock, flags);

		spin_lock_irqsave(&pmsm->sync.prev_frame_q_lock, flags);
		while (!list_empty(&pmsm->sync.prev_frame_q)) {
			qcmd = list_first_entry(&pmsm->sync.prev_frame_q,
				struct msm_queue_cmd_t, list);
			list_del(&qcmd->list);
			kfree(qcmd->command);
			kfree(qcmd);
		};
		spin_unlock_irqrestore(&pmsm->sync.prev_frame_q_lock, flags);

		pmsm->sctrl.s_release();
		wake_unlock(&pmsm->wake_lock);
		CDBG("msm_release completed!\n");
	}

	return 0;
}

#define DEFINE_MSM_RELEASE(type)                                      \
static int msm_release_##type(struct inode *node, struct file *filep) \
{                                                                     \
	int rc = -EIO;                                                \
	struct msm_device_t *pmsm = filep->private_data;              \
	mutex_lock(&pmsm->lock);                                      \
	if (pmsm->opened_##type) {                                    \
		rc = msm_release_common(pmsm);                        \
		pmsm->opened_##type = rc < 0;                         \
	}                                                             \
	else                                                          \
		pr_err("msm_release_"#type" is already closed.\n");   \
	mutex_unlock(&pmsm->lock);                                    \
	return rc;                                                    \
}

DEFINE_MSM_RELEASE(control);
DEFINE_MSM_RELEASE(config);
DEFINE_MSM_RELEASE(frame);

static unsigned int __msm_apps_poll(struct file *filep,
	struct poll_table_struct *pll_table, struct msm_device_t *pmsm)
{
	poll_wait(filep, &pmsm->sync.prev_frame_wait, pll_table);

	if (!list_empty_careful(&pmsm->sync.prev_frame_q))
		/* frame ready */
		return POLLIN | POLLRDNORM;

	return 0;
}

static unsigned int msm_poll(struct file *filep,
	struct poll_table_struct *pll_table)
{
	return __msm_apps_poll(filep, pll_table, filep->private_data);
}

static void msm_vfe_sync(struct msm_vfe_resp_t *vdata,
		enum msm_queut_t qtype, void *syncdata)
{
	struct msm_queue_cmd_t *qcmd = NULL;
	struct msm_queue_cmd_t *qcmd_frame = NULL;
	struct msm_vfe_phy_info *fphy;

	unsigned long flags;
	struct msm_device_t *msm =
		(struct msm_device_t *)syncdata;

	if (!msm)
		return;

	qcmd = kmalloc(sizeof(struct msm_queue_cmd_t),
					GFP_ATOMIC);
	if (!qcmd) {
		pr_err("evt_msg: cannot allocate buffer\n");
		goto mem_fail1;
	}

	if (qtype == MSM_CAM_Q_VFE_EVT) {
		qcmd->type    = MSM_CAM_Q_VFE_EVT;
	} else if (qtype == MSM_CAM_Q_VFE_MSG) {

		if (vdata->type == VFE_MSG_OUTPUT1 ||
				vdata->type == VFE_MSG_OUTPUT2) {

			qcmd_frame =
				kmalloc(sizeof(struct msm_queue_cmd_t),
					GFP_ATOMIC);
			if (!qcmd_frame)
				goto mem_fail2;
			fphy = kmalloc(sizeof(struct msm_vfe_phy_info),
				GFP_ATOMIC);
			if (!fphy)
				goto mem_fail3;

			*fphy = vdata->phy;

			qcmd_frame->type    = MSM_CAM_Q_VFE_MSG;
			qcmd_frame->command = fphy;

			CDBG("qcmd_frame= 0x%x phy_y= 0x%x, phy_cbcr= 0x%x\n",
			(int) qcmd_frame, fphy->y_phy, fphy->cbcr_phy);

			spin_lock_irqsave(&msm->sync.prev_frame_q_lock,
				flags);

			list_add_tail(&qcmd_frame->list,
				&msm->sync.prev_frame_q);

			spin_unlock_irqrestore(&msm->sync.prev_frame_q_lock,
				flags);

			wake_up(&msm->sync.prev_frame_wait);

			CDBG("woke up frame thread\n");

		} else if (vdata->type == VFE_MSG_SNAPSHOT) {
			unsigned pp = msm->pict_pp;

			CDBG("SNAPSHOT pp = %d\n", pp);
			if (!pp) {
				qcmd_frame =
					kmalloc(sizeof(struct msm_queue_cmd_t),
						GFP_ATOMIC);
				if (!qcmd_frame)
					goto mem_fail2;

				qcmd_frame->type    = MSM_CAM_Q_VFE_MSG;
				qcmd_frame->command = NULL;

				spin_lock_irqsave(&msm->sync.pict_frame_q_lock,
					flags);

				list_add_tail(&qcmd_frame->list,
					&msm->sync.pict_frame_q);

				spin_unlock_irqrestore(
					&msm->sync.pict_frame_q_lock, flags);
				wake_up(&msm->sync.pict_frame_wait);
			}
		}

		qcmd->type = MSM_CAM_Q_VFE_MSG;
	}

	qcmd->command = (void *)vdata;
	CDBG("vdata->type = %d\n", vdata->type);

	spin_lock_irqsave(&msm->sync.msg_event_queue_lock,
		flags);
	list_add_tail(&qcmd->list, &msm->sync.msg_event_queue);
	spin_unlock_irqrestore(&msm->sync.msg_event_queue_lock,
		flags);
	wake_up(&msm->sync.msg_event_wait);
	CDBG("woke up config thread\n");

	return;

mem_fail3:
	kfree(qcmd_frame);

mem_fail2:
	kfree(qcmd);

mem_fail1:
	if (qtype == MSM_CAMERA_MSG &&
			vdata->evt_msg.len > 0)
		kfree(vdata->evt_msg.data);

	kfree(vdata);
	return;
}

static struct msm_vfe_resp msm_vfe_s = {
	.vfe_resp = msm_vfe_sync,
};

static int __msm_open(struct msm_device_t *msm)
{
	int rc = 0;
	struct msm_camera_sensor_info *sdata = msm->pdev->dev.platform_data;
	struct msm_camera_device_platform_data *pdata = sdata->pdata;

	if (!pdata) {
		pr_err("__msm_open: no pdata\n");
		rc = -ENODEV;
		goto msm_open_done;
	}

	msm->opencnt++;
	if (msm->opencnt == 1)
		wake_lock(&msm->wake_lock);

	if (msm->opencnt == 1) {
		msm_camvfe_fn_init(&msm->vfefn, msm);
		if (msm->vfefn.vfe_init) {
			rc = msm->vfefn.vfe_init(&msm_vfe_s,
				msm->pdev);
			if (rc < 0) {
				pr_err("vfe_init failed at %d\n", rc);
				goto msm_open_done;
			}
			rc = msm->sctrl.s_init(sdata);
			if (rc < 0) {
				pr_err("sensor init failed: %d\n", rc);
				goto msm_open_done;
			}
		} else {
			pr_err("no sensor init func\n");
			rc = -ENODEV;
			goto msm_open_done;
		}

		if (rc >= 0) {
			INIT_HLIST_HEAD(&msm->sync.frame);
			INIT_HLIST_HEAD(&msm->sync.stats);
		}
	} else if (msm->opencnt > 1) {
		rc = 0;
	}

msm_open_done:
	return rc;
}

static int msm_open_common(struct inode *inode,
		struct file *filep, struct msm_device_t *pmsm)
{
	int rc = 0;

	rc = nonseekable_open(inode, filep);
	if (rc < 0)
		goto cam_open_fail;

	rc = __msm_open(pmsm);
	if (rc < 0)
		goto cam_open_done;

	filep->private_data = pmsm;

cam_open_fail:
cam_open_done:
	CDBG("msm_open() open: rc = %d\n", rc);
	return rc;
}

#define DEFINE_MSM_OPEN(type)                                                 \
static int msm_open_##type(struct inode *inode, struct file *filep)           \
{                                                                             \
	int rc;                                                               \
	struct msm_device_t *pmsm =                                           \
		container_of(inode->i_cdev, struct msm_device_t, cdev_##type);\
	mutex_lock(&pmsm->lock);                                              \
	if (pmsm->opened_##type) {                                            \
		pr_err("msm_open_"#type": already opened\n");                 \
		mutex_unlock(&pmsm->lock);                                    \
		return -EBUSY;                                                \
	}                                                                     \
	rc = msm_open_common(inode, filep, pmsm);                             \
	pmsm->opened_##type = rc >= 0;                                        \
	mutex_unlock(&pmsm->lock);                                            \
	return rc;                                                            \
}

DEFINE_MSM_OPEN(control);
DEFINE_MSM_OPEN(config);
DEFINE_MSM_OPEN(frame);

static const struct file_operations msm_fops_config = {
	.owner = THIS_MODULE,
	.open = msm_open_config,
	.unlocked_ioctl = msm_ioctl_config,
	.release = msm_release_config,
	.poll = msm_poll,
};

static const struct file_operations msm_fops_control = {
	.owner = THIS_MODULE,
	.open = msm_open_control,
	.unlocked_ioctl = msm_ioctl_control,
	.release = msm_release_control,
	.poll = msm_poll,
};

static const struct file_operations msm_fops_frame = {
	.owner = THIS_MODULE,
	.open = msm_open_frame,
	.unlocked_ioctl = msm_ioctl_frame,
	.release = msm_release_frame,
	.poll = msm_poll,
};

static int msm_setup_cdev(struct cdev *cdev,
			int node,
			dev_t devno,
			const char *suffix,
			const struct file_operations *fops)
{
	int rc = -ENODEV;
	struct device *class_dev =
		device_create(msm_class, NULL,
			devno, NULL,
			"%s%d", suffix, node);

	if (IS_ERR(class_dev))
		goto setup_fail_return;

	cdev_init(cdev, fops);
	cdev->owner = THIS_MODULE;

	rc = cdev_add(cdev, devno, 1);
	if (rc < 0)
		goto setup_fail_cleanup_all;
	return rc;

setup_fail_cleanup_all:
	cdev_del(cdev);
	device_destroy(msm_class, devno);
setup_fail_return:
	return rc;
}

static int msm_setup_cdevs(struct msm_device_t *msm, int node)
{
	int dev_num = 3 * node;
	int rc = msm_setup_cdev(&msm->cdev_control, node,
		MKDEV(MAJOR(msm_devno), dev_num++),
		"control", &msm_fops_control);
	if (rc < 0) {
		pr_err("error creating control node: %d\n", rc);
		return rc;
	}
	rc = msm_setup_cdev(&msm->cdev_frame, node,
		MKDEV(MAJOR(msm_devno), dev_num++),
		"frame", &msm_fops_frame);
	if (rc < 0) {
		pr_err("error creating frame node: %d\n", rc);
		return rc;
	}
	rc = msm_setup_cdev(&msm->cdev_config, node,
		MKDEV(MAJOR(msm_devno), dev_num),
		"config", &msm_fops_config);
	if (rc < 0)
		pr_err("error creating config node: %d\n", rc);

	CDBG("msm_camera setup finishes!\n");
	return rc;
}

static void msm_tear_down_cdevs(struct msm_device_t *msm, dev_t devno)
{
#if 0 /* FIXME: graceful teardown */
	cdev_del(&msm->cdev_frame);
	cdev_del(&msm->cdev_config);
	cdev_del(&msm->cdev_control);
	device_destroy(msm_class, devno);
#endif
}

static int __msm_control(struct msm_ctrl_cmd_t *ctrlcmd,
	struct msm_device_t *vmsm)
{
	unsigned long flags;
	int timeout;
	int rc = 0;

	struct msm_queue_cmd_t *qcmd = NULL;
	struct msm_queue_cmd_t *rcmd = NULL;

	/* wake up config thread, 4 is for V4L2 application */
	qcmd = kmalloc(sizeof(struct msm_queue_cmd_t), GFP_ATOMIC);
	if (!qcmd) {
		pr_err("msm_control: cannot allocate buffer\n");
		rc = -ENOMEM;
		goto end;
	}

	spin_lock_irqsave(&vmsm->sync.msg_event_queue_lock, flags);
	qcmd->type = MSM_CAM_Q_V4L2_REQ;
	qcmd->command = ctrlcmd;
	list_add_tail(&qcmd->list, &vmsm->sync.msg_event_queue);
	wake_up(&vmsm->sync.msg_event_wait);
	spin_unlock_irqrestore(&vmsm->sync.msg_event_queue_lock, flags);

	/* wait for config status */
	timeout = ctrlcmd->timeout_ms;
	CDBG("msm_control, timeout = %d\n", timeout);
	if (timeout > 0) {
		rc = wait_event_timeout(
				vmsm->sync.ctrl_status_wait,
				!list_empty(&vmsm->sync.ctrl_status_queue),
				msecs_to_jiffies(timeout));

		CDBG("msm_control: rc = %d\n", rc);

		if (rc == 0) {
			CDBG("msm_control: timed out\n");
			rc = -ETIMEDOUT;
			goto fail;
		}
	} else
		rc = wait_event_interruptible(
			vmsm->sync.ctrl_status_wait,
			!list_empty(&vmsm->sync.ctrl_status_queue));

	if (rc < 0) {
		pr_err("msm_control: wait_event error %d\n", rc);
		return -ERESTARTSYS;
	}

	/* control command status is ready */
	spin_lock_irqsave(&vmsm->sync.ctrl_status_lock, flags);
	if (!list_empty(&vmsm->sync.ctrl_status_queue)) {
		rcmd = list_first_entry(
			&vmsm->sync.ctrl_status_queue,
			struct msm_queue_cmd_t,
			list);
		list_del(&(rcmd->list));
	}
	spin_unlock_irqrestore(&vmsm->sync.ctrl_status_lock, flags);

	memcpy(ctrlcmd->value,
		((struct msm_ctrl_cmd_t *)(rcmd->command))->value,
		((struct msm_ctrl_cmd_t *)(rcmd->command))->length);

	if (((struct msm_ctrl_cmd_t *)(rcmd->command))->length > 0)
		kfree(((struct msm_ctrl_cmd_t *)
					 (rcmd->command))->value);
	goto end;

fail:
	kfree(qcmd);
end:
	kfree(rcmd);
	CDBG("__msm_control: end rc = %d\n", rc);
	return rc;
}

/* Needed for V4L2 */
int msm_register(struct msm_driver *drv, const char *id)
{
	int rc = -ENODEV;

	mutex_lock(&drv->vmsm->lock);
	if (list_empty(&msm_sensors))
		goto done;

	/* @todo to support multiple sensors */
	drv->vmsm = list_first_entry(&msm_sensors, struct msm_device_t, list);

	if (drv->vmsm->apps_id == NULL) {
		drv->vmsm->apps_id = id;

		drv->init      = __msm_open;
		drv->ctrl      = __msm_control;
		drv->reg_pmem  = __msm_register_pmem;
		drv->get_frame = __msm_get_frame;
		drv->put_frame = __msm_put_frame_buf;
		drv->get_pict  = __msm_get_pic;
		drv->drv_poll  = __msm_apps_poll;
		rc = 0;
	}

done:
	mutex_unlock(&drv->vmsm->lock);
	return rc;
}
EXPORT_SYMBOL(msm_register);

int msm_unregister(struct msm_driver *drv, const char *id)
{
	int rc = -EFAULT;

	mutex_lock(&drv->vmsm->lock);
	if (!strcmp(drv->vmsm->apps_id, id)) {
		drv->vmsm->apps_id = NULL;
		drv->vmsm = NULL;
		rc = 0;
	}
	mutex_unlock(&drv->vmsm->lock);

	return rc;
}
EXPORT_SYMBOL(msm_unregister);

int msm_camera_drv_start(struct platform_device *dev,
			int (*sensor_probe)(const struct msm_camera_sensor_info *,
						struct msm_sensor_ctrl_t *))
{
	struct msm_camera_sensor_info *sdata;
	struct msm_sensor_ctrl_t sctrl;
	struct msm_device_t *pmsm = NULL;
	int rc = -ENODEV;
	static int camera_node;

	sdata = dev->dev.platform_data;

	if (!msm_class) {
		/* There are three device nodes per sensor */
		rc = alloc_chrdev_region(&msm_devno, 0,
				3 * MSM_MAX_CAMERA_SENSORS,
				"msm_camera");
		if (rc < 0)
			goto start_region_fail;

		msm_class = class_create(THIS_MODULE, "msm_camera");
		if (IS_ERR(msm_class))
			goto start_class_fail;
	}

	pmsm = kzalloc(sizeof(struct msm_device_t), GFP_ATOMIC);
	if (!pmsm) {
		rc = -ENOMEM;
		goto kalloc_fail;
	}

	spin_lock_init(&pmsm->sync.msg_event_queue_lock);
	INIT_LIST_HEAD(&pmsm->sync.msg_event_queue);
	init_waitqueue_head(&pmsm->sync.msg_event_wait);

	spin_lock_init(&pmsm->sync.prev_frame_q_lock);
	INIT_LIST_HEAD(&pmsm->sync.prev_frame_q);
	init_waitqueue_head(&pmsm->sync.prev_frame_wait);

	spin_lock_init(&pmsm->sync.pict_frame_q_lock);
	INIT_LIST_HEAD(&pmsm->sync.pict_frame_q);
	init_waitqueue_head(&pmsm->sync.pict_frame_wait);

	spin_lock_init(&pmsm->sync.ctrl_status_lock);
	INIT_LIST_HEAD(&pmsm->sync.ctrl_status_queue);
	init_waitqueue_head(&pmsm->sync.ctrl_status_wait);

	wake_lock_init(&pmsm->wake_lock, WAKE_LOCK_IDLE, "msm_camera");

	mutex_init(&pmsm->lock);

	if (camera_node < MSM_MAX_CAMERA_SENSORS) {
		CDBG("setting camera node %d\n", camera_node);
		rc = msm_setup_cdevs(pmsm, camera_node);
		if (rc < 0)
			goto cdevs_fail;
		camera_node++;
	}
	else goto cdevs_fail;

	rc = msm_camio_probe_on(dev);
	if (rc < 0)
		goto probe_on_fail;
	/* FIXME: This has to be the last thing that we do.  Otherwise,
	   if it succeeds and then someting else fails, we have no
	   way to undo it.
	*/
	rc = sensor_probe(sdata, &sctrl);
	if (rc >= 0) {
		pmsm->pdev = dev;
		pmsm->sctrl = sctrl;
	}
	msm_camio_probe_off(dev);
	if (rc < 0)
		goto sensor_probe_fail;

	msm_camvfe_init();
	CDBG("DONE: %s %s:%d\n", __FILE__, __func__, __LINE__);
	list_add(&pmsm->list, &msm_sensors);
	return rc;

sensor_probe_fail:
probe_on_fail:
//	msm_tear_down_cdevs(pmsm, MKDEV(MAJOR(msm_devno), --dev_num));
cdevs_fail:
	wake_lock_destroy(&pmsm->wake_lock);
	kfree(pmsm);
kalloc_fail:
start_class_fail:
start_region_fail:
	CDBG("FAIL: %s %s: %d rc %d\n", __FILE__, __func__, __LINE__, rc);
	return rc;
}
EXPORT_SYMBOL(msm_camera_drv_start);