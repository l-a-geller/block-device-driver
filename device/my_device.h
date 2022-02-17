/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <linux/device.h>
#include <linux/blk-mq.h>

#define QUEUE_DEPTH 128

/* Device representation */
struct block_dev {
	sector_t capacity;
	u8 *data;
	struct blk_mq_tag_set tag_set;
	struct request_queue *queue;
	struct gendisk *gd;
	struct device dev;
	int mode; /* 0-read only, 1-read&write */
};

/* List of user devices & opearations with it: create, add, destroy */
struct user_device_list {
	struct block_dev *device;
	struct user_device_list *next;
};

struct user_device_list *node_create(struct block_dev *dev)
{
	struct user_device_list *lst;

	lst = kmalloc(sizeof(struct user_device_list), GFP_KERNEL);
	lst->device = dev;
	lst->next = NULL;
	return lst;
}

void list_add_front(struct user_device_list **old_head, struct block_dev *dev)
{
	struct user_device_list *new_head;

	new_head = node_create(dev);
	new_head->next = *old_head;
	*old_head = new_head;
}

struct user_device_list *list_search_name(struct user_device_list *list,
					char *name)
{
	const struct device *dev;
	struct user_device_list *res;

	dev = &(list->device->dev);
	while (list) {
		if (!dev_name(dev) ||
			!strncmp(dev_name(dev), name, strlen(name))) {
			res = list;
			break;
		}
		list  = list->next;
	}
	return res;
}


void list_destroy(struct user_device_list *list)
{
	if (list) {
		list_destroy(list->next);
		kfree(list);
	}
}

static void my_device_release_from_bus(struct device *dev)
{
	/* what to do on device release from a bus,
	 * not much here because we release all devices at once
	 * on module deinit
	 */
	pr_info("MYDRIVE: (device) bus device release requested\n");
}

static int my_device_open(struct block_device *bdev, fmode_t mode)
{
	pr_info("MYDRIVE: (device) Device opened\n");
	return 0;
}

static void my_device_release(struct gendisk *gd, fmode_t mode)
{
	pr_info("MYDRIVE: (device) Device released\n");
}

int block_dev_ioctl(struct block_device *bdev,
			fmode_t mode,
			unsigned int cmd,
			unsigned long arg)
{
	return -ENOTTY;
}

static const struct block_device_operations blockdev_ops = {
	.owner = THIS_MODULE,
	.open = my_device_open,
	.release = my_device_release,
	.ioctl = block_dev_ioctl
};

/* Request handling */
static int do_transfer(struct request *rq, unsigned int *nr_bytes)
{
	int ret = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	struct block_dev *dev = rq->q->queuedata;
	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
	loff_t dev_size = (loff_t) (dev->capacity << SECTOR_SHIFT);

	/*pr_info(
	 * "MYDRIVE: (request) request start from sector %lld pos = %lld\n",
	 * blk_rq_pos(rq),
	 * pos,
	 * dev_size);
	 */

	/* Iterate through all the requests segments */
	rq_for_each_segment(bvec, rq, iter) {
		unsigned long b_len = bvec.bv_len;

		/* Get data pointer */
		void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

		/* Check that we are not out of memory bounds */
		if ((pos + b_len) > dev_size)
			b_len = (unsigned long) (dev_size - pos);

		if (rq_data_dir(rq) == WRITE) {
			if (dev->mode)
				pr_warn("MYDRIVE: (device) don't try to write to read only device\n");
			else
			/* Write data to buffer into required position */
				memcpy(dev->data + pos, b_buf, b_len);
		} else
			/* Read data from the buffers position */
			memcpy(b_buf, dev->data + pos, b_len);

		pos += b_len;
		*nr_bytes += b_len;
	}
	return ret;
}

static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx,
				const struct blk_mq_queue_data *bd)
{
	unsigned int nr_bytes = 0;
	blk_status_t status = BLK_STS_OK;

	/* Start request serving */
	blk_mq_start_request(bd->rq);

	if (do_transfer(bd->rq, &nr_bytes) != 0 ||
			blk_update_request(bd->rq, status, nr_bytes))
		status = BLK_STS_IOERR;

	/* Stop request serving */
	blk_mq_end_request(bd->rq, status);
	return status;
}

static const struct blk_mq_ops mq_ops = {
	.queue_rq = queue_rq,
};

struct block_dev *my_device_allocate(struct block_dev **dev_pointer)
{
	*dev_pointer = kmalloc(sizeof(struct block_dev), GFP_KERNEL);

	if (!dev_pointer)
		return NULL;
	return *dev_pointer;
}

static int my_device_create(struct block_dev **dev_pointer,
				char *dev_name,
				sector_t capacity)
{
	int dev_major;
	struct block_dev *dev;

	dev = my_device_allocate(dev_pointer);
	if (!dev) {
		pr_warn("MYDRIVE: (device) Unable to allocate device\n");
		return -ENOMEM;
	}

	dev_major = register_blkdev(0, dev_name);
	if (dev_major < 0) {
		kfree(dev);
		pr_warn("MYDRIVE: (device) Unable to get major number\n");
		return -EBUSY;
	}

	pr_info("MYDRIVE: (device) allocating data\n");
	dev->mode = 1;
	dev->capacity = capacity;
	dev->data = kmalloc(capacity << 9, GFP_KERNEL);
	if (!dev->data) {
		unregister_blkdev(dev_major, dev_name);
		kfree(dev);
		pr_warn("MYDRIVE: (device) failed to allocate device IO buffer\n");
		return -ENOMEM;
	}

	pr_info("MYDRIVE: (device) initializing queue\n");
	dev->queue = blk_mq_init_sq_queue(&dev->tag_set,
					  &mq_ops,
					  QUEUE_DEPTH,
					  BLK_MQ_F_SHOULD_MERGE);
	if (!dev->queue) {
		kfree(dev->data);
		unregister_blkdev(dev_major, dev_name);
		kfree(dev);
		pr_warn("MYDRIVE: (device) failed to allocate device queue\n");
		return -ENOMEM;
	}
	/* Set driver's structure as user data of the queue */
	dev->queue->queuedata = dev;
	dev->gd = alloc_disk(1);

	/* Set disk required flags and data */
	dev->gd->flags = GENHD_FL_NO_PART_SCAN;
	dev->gd->major = dev_major;
	dev->gd->first_minor = 0;
	dev->gd->fops = &blockdev_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;

	/* Set device name (is represented in /dev under this name) */
	strcpy(dev->gd->disk_name, dev_name);
	pr_info("MYDRIVE: (device) adding disk %s\n", dev->gd->disk_name);

	set_capacity(dev->gd, capacity);

	/* Notify kernel about a new disk device */
	add_disk(dev->gd);
	return 0;
}

static void my_device_delete(struct block_dev *dev)
{
	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}
	if (dev->queue)
		blk_cleanup_queue(dev->queue);
	kfree(dev->data);
}
