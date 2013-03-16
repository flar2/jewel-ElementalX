/*
 * Block driver for media (i.e., flash cards)
 *
 * Copyright 2002 Hewlett-Packard Company
 * Copyright 2005-2008 Pierre Ossman
 *
 * Use consistent with the GNU GPL is permitted,
 * provided that this copyright notice is
 * preserved in its entirety in all copies and derived works.
 *
 * HEWLETT-PACKARD COMPANY MAKES NO WARRANTIES, EXPRESSED OR IMPLIED,
 * AS TO THE USEFULNESS OR CORRECTNESS OF THIS CODE OR ITS
 * FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 * Many thanks to Alessandro Rubini and Jonathan Corbet!
 *
 * Author:  Andrew Christian
 *          28 May 2002
 */
#include <linux/moduleparam.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/string_helpers.h>
#include <linux/delay.h>
#include <linux/capability.h>
#include <linux/compat.h>

#include <linux/mmc/ioctl.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>

#include <asm/uaccess.h>

#include "queue.h"

MODULE_ALIAS("mmc:block");
#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "mmcblk."

#define INAND_CMD38_ARG_EXT_CSD  113
#define INAND_CMD38_ARG_ERASE    0x00
#define INAND_CMD38_ARG_TRIM     0x01
#define INAND_CMD38_ARG_SECERASE 0x80
#define INAND_CMD38_ARG_SECTRIM1 0x81
#define INAND_CMD38_ARG_SECTRIM2 0x88

#define MMC_SANITIZE_REQ_TIMEOUT 240000 
#define mmc_req_rel_wr(req)	(((req->cmd_flags & REQ_FUA) || \
			(req->cmd_flags & REQ_META)) && \
			(rq_data_dir(req) == WRITE))
#define PACKED_CMD_VER		0x01
#define PACKED_CMD_WR		0x02
#define MMC_BLK_UPDATE_STOP_REASON(stats, reason)			\
	do {								\
		if (stats->enabled)					\
			stats->pack_stop_reason[reason]++;		\
	} while (0)

static DEFINE_MUTEX(block_mutex);

#ifdef CONFIG_MMC_MUST_PREVENT_WP_VIOLATION
#include <mach/board.h>
static unsigned char wp_prevention_partno[64];
static unsigned int mmc0_write_prot_type;
void mmc_blk_set_wp_prevention_partno(int partno)
{
	sprintf(wp_prevention_partno, "mmcblk0p%d", partno);
	pr_info("%s: set wp_prevention_partno to %s\n", __func__, wp_prevention_partno);
}
EXPORT_SYMBOL(mmc_blk_set_wp_prevention_partno);

char *mmc_blk_get_wp_prevention_partno(unsigned char *buff)
{
	return strcpy(buff, wp_prevention_partno);
}
EXPORT_SYMBOL(mmc_blk_get_wp_prevention_partno);

void set_mmc0_write_protection_type(unsigned int type)
{
	mmc0_write_prot_type = type;
}
EXPORT_SYMBOL(set_mmc0_write_protection_type);

unsigned int get_mmc0_write_protection_type(void)
{
	return mmc0_write_prot_type;
}
EXPORT_SYMBOL(get_mmc0_write_protection_type);
#endif	

/* make sure wp_mod.ko doesn't crash kernel */

static unsigned int mmc0_write_prot_type;

void set_mmc0_write_protection_type(unsigned int type)
{
	mmc0_write_prot_type = type;
}

static int perdev_minors = CONFIG_MMC_BLOCK_MINORS;

static int max_devices;

atomic_t emmc_reboot;

static DECLARE_BITMAP(dev_use, 256);
static DECLARE_BITMAP(name_use, 256);

struct mmc_blk_data {
	spinlock_t	lock;
	struct gendisk	*disk;
	struct mmc_queue queue;
	struct list_head part;

	unsigned int	flags;
#define MMC_BLK_CMD23	(1 << 0)	
#define MMC_BLK_REL_WR	(1 << 1)	

	unsigned int	usage;
	unsigned int	read_only;
	unsigned int	part_type;
	unsigned int	name_idx;
	unsigned int	reset_done;
#define MMC_BLK_READ		BIT(0)
#define MMC_BLK_WRITE		BIT(1)
#define MMC_BLK_DISCARD		BIT(2)
#define MMC_BLK_SECDISCARD	BIT(3)

	unsigned int	part_curr;
	struct device_attribute force_ro;
	struct device_attribute power_ro_lock;
	int	area_type;
	struct device_attribute num_wr_reqs_to_start_packing;
};

static DEFINE_MUTEX(open_lock);

enum {
        MMC_PACKED_N_IDX = -1,
        MMC_PACKED_N_ZERO,
        MMC_PACKED_N_SINGLE,
};

module_param(perdev_minors, int, 0444);
MODULE_PARM_DESC(perdev_minors, "Minors numbers to allocate per device");

static inline void mmc_blk_clear_packed(struct mmc_queue_req *mqrq)
{
        mqrq->packed_cmd = MMC_PACKED_NONE;
        mqrq->packed_num = MMC_PACKED_N_ZERO;
}

static struct mmc_blk_data *mmc_blk_get(struct gendisk *disk)
{
	struct mmc_blk_data *md;

	mutex_lock(&open_lock);
	md = disk->private_data;
	if (md && md->usage == 0)
		md = NULL;
	if (md)
		md->usage++;
	mutex_unlock(&open_lock);

	return md;
}

static inline int mmc_get_devidx(struct gendisk *disk)
{
	int devidx = disk->first_minor / perdev_minors;
	return devidx;
}

static void mmc_blk_put(struct mmc_blk_data *md)
{
	mutex_lock(&open_lock);
	md->usage--;
	if (md->usage == 0) {
		int devidx = mmc_get_devidx(md->disk);
		blk_cleanup_queue(md->queue.queue);

		__clear_bit(devidx, dev_use);

		put_disk(md->disk);
		kfree(md);
	}
	mutex_unlock(&open_lock);
}

static ssize_t power_ro_lock_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	struct mmc_blk_data *md = mmc_blk_get(dev_to_disk(dev));
	struct mmc_card *card = md->queue.card;
	int locked = 0;

	if (card->ext_csd.boot_ro_lock & EXT_CSD_BOOT_WP_B_PERM_WP_EN)
		locked = 2;
	else if (card->ext_csd.boot_ro_lock & EXT_CSD_BOOT_WP_B_PWR_WP_EN)
		locked = 1;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", locked);

	return ret;
}

static ssize_t power_ro_lock_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct mmc_blk_data *md, *part_md;
	struct mmc_card *card;
	unsigned long set;

	if (kstrtoul(buf, 0, &set))
		return -EINVAL;

	if (set != 1)
		return count;

	md = mmc_blk_get(dev_to_disk(dev));
	card = md->queue.card;

	mmc_claim_host(card->host);

	ret = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_BOOT_WP,
				card->ext_csd.boot_ro_lock |
				EXT_CSD_BOOT_WP_B_PWR_WP_EN,
				card->ext_csd.part_time);
	if (ret)
		pr_err("%s: Locking boot partition ro until next power on failed: %d\n", md->disk->disk_name, ret);
	else
		card->ext_csd.boot_ro_lock |= EXT_CSD_BOOT_WP_B_PWR_WP_EN;

	mmc_release_host(card->host);

	if (!ret) {
		pr_info("%s: Locking boot partition ro until next power on\n",
			md->disk->disk_name);
		set_disk_ro(md->disk, 1);

		list_for_each_entry(part_md, &md->part, part)
			if (part_md->area_type == MMC_BLK_DATA_AREA_BOOT) {
				pr_info("%s: Locking boot partition ro until next power on\n", part_md->disk->disk_name);
				set_disk_ro(part_md->disk, 1);
			}
	}

	mmc_blk_put(md);
	return count;
}

static ssize_t force_ro_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int ret;
	struct mmc_blk_data *md = mmc_blk_get(dev_to_disk(dev));

	ret = snprintf(buf, PAGE_SIZE, "%d",
		       get_disk_ro(dev_to_disk(dev)) ^
		       md->read_only);
	mmc_blk_put(md);
	return ret;
}

static ssize_t force_ro_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int ret;
	char *end;
	struct mmc_blk_data *md = mmc_blk_get(dev_to_disk(dev));
	unsigned long set = simple_strtoul(buf, &end, 0);
	if (end == buf) {
		ret = -EINVAL;
		goto out;
	}

	set_disk_ro(dev_to_disk(dev), set || md->read_only);
	ret = count;
out:
	mmc_blk_put(md);
	return ret;
}

static ssize_t
num_wr_reqs_to_start_packing_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mmc_blk_data *md = mmc_blk_get(dev_to_disk(dev));
	int num_wr_reqs_to_start_packing;
	int ret;

	num_wr_reqs_to_start_packing = md->queue.num_wr_reqs_to_start_packing;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", num_wr_reqs_to_start_packing);

	mmc_blk_put(md);
	return ret;
}

static ssize_t
num_wr_reqs_to_start_packing_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int value;
	struct mmc_blk_data *md = mmc_blk_get(dev_to_disk(dev));

	sscanf(buf, "%d", &value);
	if (value >= 0)
		md->queue.num_wr_reqs_to_start_packing = value;

	mmc_blk_put(md);
	return count;
}

static int mmc_blk_open(struct block_device *bdev, fmode_t mode)
{
	struct mmc_blk_data *md = mmc_blk_get(bdev->bd_disk);
	int ret = -ENXIO;

	mutex_lock(&block_mutex);
	if (md) {
		if (md->usage == 2)
			check_disk_change(bdev);
		ret = 0;

		if ((mode & FMODE_WRITE) && md->read_only) {
			mmc_blk_put(md);
			ret = -EROFS;
		}
	}
	mutex_unlock(&block_mutex);

	return ret;
}

static int mmc_blk_release(struct gendisk *disk, fmode_t mode)
{
	struct mmc_blk_data *md = disk->private_data;

	mutex_lock(&block_mutex);
	mmc_blk_put(md);
	mutex_unlock(&block_mutex);
	return 0;
}

static int
mmc_blk_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	geo->cylinders = get_capacity(bdev->bd_disk) / (4 * 16);
	geo->heads = 4;
	geo->sectors = 16;
	return 0;
}

struct mmc_blk_ioc_data {
	struct mmc_ioc_cmd ic;
	unsigned char *buf;
	u64 buf_bytes;
};

static struct mmc_blk_ioc_data *mmc_blk_ioctl_copy_from_user(
	struct mmc_ioc_cmd __user *user)
{
	struct mmc_blk_ioc_data *idata;
	int err;

	idata = kzalloc(sizeof(*idata), GFP_KERNEL);
	if (!idata) {
		err = -ENOMEM;
		goto out;
	}

	if (copy_from_user(&idata->ic, user, sizeof(idata->ic))) {
		err = -EFAULT;
		goto idata_err;
	}

	idata->buf_bytes = (u64) idata->ic.blksz * idata->ic.blocks;
	if (idata->buf_bytes > MMC_IOC_MAX_BYTES) {
		err = -EOVERFLOW;
		goto idata_err;
	}

	if (!idata->buf_bytes)
		return idata;

	idata->buf = kzalloc(idata->buf_bytes, GFP_KERNEL);
	if (!idata->buf) {
		err = -ENOMEM;
		goto idata_err;
	}

	if (copy_from_user(idata->buf, (void __user *)(unsigned long)
					idata->ic.data_ptr, idata->buf_bytes)) {
		err = -EFAULT;
		goto copy_err;
	}

	return idata;

copy_err:
	kfree(idata->buf);
idata_err:
	kfree(idata);
out:
	return ERR_PTR(err);
}
#ifdef CONFIG_MMC_CPRM_SUPPORT
static u32 mmc_read_card_status(struct mmc_card *card)
{
	struct mmc_command cmd;
	int err;

	memset(&cmd, 0, sizeof(struct mmc_command));
	cmd.opcode = MMC_SEND_STATUS;
	if (!mmc_host_is_spi(card->host))
		cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err)
		printk(KERN_ERR "%s: %s error %d sending status comand", mmc_hostname(card->host),
		       __func__, err);
	return cmd.resp[0];
}
static int mmc_blk_ioctl_cmd_extend(struct block_device *bdev, struct mmc_ioc_cmd_extend __user *data)
{
	int err = 0;
	struct mmc_ioc_cmd_extend card_data_to_user;
	struct mmc_card *pCard;
	struct mmc_blk_data *md;

	md = mmc_blk_get(bdev->bd_disk);

	if (!md) {
		err = -EINVAL;
		goto cmd_done;
	}

	pCard = md->queue.card;
	if (IS_ERR(pCard)) {
		err = PTR_ERR(pCard);
		goto cmd_done;
	}

	mmc_claim_host(pCard->host);

	card_data_to_user.card_status = mmc_read_card_status(pCard);

	mmc_read_sd_status(pCard);
	memcpy(&card_data_to_user.ssr, &pCard->ssr, sizeof(struct sd_ssr));
	if (!access_ok(VERIFY_WRITE, data, sizeof(struct mmc_ioc_cmd_extend))) {
		printk(KERN_ERR "%s: %s invalid user area\n", mmc_hostname(pCard->host), __func__);
		err = -EFAULT;
		goto cmd_rel_host;
	}

	if (copy_to_user((void __user*)data, (void*)&card_data_to_user, sizeof(struct mmc_ioc_cmd_extend))) {
		printk(KERN_ERR "%s: %s fail to copy to user\n", mmc_hostname(pCard->host), __func__);
		err = -EFAULT;
		goto cmd_rel_host;
	}

cmd_rel_host:
	mmc_release_host(pCard->host);

cmd_done:
	mmc_blk_put(md);
	return err;
}
#endif
static int mmc_blk_ioctl_cmd(struct block_device *bdev,
	struct mmc_ioc_cmd __user *ic_ptr)
{
	struct mmc_blk_ioc_data *idata;
	struct mmc_blk_data *md;
	struct mmc_card *card;
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};
	struct mmc_request mrq = {NULL};
	struct scatterlist sg;
	int err;

	if ((!capable(CAP_SYS_RAWIO)) || (bdev != bdev->bd_contains))
		return -EPERM;

	idata = mmc_blk_ioctl_copy_from_user(ic_ptr);
	if (IS_ERR(idata))
		return PTR_ERR(idata);

	md = mmc_blk_get(bdev->bd_disk);
	if (!md) {
		err = -EINVAL;
		goto cmd_done;
	}

	card = md->queue.card;
	if (IS_ERR(card)) {
		err = PTR_ERR(card);
		goto cmd_done;
	}

	cmd.opcode = idata->ic.opcode;
	cmd.arg = idata->ic.arg;
	cmd.flags = idata->ic.flags;

	if (idata->buf_bytes) {
		data.sg = &sg;
		data.sg_len = 1;
		data.blksz = idata->ic.blksz;
		data.blocks = idata->ic.blocks;

		sg_init_one(data.sg, idata->buf, idata->buf_bytes);

		if (idata->ic.write_flag)
			data.flags = MMC_DATA_WRITE;
		else
			data.flags = MMC_DATA_READ;

		
		mmc_set_data_timeout(&data, card);

		
		if (idata->ic.data_timeout_ns)
			data.timeout_ns = idata->ic.data_timeout_ns;

		if ((cmd.flags & MMC_RSP_R1B) == MMC_RSP_R1B) {
			data.timeout_ns = idata->ic.cmd_timeout_ms * 1000000;
		}

		mrq.data = &data;
	}

	mrq.cmd = &cmd;

	if (!card || (card->sd_removed == 1) || !mmc_card_present(card)) {
		err = -ENODEV;
		goto cmd_done;
	}

	mmc_claim_host(card->host);

	if (idata->ic.is_acmd) {
		err = mmc_app_cmd(card->host, card);
		if (err)
			goto cmd_rel_host;
	}

	mmc_wait_for_req(card->host, &mrq);

	if (cmd.error) {
		dev_err(mmc_dev(card->host), "%s: cmd error %d\n",
						__func__, cmd.error);
		err = cmd.error;
		goto cmd_rel_host;
	}
	if (data.error) {
		dev_err(mmc_dev(card->host), "%s: data error %d\n",
						__func__, data.error);
		err = data.error;
		goto cmd_rel_host;
	}

	if (idata->ic.postsleep_min_us)
		usleep_range(idata->ic.postsleep_min_us, idata->ic.postsleep_max_us);

	if (copy_to_user(&(ic_ptr->response), cmd.resp, sizeof(cmd.resp))) {
		err = -EFAULT;
		goto cmd_rel_host;
	}

	if (!idata->ic.write_flag) {
		if (copy_to_user((void __user *)(unsigned long) idata->ic.data_ptr,
						idata->buf, idata->buf_bytes)) {
			err = -EFAULT;
			goto cmd_rel_host;
		}
	}

cmd_rel_host:
	mmc_release_host(card->host);

cmd_done:
	mmc_blk_put(md);
	kfree(idata->buf);
	kfree(idata);
	return err;
}

void remove_card(struct mmc_host *host)
{
	printk(KERN_INFO "%s: remove card\n",
		mmc_hostname(host));

	if (!host->card || host->card->sd_removed) {
		printk(KERN_INFO "%s: card already removed\n",
			mmc_hostname(host));
		return;
	}
	if (!mmc_card_present(host->card)) {
		printk(KERN_INFO "%s: card is not present\n",
			mmc_hostname(host));
		return;
	}
	host->card->sd_removed = 1;
	mmc_schedule_card_removal_work(&host->remove, 0);
}

static int mmc_blk_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
	int ret = -EINVAL;
#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	int err = 0, card_no_ready = 0;
	int retries = 3;
	struct mmc_blk_data *md;
	struct mmc_card *card;

	md = mmc_blk_get(bdev->bd_disk);
	if (!md)
		return ret;

	card = md->queue.card;
	if (IS_ERR(card))
		return ret;

	if (!card || (card->sd_removed == 1) || !mmc_card_present(card))
		return ret;

	mmc_claim_host(card->host);

	if (mmc_bus_needs_resume(card->host)) {
		if (mmc_card_sd(card) && card->sd_removed == 1) {
			printk(KERN_INFO "%s: %s card already removed\n", __func__,
				mmc_hostname(card->host));
			mmc_release_host(card->host);
			return ret;
		}

		do {
			err = mmc_resume_bus(card->host);
			retries--;
		} while (err && retries);

		if (err) {
			if (mmc_card_sd(card))
				remove_card(card->host);
			mmc_release_host(card->host);
			return ret;
		}
		retries = 3;
	}

	if (mmc_bus_fails_resume(card->host) || card_no_ready ||
		!retries || (mmc_card_sd(card) && card->sd_removed == 1)) {
		mmc_release_host(card->host);
		return ret;
	} else
		mmc_release_host(card->host);
#endif

	if (cmd == MMC_IOC_CMD)
		ret = mmc_blk_ioctl_cmd(bdev, (struct mmc_ioc_cmd __user *)arg);
#ifdef CONFIG_MMC_CPRM_SUPPORT
	
	else if (cmd == MMC_IOC_CMD_EXTEND)
		ret = mmc_blk_ioctl_cmd_extend(bdev, (struct mmc_ioc_cmd_extend __user *)arg);
	
#endif
	return ret;
}

#ifdef CONFIG_COMPAT
static int mmc_blk_compat_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
	return mmc_blk_ioctl(bdev, mode, cmd, (unsigned long) compat_ptr(arg));
}
#endif

static const struct block_device_operations mmc_bdops = {
	.open			= mmc_blk_open,
	.release		= mmc_blk_release,
	.getgeo			= mmc_blk_getgeo,
	.owner			= THIS_MODULE,
	.ioctl			= mmc_blk_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= mmc_blk_compat_ioctl,
#endif
};

static inline int mmc_blk_part_switch(struct mmc_card *card,
				      struct mmc_blk_data *md)
{
	int ret;
	struct mmc_blk_data *main_md = mmc_get_drvdata(card);

	if (main_md->part_curr == md->part_type)
		return 0;

	if (mmc_card_mmc(card)) {
		u8 part_config = card->ext_csd.part_config;

		part_config &= ~EXT_CSD_PART_CONFIG_ACC_MASK;
		part_config |= md->part_type;

		ret = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_PART_CONFIG, part_config,
				 card->ext_csd.part_time);
		if (ret)
			return ret;

		card->ext_csd.part_config = part_config;
	}

	main_md->part_curr = md->part_type;
	return 0;
}

static u32 mmc_sd_num_wr_blocks(struct mmc_card *card)
{
	int err;
	u32 result;
	__be32 *blocks;

	struct mmc_request mrq = {NULL};
	struct mmc_command cmd = {0};
	struct mmc_data data = {0};

	struct scatterlist sg;

	cmd.opcode = MMC_APP_CMD;
	cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err)
		return (u32)-1;
	if (!mmc_host_is_spi(card->host) && !(cmd.resp[0] & R1_APP_CMD))
		return (u32)-1;

	memset(&cmd, 0, sizeof(struct mmc_command));

	cmd.opcode = SD_APP_SEND_NUM_WR_BLKS;
	cmd.arg = 0;
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = 4;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;
	mmc_set_data_timeout(&data, card);

	mrq.cmd = &cmd;
	mrq.data = &data;

	blocks = kmalloc(4, GFP_KERNEL);
	if (!blocks)
		return (u32)-1;

	sg_init_one(&sg, blocks, 4);

	mmc_wait_for_req(card->host, &mrq);

	result = ntohl(*blocks);
	kfree(blocks);

	if (cmd.error || data.error)
		result = (u32)-1;

	return result;
}

static int send_stop(struct mmc_card *card, u32 *status)
{
	struct mmc_command cmd = {0};
	int err;

	cmd.opcode = MMC_STOP_TRANSMISSION;
	cmd.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, 5);
	if (err == 0)
		*status = cmd.resp[0];
	return err;
}

static int get_card_status(struct mmc_card *card, u32 *status, int retries)
{
	struct mmc_command cmd = {0};
	int err;

	cmd.opcode = MMC_SEND_STATUS;
	if (!mmc_host_is_spi(card->host))
		cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_SPI_R2 | MMC_RSP_R1 | MMC_CMD_AC;
	err = mmc_wait_for_cmd(card->host, &cmd, retries);
	if (err == 0)
		*status = cmd.resp[0];
	return err;
}

#define ERR_NOMEDIUM	3
#define ERR_RETRY	2
#define ERR_ABORT	1
#define ERR_CONTINUE	0

static int mmc_blk_cmd_error(struct request *req, const char *name, int error,
	bool status_valid, u32 status)
{
	switch (error) {
	case -EILSEQ:
		
		pr_err("%s: %s sending %s command, card status %#x\n",
			req->rq_disk->disk_name, "response CRC error",
			name, status);
		return ERR_RETRY;

	case -ETIMEDOUT:
		pr_err("%s: %s sending %s command, card status %#x\n",
			req->rq_disk->disk_name, "timed out", name, status);

		
		if (!status_valid) {
			pr_err("%s: status not valid, retrying timeout\n", req->rq_disk->disk_name);
			return ERR_RETRY;
		}
		if (status & (R1_COM_CRC_ERROR | R1_ILLEGAL_COMMAND)) {
			pr_err("%s: command error, retrying timeout\n", req->rq_disk->disk_name);
			return ERR_RETRY;
		}

		
		pr_err("%s: not retrying timeout\n", req->rq_disk->disk_name);
		return ERR_ABORT;

	default:
		
		pr_err("%s: unknown error %d sending read/write command, card status %#x\n",
		       req->rq_disk->disk_name, error, status);
		return ERR_ABORT;
	}
}

static int mmc_blk_cmd_recovery(struct mmc_card *card, struct request *req,
	struct mmc_blk_request *brq, int *ecc_err)
{
	bool prev_cmd_status_valid = true;
	u32 status, stop_status = 0;
	int err, retry;

	if (mmc_card_removed(card))
		return ERR_NOMEDIUM;

	for (retry = 2; retry >= 0; retry--) {
		err = get_card_status(card, &status, 0);
		if (!err)
			break;

		prev_cmd_status_valid = false;
		pr_err("%s: error %d sending status command, %sing\n",
		       req->rq_disk->disk_name, err, retry ? "retry" : "abort");
	}

	
	if (err) {
		
		if (mmc_detect_card_removed(card->host))
			return ERR_NOMEDIUM;
		return ERR_ABORT;
	}

	
	if ((status & R1_CARD_ECC_FAILED) ||
	    (brq->stop.resp[0] & R1_CARD_ECC_FAILED) ||
	    (brq->cmd.resp[0] & R1_CARD_ECC_FAILED))
		*ecc_err = 1;

	if (R1_CURRENT_STATE(status) == R1_STATE_DATA ||
	    R1_CURRENT_STATE(status) == R1_STATE_RCV) {
		err = send_stop(card, &stop_status);
		if (err)
			pr_err("%s: error %d sending stop command\n",
			       req->rq_disk->disk_name, err);

		if (err)
			return ERR_ABORT;
		if (stop_status & R1_CARD_ECC_FAILED)
			*ecc_err = 1;
	}

	
	if (brq->sbc.error)
		return mmc_blk_cmd_error(req, "SET_BLOCK_COUNT", brq->sbc.error,
				prev_cmd_status_valid, status);

	
	if (brq->cmd.error)
		return mmc_blk_cmd_error(req, "r/w cmd", brq->cmd.error,
				prev_cmd_status_valid, status);

	
	if (!brq->stop.error)
		return ERR_CONTINUE;

	
	pr_err("%s: error %d sending stop command, original cmd response %#x, card status %#x\n",
	       req->rq_disk->disk_name, brq->stop.error,
	       brq->cmd.resp[0], status);

	if (stop_status) {
		brq->stop.resp[0] = stop_status;
		brq->stop.error = 0;
	}
	return ERR_CONTINUE;
}

static int mmc_blk_reset(struct mmc_blk_data *md, struct mmc_host *host,
			 int type)
{
	int err;

	if (md->reset_done & type)
		return -EEXIST;

	md->reset_done |= type;
	err = mmc_hw_reset(host);
	
	if (err != -EOPNOTSUPP) {
		struct mmc_blk_data *main_md = mmc_get_drvdata(host->card);
		int part_err;

		main_md->part_curr = main_md->part_type;
		part_err = mmc_blk_part_switch(host->card, md);
		if (part_err) {
			return -ENODEV;
		}
	}
	return err;
}

static inline void mmc_blk_reset_success(struct mmc_blk_data *md, int type)
{
	md->reset_done &= ~type;
}

static int mmc_blk_issue_discard_rq(struct mmc_queue *mq, struct request *req)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	unsigned int from, nr, arg;
	int err = 0, type = MMC_BLK_DISCARD;

	if (!mmc_can_erase(card)) {
		err = -EOPNOTSUPP;
		goto out;
	}

	from = blk_rq_pos(req);
	nr = blk_rq_sectors(req);

	if (mmc_can_discard(card))
		arg = MMC_DISCARD_ARG;
	else if (mmc_can_trim(card))
		arg = MMC_TRIM_ARG;
	else
		arg = MMC_ERASE_ARG;
retry:
	if (card->quirks & MMC_QUIRK_INAND_CMD38) {
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 INAND_CMD38_ARG_EXT_CSD,
				 arg == MMC_TRIM_ARG ?
				 INAND_CMD38_ARG_TRIM :
				 INAND_CMD38_ARG_ERASE,
				 0);
		if (err)
			goto out;
	}
	err = mmc_erase(card, from, nr, arg);
out:
	if (err == -EIO && !mmc_blk_reset(md, card->host, type))
		goto retry;
	if (!err)
		mmc_blk_reset_success(md, type);
	blk_end_request(req, err, blk_rq_bytes(req));

	return err ? 0 : 1;
}

static int mmc_blk_issue_secdiscard_rq(struct mmc_queue *mq,
				       struct request *req)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	unsigned int from, nr, arg;
	int err = 0, type = MMC_BLK_SECDISCARD;

	if (!(mmc_can_secure_erase_trim(card))) {
		err = -EOPNOTSUPP;
		goto out;
	}

	from = blk_rq_pos(req);
	nr = blk_rq_sectors(req);

	if (mmc_can_trim(card) && !mmc_erase_group_aligned(card, from, nr))
		
		arg = MMC_TRIM_ARG;
	else
		
		arg = MMC_ERASE_ARG;
retry:

	err = mmc_erase(card, from, nr, arg);
	if (err == -EIO)
		goto out_retry;
	if (err)
		goto out;


	if (mmc_can_sanitize(card))
		err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_SANITIZE_START, 1, 0);
out_retry:
	if (err && !mmc_blk_reset(md, card->host, type))
		goto retry;
	if (!err)
		mmc_blk_reset_success(md, type);
out:
	blk_end_request(req, err, blk_rq_bytes(req));

	return err ? 0 : 1;
}

static int mmc_blk_issue_sanitize_rq(struct mmc_queue *mq,
				      struct request *req)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	int err = 0;

	BUG_ON(!card);
	BUG_ON(!card->host);

	if (!(mmc_can_sanitize(card) &&
	     (card->host->caps2 & MMC_CAP2_SANITIZE))) {
			pr_warning("%s: %s - SANITIZE is not supported\n",
				   mmc_hostname(card->host), __func__);
			err = -EOPNOTSUPP;
			goto out;
	}

	pr_debug("%s: %s - SANITIZE IN PROGRESS...\n",
		mmc_hostname(card->host), __func__);

	err = mmc_switch(card, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_SANITIZE_START, 1,
					MMC_SANITIZE_REQ_TIMEOUT);

	if (err)
		pr_err("%s: %s - mmc_switch() with "
		       "EXT_CSD_SANITIZE_START failed. err=%d\n",
		       mmc_hostname(card->host), __func__, err);

	pr_debug("%s: %s - SANITIZE COMPLETED\n", mmc_hostname(card->host),
					     __func__);

out:
	blk_end_request(req, err, blk_rq_bytes(req));

	return err ? 0 : 1;
}

static int mmc_blk_issue_flush(struct mmc_queue *mq, struct request *req)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	int ret = 0;

	ret = mmc_flush_cache(card);
	if (ret)
		ret = -EIO;

	blk_end_request_all(req, ret);

	return ret ? 0 : 1;
}

static inline void mmc_apply_rel_rw(struct mmc_blk_request *brq,
				    struct mmc_card *card,
				    struct request *req)
{
	if (!(card->ext_csd.rel_param & EXT_CSD_WR_REL_PARAM_EN)) {
		
		if (!IS_ALIGNED(brq->cmd.arg, card->ext_csd.rel_sectors))
			brq->data.blocks = 1;

		if (brq->data.blocks > card->ext_csd.rel_sectors)
			brq->data.blocks = card->ext_csd.rel_sectors;
		else if (brq->data.blocks < card->ext_csd.rel_sectors)
			brq->data.blocks = 1;
	}
}

#define CMD_ERRORS							\
	(R1_OUT_OF_RANGE |		\
	 R1_ADDRESS_ERROR |			\
	 R1_BLOCK_LEN_ERROR |	\
	 R1_WP_VIOLATION |		\
	 R1_CC_ERROR |				\
	 R1_ERROR)		

static int mmc_blk_err_check(struct mmc_card *card,
			     struct mmc_async_req *areq)
{
	struct mmc_queue_req *mq_mrq = container_of(areq, struct mmc_queue_req,
						    mmc_active);
	struct mmc_blk_request *brq = &mq_mrq->brq;
	struct request *req = mq_mrq->req;
	int ecc_err = 0;

	if (!mq_mrq)
		return MMC_BLK_ABORT;

	if (!brq)
		return MMC_BLK_ABORT;

	if (!req)
		return MMC_BLK_ABORT;

	if (brq->sbc.error || brq->cmd.error || brq->stop.error ||
	    brq->data.error) {
		switch (mmc_blk_cmd_recovery(card, req, brq, &ecc_err)) {
		case ERR_RETRY:
			return MMC_BLK_RETRY;
		case ERR_ABORT:
			return MMC_BLK_ABORT;
		case ERR_NOMEDIUM:
			return MMC_BLK_NOMEDIUM;
		case ERR_CONTINUE:
			break;
		}
	}

	if (brq->cmd.resp[0] & CMD_ERRORS) {
		pr_err("%s: r/w command failed, status = %#x\n",
		       req->rq_disk->disk_name, brq->cmd.resp[0]);
		return MMC_BLK_ABORT;
	}

	if (!mmc_host_is_spi(card->host) && rq_data_dir(req) != READ) {
		u32 status;
		do {
			int err = get_card_status(card, &status, 5);
			if (err) {
				pr_err("%s: error %d requesting status\n",
				       req->rq_disk->disk_name, err);
				return MMC_BLK_CMD_ERR;
			}
		} while (!(status & R1_READY_FOR_DATA) ||
			 (R1_CURRENT_STATE(status) == R1_STATE_PRG));
	}

	if (brq->data.error) {
		pr_err("%s: error %d transferring data, sector %u, nr %u, cmd response %#x, card status %#x\n",
		       req->rq_disk->disk_name, brq->data.error,
		       (unsigned)blk_rq_pos(req),
		       (unsigned)blk_rq_sectors(req),
		       brq->cmd.resp[0], brq->stop.resp[0]);

		if (rq_data_dir(req) == READ) {
			if (ecc_err)
				return MMC_BLK_ECC_ERR;
			return MMC_BLK_DATA_ERR;
		} else {
			return MMC_BLK_CMD_ERR;
		}
	}

	if (!brq->data.bytes_xfered)
		return MMC_BLK_RETRY;

	if (mq_mrq->packed_cmd != MMC_PACKED_NONE) {
		if (unlikely(brq->data.blocks << 9 != brq->data.bytes_xfered))
			return MMC_BLK_PARTIAL;
		else
			return MMC_BLK_SUCCESS;
	}

	if (blk_rq_bytes(req) != brq->data.bytes_xfered)
		return MMC_BLK_PARTIAL;

	return MMC_BLK_SUCCESS;
}

static int sd_blk_err_check(struct mmc_card *card,
			     struct mmc_async_req *areq)
{
	struct mmc_queue_req *mq_mrq = container_of(areq, struct mmc_queue_req,
						    mmc_active);
	struct mmc_blk_request *brq = &mq_mrq->brq;
	struct request *req = mq_mrq->req;
	int ecc_err = 0;

	if (brq->sbc.error || brq->cmd.error || brq->stop.error ||
	    brq->data.error) {
		switch (mmc_blk_cmd_recovery(card, req, brq, &ecc_err)) {
		case ERR_RETRY:
			return MMC_BLK_RETRY;
		case ERR_ABORT:
			return MMC_BLK_ABORT;
		case ERR_NOMEDIUM:
			return MMC_BLK_NOMEDIUM;
		case ERR_CONTINUE:
			break;
		}
	}

	if (brq->cmd.resp[0] & CMD_ERRORS) {
		pr_err("%s: r/w command failed, status = %#x\n",
		       req->rq_disk->disk_name, brq->cmd.resp[0]);
		return MMC_BLK_ABORT;
	}

	if (!mmc_host_is_spi(card->host) && rq_data_dir(req) != READ) {
		u32 status;
		int i = 0, err = 0;
		unsigned int msec = 0;
		unsigned long delay = jiffies + HZ;
		do {
			if (fls(i) > 11) {
				msec = (unsigned int)fls(i >> 11);
				msleep(msec);

				if (msec > 3 && ((i - 1) & i) == 0) {
					printk(KERN_ERR "%s: start "
						"sleep %u msecs\n",
						req->rq_disk->disk_name,
						msec);
				}
			}
			err = get_card_status(card, &status, 5);

			if (err) {
				printk(KERN_ERR "%s: error %d mmc requesting status\n",
					req->rq_disk->disk_name, err);
				return MMC_BLK_CMD_ERR;
			}
			if (time_after(jiffies, delay) && (fls(i) > 10)) {
				if ((status & R1_READY_FOR_DATA) &&
					(R1_CURRENT_STATE(status) == R1_STATE_TRAN)) {
					printk(KERN_ERR "%s: timeout but get card ready i = %d\n",
					mmc_hostname(card->host), i);
					break;
				}
				printk(KERN_ERR "%s: Failed to get card ready i = %d\n",
					mmc_hostname(card->host), i);
				return MMC_BLK_DATA_ERR;
			}
			i++;
		} while (!(status & R1_READY_FOR_DATA) ||
			 (R1_CURRENT_STATE(status) == R1_STATE_PRG));
	}

	if (brq->data.error) {
		pr_err("%s: error %d transferring data, sector %u, nr %u, cmd response %#x, card status %#x\n",
		       req->rq_disk->disk_name, brq->data.error,
		       (unsigned)blk_rq_pos(req),
		       (unsigned)blk_rq_sectors(req),
		       brq->cmd.resp[0], brq->stop.resp[0]);

		if (rq_data_dir(req) == READ) {
			if (ecc_err)
				return MMC_BLK_ECC_ERR;
			return MMC_BLK_DATA_ERR;
		} else {
			return MMC_BLK_CMD_ERR;
		}
	}

	if (!brq->data.bytes_xfered)
		return MMC_BLK_RETRY;

	if (mq_mrq->packed_cmd != MMC_PACKED_NONE) {
		if (unlikely(brq->data.blocks << 9 != brq->data.bytes_xfered))
			return MMC_BLK_PARTIAL;
		else
			return MMC_BLK_SUCCESS;
	}

	if (blk_rq_bytes(req) != brq->data.bytes_xfered)
		return MMC_BLK_PARTIAL;

	return MMC_BLK_SUCCESS;
}

static int mmc_blk_packed_err_check(struct mmc_card *card,
				    struct mmc_async_req *areq)
{
	struct mmc_queue_req *mq_rq = container_of(areq, struct mmc_queue_req,
			mmc_active);
	struct request *req = mq_rq->req;
	int err, check, status;
	u8 ext_csd[512];

	check = mmc_blk_err_check(card, areq);
	err = get_card_status(card, &status, 0);
	if (err) {
		pr_err("%s: error %d sending status command\n",
				req->rq_disk->disk_name, err);
		return MMC_BLK_ABORT;
	}

	if (status & R1_EXP_EVENT) {
		err = mmc_send_ext_csd(card, ext_csd);
		if (err) {
			pr_err("%s: error %d sending ext_csd\n",
					req->rq_disk->disk_name, err);
			return MMC_BLK_ABORT;
		}

		if ((ext_csd[EXT_CSD_EXP_EVENTS_STATUS] &
					EXT_CSD_PACKED_FAILURE) &&
				(ext_csd[EXT_CSD_PACKED_CMD_STATUS] &
				 EXT_CSD_PACKED_GENERIC_ERROR)) {
			if (ext_csd[EXT_CSD_PACKED_CMD_STATUS] &
					EXT_CSD_PACKED_INDEXED_ERROR) {
				mq_rq->packed_fail_idx =
				  ext_csd[EXT_CSD_PACKED_FAILURE_INDEX] - 1;
				return MMC_BLK_PARTIAL;
			}
		}
	}

	return check;
}

static void mmc_blk_rw_rq_prep(struct mmc_queue_req *mqrq,
			       struct mmc_card *card,
			       int disable_multi,
			       struct mmc_queue *mq)
{
	u32 readcmd, writecmd;
	struct mmc_blk_request *brq = &mqrq->brq;
	struct request *req = mqrq->req;
	struct mmc_blk_data *md = mq->data;
	bool do_data_tag;

	bool do_rel_wr = ((req->cmd_flags & REQ_FUA) ||
			  (req->cmd_flags & REQ_META)) &&
		(rq_data_dir(req) == WRITE) &&
		(md->flags & MMC_BLK_REL_WR);

	memset(brq, 0, sizeof(struct mmc_blk_request));
	brq->mrq.cmd = &brq->cmd;
	brq->mrq.data = &brq->data;

	brq->cmd.arg = blk_rq_pos(req);
	if (!mmc_card_blockaddr(card))
		brq->cmd.arg <<= 9;
	brq->cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	brq->data.blksz = 512;
	brq->stop.opcode = MMC_STOP_TRANSMISSION;
	brq->stop.arg = 0;
	brq->stop.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;
	brq->data.blocks = blk_rq_sectors(req);

	if (brq->data.blocks > card->host->max_blk_count)
		brq->data.blocks = card->host->max_blk_count;

	if (brq->data.blocks > 1) {
		if (disable_multi)
			brq->data.blocks = 1;

		
		if (card->host->caps2 & MMC_CAP2_NO_MULTI_READ &&
		    rq_data_dir(req) == READ)
			brq->data.blocks = 1;
	}

	if (brq->data.blocks > 1 || do_rel_wr) {
		if (!mmc_host_is_spi(card->host) ||
		    rq_data_dir(req) == READ)
			brq->mrq.stop = &brq->stop;
		readcmd = MMC_READ_MULTIPLE_BLOCK;
		writecmd = MMC_WRITE_MULTIPLE_BLOCK;
	} else {
		brq->mrq.stop = NULL;
		readcmd = MMC_READ_SINGLE_BLOCK;
		writecmd = MMC_WRITE_BLOCK;
	}
	if (rq_data_dir(req) == READ) {
		brq->cmd.opcode = readcmd;
		brq->data.flags |= MMC_DATA_READ;
	} else {
		brq->cmd.opcode = writecmd;
		brq->data.flags |= MMC_DATA_WRITE;
	}

	if (do_rel_wr)
		mmc_apply_rel_rw(brq, card, req);

	do_data_tag = (card->ext_csd.data_tag_unit_size) &&
		(req->cmd_flags & REQ_META) &&
		(rq_data_dir(req) == WRITE) &&
		((brq->data.blocks * brq->data.blksz) >=
		 card->ext_csd.data_tag_unit_size);

	if ((md->flags & MMC_BLK_CMD23) && mmc_op_multi(brq->cmd.opcode) &&
	    (do_rel_wr || !(card->quirks & MMC_QUIRK_BLK_NO_CMD23) ||
	     do_data_tag)) {
		brq->sbc.opcode = MMC_SET_BLOCK_COUNT;
		brq->sbc.arg = brq->data.blocks |
			(do_rel_wr ? (1 << 31) : 0) |
			(do_data_tag ? (1 << 29) : 0);
		brq->sbc.flags = MMC_RSP_R1 | MMC_CMD_AC;
		brq->mrq.sbc = &brq->sbc;
	}

	mmc_set_data_timeout(&brq->data, card);

	brq->data.sg = mqrq->sg;
	brq->data.sg_len = mmc_queue_map_sg(mq, mqrq);

	if (brq->data.blocks != blk_rq_sectors(req)) {
		int i, data_size = brq->data.blocks << 9;
		struct scatterlist *sg;

		for_each_sg(brq->data.sg, sg, brq->data.sg_len, i) {
			data_size -= sg->length;
			if (data_size <= 0) {
				sg->length += data_size;
				i++;
				break;
			}
		}
		brq->data.sg_len = i;
	}

	mqrq->mmc_active.mrq = &brq->mrq;
	if (mmc_card_sd(card))
		mqrq->mmc_active.err_check = sd_blk_err_check;
	else
		mqrq->mmc_active.err_check = mmc_blk_err_check;

	mmc_queue_bounce_pre(mqrq);
}

static void mmc_blk_write_packing_control(struct mmc_queue *mq,
					  struct request *req)
{
	struct mmc_host *host = mq->card->host;
	int data_dir;

	if (!(host->caps2 & MMC_CAP2_PACKED_WR))
		return;

	if (!(host->caps2 & MMC_CAP2_PACKED_WR_CONTROL)) {
		mq->wr_packing_enabled = true;
		return;
	}

	if (!req || (req && (req->cmd_flags & REQ_FLUSH))) {
		if (mq->num_of_potential_packed_wr_reqs >
				mq->num_wr_reqs_to_start_packing)
			mq->wr_packing_enabled = true;
		mq->num_of_potential_packed_wr_reqs = 0;
		return;
	}

	data_dir = rq_data_dir(req);

	if (data_dir == READ) {
		mq->num_of_potential_packed_wr_reqs = 0;
		mq->wr_packing_enabled = false;
		return;
	} else if (data_dir == WRITE) {
		mq->num_of_potential_packed_wr_reqs++;
	}

	if (mq->num_of_potential_packed_wr_reqs >
			mq->num_wr_reqs_to_start_packing)
		mq->wr_packing_enabled = true;

}

struct mmc_wr_pack_stats *mmc_blk_get_packed_statistics(struct mmc_card *card)
{
	if (!card)
		return NULL;

	return &card->wr_pack_stats;
}
EXPORT_SYMBOL(mmc_blk_get_packed_statistics);

void mmc_blk_init_packed_statistics(struct mmc_card *card)
{
	int max_num_of_packed_reqs = 0;

	if (!card || !card->wr_pack_stats.packing_events)
		return;

	max_num_of_packed_reqs = card->ext_csd.max_packed_writes;

	spin_lock(&card->wr_pack_stats.lock);
	memset(card->wr_pack_stats.packing_events, 0,
		(max_num_of_packed_reqs + 1) *
	       sizeof(*card->wr_pack_stats.packing_events));
	memset(&card->wr_pack_stats.pack_stop_reason, 0,
		sizeof(card->wr_pack_stats.pack_stop_reason));
	card->wr_pack_stats.enabled = true;
	spin_unlock(&card->wr_pack_stats.lock);
}
EXPORT_SYMBOL(mmc_blk_init_packed_statistics);

void print_mmc_packing_stats(struct mmc_card *card)
{
	int i;
	int max_num_of_packed_reqs = 0;

	if ((!card) || (!card->wr_pack_stats.packing_events))
		return;

	max_num_of_packed_reqs = card->ext_csd.max_packed_writes;

	spin_lock(&card->wr_pack_stats.lock);

	pr_info("%s: write packing statistics:\n",
		mmc_hostname(card->host));

	for (i = 1 ; i <= max_num_of_packed_reqs ; ++i) {
		if (card->wr_pack_stats.packing_events[i] != 0)
			pr_info("%s: Packed %d reqs - %d times\n",
				mmc_hostname(card->host), i,
				card->wr_pack_stats.packing_events[i]);
	}

	pr_info("%s: stopped packing due to the following reasons:\n",
		mmc_hostname(card->host));

	if (card->wr_pack_stats.pack_stop_reason[EXCEEDS_SEGMENTS])
		pr_info("%s: %d times: exceedmax num of segments\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[EXCEEDS_SEGMENTS]);
	if (card->wr_pack_stats.pack_stop_reason[EXCEEDS_SECTORS])
		pr_info("%s: %d times: exceeding the max num of sectors\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[EXCEEDS_SECTORS]);
	if (card->wr_pack_stats.pack_stop_reason[WRONG_DATA_DIR])
		pr_info("%s: %d times: wrong data direction\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[WRONG_DATA_DIR]);
	if (card->wr_pack_stats.pack_stop_reason[FLUSH_OR_DISCARD])
		pr_info("%s: %d times: flush or discard\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[FLUSH_OR_DISCARD]);
	if (card->wr_pack_stats.pack_stop_reason[EMPTY_QUEUE])
		pr_info("%s: %d times: empty queue\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[EMPTY_QUEUE]);
	if (card->wr_pack_stats.pack_stop_reason[REL_WRITE])
		pr_info("%s: %d times: rel write\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[REL_WRITE]);
	if (card->wr_pack_stats.pack_stop_reason[THRESHOLD])
		pr_info("%s: %d times: Threshold\n",
			mmc_hostname(card->host),
			card->wr_pack_stats.pack_stop_reason[THRESHOLD]);

	spin_unlock(&card->wr_pack_stats.lock);
}
EXPORT_SYMBOL(print_mmc_packing_stats);

static u8 mmc_blk_prep_packed_list(struct mmc_queue *mq, struct request *req)
{
	struct request_queue *q = mq->queue;
	struct mmc_card *card = mq->card;
	struct request *cur = req, *next = NULL;
	struct mmc_blk_data *md = mq->data;
	bool en_rel_wr = card->ext_csd.rel_param & EXT_CSD_WR_REL_PARAM_EN;
	unsigned int req_sectors = 0, phys_segments = 0;
	unsigned int max_blk_count, max_phys_segs;
	u8 put_back = 0;
	u8 max_packed_rw = 0;
	u8 reqs = 0;
	struct mmc_wr_pack_stats *stats = &card->wr_pack_stats;

	mmc_blk_clear_packed(mq->mqrq_cur);

	if (!(md->flags & MMC_BLK_CMD23) ||
			!card->ext_csd.packed_event_en)
		goto no_packed;

	if (!mq->wr_packing_enabled)
		goto no_packed;

	if ((rq_data_dir(cur) == WRITE) &&
			(card->host->caps2 & MMC_CAP2_PACKED_WR))
		max_packed_rw = card->ext_csd.max_packed_writes;

	if (max_packed_rw == 0)
		goto no_packed;

	if (mmc_req_rel_wr(cur) &&
			(md->flags & MMC_BLK_REL_WR) &&
			!en_rel_wr) {
		goto no_packed;
	}

	max_blk_count = min(card->host->max_blk_count,
			card->host->max_req_size >> 9);
	if (unlikely(max_blk_count > 0xffff))
		max_blk_count = 0xffff;

	max_phys_segs = queue_max_segments(q);
	req_sectors += blk_rq_sectors(cur);
	phys_segments += cur->nr_phys_segments;

	if (rq_data_dir(cur) == WRITE) {
		req_sectors++;
		phys_segments++;
	}

	spin_lock(&stats->lock);

	while (reqs < max_packed_rw - 1) {
		spin_lock_irq(q->queue_lock);
		next = blk_fetch_request(q);
		spin_unlock_irq(q->queue_lock);
		if (!next) {
			MMC_BLK_UPDATE_STOP_REASON(stats, EMPTY_QUEUE);
			break;
		}

		if (next->cmd_flags & REQ_DISCARD ||
				next->cmd_flags & REQ_FLUSH) {
			MMC_BLK_UPDATE_STOP_REASON(stats, FLUSH_OR_DISCARD);
			put_back = 1;
			break;
		}

		if (rq_data_dir(cur) != rq_data_dir(next)) {
			MMC_BLK_UPDATE_STOP_REASON(stats, WRONG_DATA_DIR);
			put_back = 1;
			break;
		}

		if (mmc_req_rel_wr(next) &&
				(md->flags & MMC_BLK_REL_WR) &&
				!en_rel_wr) {
			MMC_BLK_UPDATE_STOP_REASON(stats, REL_WRITE);
			put_back = 1;
			break;
		}

		req_sectors += blk_rq_sectors(next);
		if (req_sectors > max_blk_count) {
			if (stats->enabled)
				stats->pack_stop_reason[EXCEEDS_SECTORS]++;
			put_back = 1;
			break;
		}

		phys_segments +=  next->nr_phys_segments;
		if (phys_segments > max_phys_segs) {
			MMC_BLK_UPDATE_STOP_REASON(stats, EXCEEDS_SEGMENTS);
			put_back = 1;
			break;
		}

		if (rq_data_dir(next) == WRITE)
			mq->num_of_potential_packed_wr_reqs++;
		list_add_tail(&next->queuelist, &mq->mqrq_cur->packed_list);
		cur = next;
		reqs++;
	}

	if (put_back) {
		spin_lock_irq(q->queue_lock);
		blk_requeue_request(q, next);
		spin_unlock_irq(q->queue_lock);
	}

	if (stats->enabled) {
		if (reqs + 1 <= card->ext_csd.max_packed_writes)
			stats->packing_events[reqs + 1]++;
		if (reqs + 1 == max_packed_rw)
			MMC_BLK_UPDATE_STOP_REASON(stats, THRESHOLD);
	}

	spin_unlock(&stats->lock);

	if (reqs > 0) {
		list_add(&req->queuelist, &mq->mqrq_cur->packed_list);
		mq->mqrq_cur->packed_num = ++reqs;
		return reqs;
	}

no_packed:
	mmc_blk_clear_packed(mq->mqrq_cur);
	return 0;
}

static void mmc_blk_packed_hdr_wrq_prep(struct mmc_queue_req *mqrq,
					struct mmc_card *card,
					struct mmc_queue *mq)
{
	struct mmc_blk_request *brq = &mqrq->brq;
	struct request *req = mqrq->req;
	struct request *prq;
	struct mmc_blk_data *md = mq->data;
	bool do_rel_wr;
	u32 *packed_cmd_hdr = mqrq->packed_cmd_hdr;
	u8 i = 1;

	mqrq->packed_cmd = MMC_PACKED_WRITE;
	mqrq->packed_blocks = 0;
	mqrq->packed_fail_idx = MMC_PACKED_N_IDX;

	memset(packed_cmd_hdr, 0, sizeof(mqrq->packed_cmd_hdr));
	packed_cmd_hdr[0] = (mqrq->packed_num << 16) |
		(PACKED_CMD_WR << 8) | PACKED_CMD_VER;

	list_for_each_entry(prq, &mqrq->packed_list, queuelist) {
		do_rel_wr = mmc_req_rel_wr(prq) && (md->flags & MMC_BLK_REL_WR);
		
		packed_cmd_hdr[(i * 2)] =
			(do_rel_wr ? MMC_CMD23_ARG_REL_WR : 0) |
			blk_rq_sectors(prq);
		
		packed_cmd_hdr[((i * 2)) + 1] =
			mmc_card_blockaddr(card) ?
			blk_rq_pos(prq) : blk_rq_pos(prq) << 9;
		mqrq->packed_blocks += blk_rq_sectors(prq);
		i++;
	}

	memset(brq, 0, sizeof(struct mmc_blk_request));
	brq->mrq.cmd = &brq->cmd;
	brq->mrq.data = &brq->data;
	brq->mrq.sbc = &brq->sbc;
	brq->mrq.stop = &brq->stop;

	brq->sbc.opcode = MMC_SET_BLOCK_COUNT;
	brq->sbc.arg = MMC_CMD23_ARG_PACKED | (mqrq->packed_blocks + 1);
	brq->sbc.flags = MMC_RSP_R1 | MMC_CMD_AC;

	brq->cmd.opcode = MMC_WRITE_MULTIPLE_BLOCK;
	brq->cmd.arg = blk_rq_pos(req);
	if (!mmc_card_blockaddr(card))
		brq->cmd.arg <<= 9;
	brq->cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	brq->data.blksz = 512;
	brq->data.blocks = mqrq->packed_blocks + 1;
	brq->data.flags |= MMC_DATA_WRITE;

	brq->stop.opcode = MMC_STOP_TRANSMISSION;
	brq->stop.arg = 0;
	brq->stop.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC;

	mmc_set_data_timeout(&brq->data, card);

	brq->data.sg = mqrq->sg;
	brq->data.sg_len = mmc_queue_map_sg(mq, mqrq);

	mqrq->mmc_active.mrq = &brq->mrq;

	if (mq->err_check_fn)
		mqrq->mmc_active.err_check = mq->err_check_fn;
	else
		mqrq->mmc_active.err_check = mmc_blk_packed_err_check;

	if (mq->packed_test_fn)
		mq->packed_test_fn(mq->queue, mqrq);

	mmc_queue_bounce_pre(mqrq);
}

static int mmc_blk_cmd_err(struct mmc_blk_data *md, struct mmc_card *card,
			   struct mmc_blk_request *brq, struct request *req,
			   int ret)
{
	struct mmc_queue_req *mq_rq;
	mq_rq = container_of(brq, struct mmc_queue_req, brq);

	if (mmc_card_sd(card)) {
		u32 blocks;

		blocks = mmc_sd_num_wr_blocks(card);
		if (blocks != (u32)-1) {
			ret = blk_end_request(req, 0, blocks << 9);
		}
	} else {
		if (mq_rq->packed_cmd == MMC_PACKED_NONE) {
			ret = blk_end_request(req, 0, brq->data.bytes_xfered);
		}
	}
	return ret;
}

static int mmc_blk_end_packed_req(struct mmc_queue *mq,
				  struct mmc_queue_req *mq_rq)
{
	struct request *prq;
	int idx = mq_rq->packed_fail_idx, i = 0;
	int ret = 0;

	while (!list_empty(&mq_rq->packed_list)) {
		prq = list_entry_rq(mq_rq->packed_list.next);
		if (idx == i) {
			
			mq_rq->packed_num -= idx;
			mq_rq->req = prq;
			ret = 1;

			if (mq_rq->packed_num == MMC_PACKED_N_SINGLE) {
				list_del_init(&prq->queuelist);
				mmc_blk_clear_packed(mq_rq);
			}
			return ret;
		}
		list_del_init(&prq->queuelist);
		blk_end_request(prq, 0, blk_rq_bytes(prq));
		i++;
	}

	mmc_blk_clear_packed(mq_rq);
	return ret;
}

static int mmc_blk_issue_rw_rq(struct mmc_queue *mq, struct request *rqc)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	struct mmc_blk_request *brq = &mq->mqrq_cur->brq;
	int ret = 1, disable_multi = 0, retry = 0, type;
	enum mmc_blk_status status;
	struct mmc_queue_req *mq_rq;
	struct request *req, *prq;
	struct mmc_async_req *areq;
	const u8 packed_num = 2;
	u8 reqs = 0;

	if (!rqc && !mq->mqrq_prev->req)
		return 0;

	if (rqc)
		reqs = mmc_blk_prep_packed_list(mq, rqc);

	do {
		if (rqc) {
			if (reqs >= packed_num)
				mmc_blk_packed_hdr_wrq_prep(mq->mqrq_cur,
						card, mq);
			else
				mmc_blk_rw_rq_prep(mq->mqrq_cur, card, 0, mq);
			areq = &mq->mqrq_cur->mmc_active;
		} else
			areq = NULL;
		areq = mmc_start_req(card->host, areq, (int *) &status);
		if (!areq)
			return 0;

		mq_rq = container_of(areq, struct mmc_queue_req, mmc_active);
		brq = &mq_rq->brq;
		req = mq_rq->req;
		type = rq_data_dir(req) == READ ? MMC_BLK_READ : MMC_BLK_WRITE;
		mmc_queue_bounce_post(mq_rq);

		if (mmc_card_mmc(card) &&
			(brq->cmd.resp[0] & R1_EXCEPTION_EVENT))
			mmc_card_set_check_bkops(card);

		switch (status) {
		case MMC_BLK_SUCCESS:
		case MMC_BLK_PARTIAL:
			mmc_blk_reset_success(md, type);

			if (mq_rq->packed_cmd != MMC_PACKED_NONE) {
				ret = mmc_blk_end_packed_req(mq, mq_rq);
				break;
			} else {
				ret = blk_end_request(req, 0,
						brq->data.bytes_xfered);
			}

			if (status == MMC_BLK_SUCCESS && ret) {
				pr_err("%s BUG rq_tot %d d_xfer %d\n",
				       __func__, blk_rq_bytes(req),
				       brq->data.bytes_xfered);
				rqc = NULL;
				goto cmd_abort;
			}
			break;
		case MMC_BLK_CMD_ERR:
			ret = mmc_blk_cmd_err(md, card, brq, req, ret);
			if (!mmc_blk_reset(md, card->host, type))
				break;
			goto cmd_abort;
		case MMC_BLK_RETRY:
			if (retry++ < 5)
				break;
			
		case MMC_BLK_ABORT:
			if (!mmc_blk_reset(md, card->host, type))
				break;
			goto cmd_abort;
		case MMC_BLK_DATA_ERR: {
			int err;

			err = mmc_blk_reset(md, card->host, type);
			if (!err)
				break;
			if (err == -ENODEV ||
				mq_rq->packed_cmd != MMC_PACKED_NONE)
				goto cmd_abort;
			
		}
		case MMC_BLK_ECC_ERR:
			if (brq->data.blocks > 1) {
				
				pr_warning("%s: retrying using single block read\n",
					   req->rq_disk->disk_name);
				disable_multi = 1;
				break;
			}
			ret = blk_end_request(req, -EIO,
						brq->data.blksz);
			if (!ret)
				goto start_new_req;
			break;
		case MMC_BLK_NOMEDIUM:
			goto cmd_abort;
		}

		if (ret) {
			if (mq_rq->packed_cmd == MMC_PACKED_NONE) {
				mmc_blk_rw_rq_prep(mq_rq, card,
						disable_multi, mq);
				mmc_start_req(card->host,
						&mq_rq->mmc_active, NULL);
			} else {
				mmc_blk_packed_hdr_wrq_prep(mq_rq, card, mq);
				mmc_start_req(card->host,
						&mq_rq->mmc_active, NULL);
			}
		}
	} while (ret);

	return 1;

 cmd_abort:
	if (mq_rq->packed_cmd == MMC_PACKED_NONE) {
		if (mmc_card_removed(card))
			req->cmd_flags |= REQ_QUIET;
		while (ret)
			ret = blk_end_request(req, -EIO,
					blk_rq_cur_bytes(req));
	} else {
		while (!list_empty(&mq_rq->packed_list)) {
			prq = list_entry_rq(mq_rq->packed_list.next);
			list_del_init(&prq->queuelist);
			blk_end_request(prq, -EIO, blk_rq_bytes(prq));
		}
		mmc_blk_clear_packed(mq_rq);
	}

 start_new_req:
	if (rqc) {
		if (mq->mqrq_cur->packed_cmd != MMC_PACKED_NONE) {
			while (!list_empty(&mq->mqrq_cur->packed_list)) {
				prq = list_entry_rq(
					mq->mqrq_cur->packed_list.prev);
				if (prq->queuelist.prev !=
						&mq->mqrq_cur->packed_list) {
					list_del_init(&prq->queuelist);
					spin_lock_irq(mq->queue->queue_lock);
					blk_requeue_request(mq->queue, prq);
					spin_unlock_irq(mq->queue->queue_lock);
				} else {
					list_del_init(&prq->queuelist);
				}
			}
			mmc_blk_clear_packed(mq->mqrq_cur);
		}
		mmc_blk_rw_rq_prep(mq->mqrq_cur, card, 0, mq);
		mmc_start_req(card->host, &mq->mqrq_cur->mmc_active, NULL);
	}

	return 0;
}

static int mmc_blk_issue_rq(struct mmc_queue *mq, struct request *req)
{
	int ret;
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;

#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	mmc_claim_host(card->host);
	if (mmc_bus_needs_resume(card->host)) {
		mmc_resume_bus(card->host);
	}
	mmc_release_host(card->host);
#endif

	if (req && !mq->mqrq_prev->req)
		
		mmc_claim_host(card->host);

	ret = mmc_blk_part_switch(card, md);
	if (ret) {
		if (req) {
			blk_end_request_all(req, -EIO);
		}
		ret = 0;
		goto out;
	}

	mmc_blk_write_packing_control(mq, req);

	if (req && req->cmd_flags & REQ_SANITIZE) {
		
		if (card->host && card->host->areq)
			mmc_blk_issue_rw_rq(mq, NULL);
		ret = mmc_blk_issue_sanitize_rq(mq, req);
	} else if (req && req->cmd_flags & REQ_DISCARD) {
		
		if (card->host->areq)
			mmc_blk_issue_rw_rq(mq, NULL);
		if (req->cmd_flags & REQ_SECURE)
			ret = mmc_blk_issue_secdiscard_rq(mq, req);
		else
			ret = mmc_blk_issue_discard_rq(mq, req);
	} else if (req && req->cmd_flags & REQ_FLUSH) {
		
		if (card->host->areq)
			mmc_blk_issue_rw_rq(mq, NULL);
		ret = mmc_blk_issue_flush(mq, req);
	} else {
		ret = mmc_blk_issue_rw_rq(mq, req);
	}

out:
	if (!req)
		
		mmc_release_host(card->host);
	return ret;
}

static int sd_blk_issue_rw_rq(struct mmc_queue *mq, struct request *rqc)
{
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;
	struct mmc_blk_request *brq = &mq->mqrq_cur->brq;
	int ret = 1, disable_multi = 0, retry = 0, err = 0, type;
	enum mmc_blk_status status;
	struct mmc_queue_req *mq_rq;
	struct request *req, *prq;
	struct mmc_async_req *areq;
	ktime_t endrq_t, endrq_diff;
	const u8 packed_num = 2;
	u8 reqs = 0;
	int try_recovery = 0, do_reinit = 0;

	if (!rqc && !mq->mqrq_prev->req)
		return 0;

	if (rqc)
		reqs = mmc_blk_prep_packed_list(mq, rqc);

	do {
		if (rqc) {
			if (reqs >= packed_num)
				mmc_blk_packed_hdr_wrq_prep(mq->mqrq_cur,
						card, mq);
			else
				mmc_blk_rw_rq_prep(mq->mqrq_cur, card, 0, mq);
			areq = &mq->mqrq_cur->mmc_active;
		} else
			areq = NULL;
		areq = mmc_start_req(card->host, areq, (int *) &status);
		if (!areq)
			return 0;

		mq_rq = container_of(areq, struct mmc_queue_req, mmc_active);
		brq = &mq_rq->brq;
		req = mq_rq->req;
		type = rq_data_dir(req) == READ ? MMC_BLK_READ : MMC_BLK_WRITE;
		mmc_queue_bounce_post(mq_rq);

		if (mmc_card_mmc(card) &&
			(brq->cmd.resp[0] & R1_EXCEPTION_EVENT))
			mmc_card_set_check_bkops(card);

		switch (status) {
		case MMC_BLK_SUCCESS:
		case MMC_BLK_PARTIAL:
			mmc_blk_reset_success(md, type);

			if (mq_rq->packed_cmd != MMC_PACKED_NONE) {
				ret = mmc_blk_end_packed_req(mq, mq_rq);
				break;
			} else {
#ifdef CONFIG_MMC_PERF_PROFILING
				endrq_t = ktime_get();
#endif
				ret = blk_end_request(req, 0,
						brq->data.bytes_xfered);
#ifdef CONFIG_MMC_PERF_PROFILING
				endrq_diff = ktime_sub(ktime_get(), endrq_t);
				if (ktime_to_us(endrq_diff) > 2000)
					printk(KERN_DEBUG "%s (%s), cmd(%d) s_sec %d, size %d, end request time = %lld us\n",
					mmc_hostname(card->host), current->comm, brq->cmd.opcode,
					 brq->cmd.arg , brq->data.blocks, ktime_to_us(endrq_diff));
#endif
			}

			if (status == MMC_BLK_SUCCESS && ret) {
				pr_err("%s BUG rq_tot %d d_xfer %d\n",
				       __func__, blk_rq_bytes(req),
				       brq->data.bytes_xfered);
				rqc = NULL;
				goto cmd_abort;
			}
			break;
		case MMC_BLK_RETRY:
			if (retry++ < 2 && card->do_remove == 0)
				break;
			
			try_recovery++;
			if (try_recovery < 2 && card->do_remove == 0) {
				do_reinit = 1;
				goto recovery;
			} else {
				card->do_remove = 1;
				goto cmd_abort;
			}
		case MMC_BLK_CMD_ERR:
		case MMC_BLK_ABORT:
		case MMC_BLK_DATA_ERR: {
			int err;
			err = mmc_blk_reset(md, card->host, type);
			
			try_recovery++;
			if (try_recovery < 2 && card->do_remove == 0) {
				do_reinit = 1;
				goto recovery;
			} else {
				card->do_remove = 1;
				goto cmd_abort;
			}
		}
		case MMC_BLK_ECC_ERR:
			if (brq->data.blocks > 1 && card->do_remove == 0) {
				
				pr_warning("%s: retrying using single block read\n",
					   req->rq_disk->disk_name);
				disable_multi = 1;
				break;
			}
			
			try_recovery++;
			if (try_recovery < 2 && card->do_remove == 0) {
				do_reinit = 1;
				goto recovery;
			} else {
				card->do_remove = 1;
				goto cmd_abort;
			}
			ret = blk_end_request(req, -EIO,
						brq->data.blksz);
			if (!ret)
				goto start_new_req;
			break;
		case MMC_BLK_NOMEDIUM:
			card->do_remove = 1;
			goto cmd_abort;
		}

		if (ret) {
			if (mq_rq->packed_cmd == MMC_PACKED_NONE) {
				mmc_blk_rw_rq_prep(mq_rq, card,
						disable_multi, mq);
				mmc_start_req(card->host,
						&mq_rq->mmc_active, NULL);
			} else {
				mmc_blk_packed_hdr_wrq_prep(mq_rq, card, mq);
				mmc_start_req(card->host,
						&mq_rq->mmc_active, NULL);
			}
		}
recovery:
		if (do_reinit) {
			do_reinit = 0;
			if (card->sd_removed) {
				printk(KERN_INFO "%s: Card already removed. STOP reinit card\n",
				mmc_hostname(card->host));
				goto cmd_abort;
			}
			printk(KERN_INFO "%s: reinit card\n",
				mmc_hostname(card->host));
			err = mmc_reinit_card(card->host);
			if (!err) {
				mmc_blk_rw_rq_prep(mq_rq, card, 0, mq);
				mmc_start_req(card->host, &mq_rq->mmc_active, NULL);
			} else {
				printk(KERN_INFO "%s: reinit failed, remove card\n", mmc_hostname(card->host));
				card->do_remove = 1;
				goto cmd_abort;
			}
		}
	} while (ret);

	return 1;

 cmd_abort:
	if (mq_rq->packed_cmd == MMC_PACKED_NONE) {
		if (mmc_card_removed(card))
			req->cmd_flags |= REQ_QUIET;
		endrq_t = ktime_get();
		while (ret) {
			ret = blk_end_request(req, -EIO,
					blk_rq_cur_bytes(req));
			endrq_diff = ktime_sub(ktime_get(), endrq_t);
			if (ktime_to_us(endrq_diff) > 2000000) {
				printk(KERN_ERR "%s __blk_end_request over 2 seconds !\n", mmc_hostname(card->host));
				break;
			}
		}
	} else {
		while (!list_empty(&mq_rq->packed_list)) {
			prq = list_entry_rq(mq_rq->packed_list.next);
			list_del_init(&prq->queuelist);
			blk_end_request(prq, -EIO, blk_rq_bytes(prq));
		}
		mmc_blk_clear_packed(mq_rq);
	}

 start_new_req:
	if (card->do_remove) {
		printk(KERN_INFO "%s: %s, remove card\n", mmc_hostname(card->host), __func__);
		if (rqc)
			mmc_release_host(card->host);
		remove_card(card->host);
	}
	if (rqc) {
		if (card->sd_removed || card->do_remove) {
			printk(KERN_INFO "%s: Card already removed. STOP starting new request\n",
			mmc_hostname(card->host));
			blk_end_request_all(rqc, -EIO);
			return 0;
		}
		if (mq->mqrq_cur->packed_cmd != MMC_PACKED_NONE) {
			while (!list_empty(&mq->mqrq_cur->packed_list)) {
				prq = list_entry_rq(
					mq->mqrq_cur->packed_list.prev);
				if (prq->queuelist.prev !=
						&mq->mqrq_cur->packed_list) {
					list_del_init(&prq->queuelist);
					spin_lock_irq(mq->queue->queue_lock);
					blk_requeue_request(mq->queue, prq);
					spin_unlock_irq(mq->queue->queue_lock);
				} else {
					list_del_init(&prq->queuelist);
				}
			}
			mmc_blk_clear_packed(mq->mqrq_cur);
		}
		mmc_blk_rw_rq_prep(mq->mqrq_cur, card, 0, mq);
		mmc_start_req(card->host, &mq->mqrq_cur->mmc_active, NULL);
	}

	return 0;
}

static int sd_blk_issue_rq(struct mmc_queue *mq, struct request *req)
{
	int ret;
	struct mmc_blk_data *md = mq->data;
	struct mmc_card *card = md->queue.card;

#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	int err = 0;
	int retries = 3;
	mmc_claim_host(card->host);
	if (mmc_bus_needs_resume(card->host)) {
		if (card->sd_removed == 1) {
			printk(KERN_INFO "%s: card already removed, %s\n",
				mmc_hostname(card->host), __func__);
			if (req) {
				blk_end_request_all(req, -EIO);
			}
			mmc_release_host(card->host);
			return 0;
		}

		do {
			err = mmc_resume_bus(card->host);
			retries--;
		} while (err && retries);

		if (err) {
			printk(KERN_ERR "%s: Resume fail, removed card, %s\n",
				mmc_hostname(card->host), __func__);
			remove_card(card->host);
			if (req) {
				blk_end_request_all(req, -EIO);
			}
			mmc_release_host(card->host);
			return 0;
		}
	}
	if (mmc_bus_fails_resume(card->host) ||
		!retries || (mmc_card_sd(card) && (card->sd_removed == 1 || card->do_remove == 1))) {
		if (req) {
			blk_end_request_all(req, -EIO);
		}
		mmc_release_host(card->host);
		return 0;
	} else
		mmc_release_host(card->host);
#endif

	if (req && !mq->mqrq_prev->req)
		
		mmc_claim_host(card->host);

	ret = mmc_blk_part_switch(card, md);
	if (ret) {
		if (req) {
			blk_end_request_all(req, -EIO);
		}
		ret = 0;
		goto out;
	}

	mmc_blk_write_packing_control(mq, req);

	if (req && req->cmd_flags & REQ_SANITIZE) {
		
		if (card->host && card->host->areq)
			sd_blk_issue_rw_rq(mq, NULL);
		ret = mmc_blk_issue_sanitize_rq(mq, req);
	} else if (req && req->cmd_flags & REQ_DISCARD) {
		
		if (card->host->areq)
			sd_blk_issue_rw_rq(mq, NULL);
		if (req->cmd_flags & REQ_SECURE)
			ret = mmc_blk_issue_secdiscard_rq(mq, req);
		else
			ret = mmc_blk_issue_discard_rq(mq, req);
	} else if (req && req->cmd_flags & REQ_FLUSH) {
		
		if (card->host->areq)
			sd_blk_issue_rw_rq(mq, NULL);
		ret = mmc_blk_issue_flush(mq, req);
	} else {
		ret = sd_blk_issue_rw_rq(mq, req);
	}

out:
	if (!req)
		
		mmc_release_host(card->host);
	return ret;
}

static inline int mmc_blk_readonly(struct mmc_card *card)
{
	return mmc_card_readonly(card) ||
	       !(card->csd.cmdclass & CCC_BLOCK_WRITE);
}

static struct mmc_blk_data *mmc_blk_alloc_req(struct mmc_card *card,
					      struct device *parent,
					      sector_t size,
					      bool default_ro,
					      const char *subname,
					      int area_type)
{
	struct mmc_blk_data *md;
	int devidx, ret;

	devidx = find_first_zero_bit(dev_use, max_devices);
	if (devidx >= max_devices)
		return ERR_PTR(-ENOSPC);
	__set_bit(devidx, dev_use);

	md = kzalloc(sizeof(struct mmc_blk_data), GFP_KERNEL);
	if (!md) {
		ret = -ENOMEM;
		goto out;
	}

	if (!subname) {
		md->name_idx = find_first_zero_bit(name_use, max_devices);
		__set_bit(md->name_idx, name_use);
	} else
		md->name_idx = ((struct mmc_blk_data *)
				dev_to_disk(parent)->private_data)->name_idx;

	md->area_type = area_type;

	md->read_only = mmc_blk_readonly(card);

	md->disk = alloc_disk(perdev_minors);
	if (md->disk == NULL) {
		ret = -ENOMEM;
		goto err_kfree;
	}

	spin_lock_init(&md->lock);
	INIT_LIST_HEAD(&md->part);
	md->usage = 1;

	ret = mmc_init_queue(&md->queue, card, &md->lock, subname);
	if (ret)
		goto err_putdisk;

       if(mmc_card_sd(card))
               md->queue.issue_fn = sd_blk_issue_rq;
       else
               md->queue.issue_fn = mmc_blk_issue_rq;
	md->queue.data = md;

	md->disk->major	= MMC_BLOCK_MAJOR;
	md->disk->first_minor = devidx * perdev_minors;
	md->disk->fops = &mmc_bdops;
	md->disk->private_data = md;
	md->disk->queue = md->queue.queue;
	md->disk->driverfs_dev = parent;
	set_disk_ro(md->disk, md->read_only || default_ro);
	md->disk->flags = GENHD_FL_EXT_DEVT;


	snprintf(md->disk->disk_name, sizeof(md->disk->disk_name),
		 "mmcblk%d%s", md->name_idx, subname ? subname : "");

	blk_queue_logical_block_size(md->queue.queue, 512);
	set_capacity(md->disk, size);

	if (mmc_host_cmd23(card->host)) {
		if (mmc_card_mmc(card) ||
		    (mmc_card_sd(card) &&
		     card->scr.cmds & SD_SCR_CMD23_SUPPORT))
			md->flags |= MMC_BLK_CMD23;
	}

	if (mmc_card_mmc(card) &&
	    md->flags & MMC_BLK_CMD23 &&
	    ((card->ext_csd.rel_param & EXT_CSD_WR_REL_PARAM_EN) ||
	     card->ext_csd.rel_sectors)) {
		md->flags |= MMC_BLK_REL_WR;
		blk_queue_flush(md->queue.queue, REQ_FLUSH | REQ_FUA);
	}

	return md;

 err_putdisk:
	put_disk(md->disk);
 err_kfree:
	kfree(md);
 out:
	return ERR_PTR(ret);
}

static struct mmc_blk_data *mmc_blk_alloc(struct mmc_card *card)
{
	sector_t size;
	struct mmc_blk_data *md;

	if (!mmc_card_sd(card) && mmc_card_blockaddr(card)) {
		size = card->ext_csd.sectors;
	} else {
		size = card->csd.capacity << (card->csd.read_blkbits - 9);
	}

	md = mmc_blk_alloc_req(card, &card->dev, size, false, NULL,
					MMC_BLK_DATA_AREA_MAIN);
	return md;
}

static int mmc_blk_alloc_part(struct mmc_card *card,
			      struct mmc_blk_data *md,
			      unsigned int part_type,
			      sector_t size,
			      bool default_ro,
			      const char *subname,
			      int area_type)
{
	char cap_str[10];
	struct mmc_blk_data *part_md;

	part_md = mmc_blk_alloc_req(card, disk_to_dev(md->disk), size, default_ro,
				    subname, area_type);
	if (IS_ERR(part_md))
		return PTR_ERR(part_md);
	part_md->part_type = part_type;
	list_add(&part_md->part, &md->part);

	string_get_size((u64)get_capacity(part_md->disk) << 9, STRING_UNITS_2,
			cap_str, sizeof(cap_str));
	pr_info("%s: %s %s partition %u %s\n",
	       part_md->disk->disk_name, mmc_card_id(card),
	       mmc_card_name(card), part_md->part_type, cap_str);
	return 0;
}


static int mmc_blk_alloc_parts(struct mmc_card *card, struct mmc_blk_data *md)
{
	int idx, ret = 0;

	if (!mmc_card_mmc(card))
		return 0;

	for (idx = 0; idx < card->nr_parts; idx++) {
		if (card->part[idx].size) {
			ret = mmc_blk_alloc_part(card, md,
				card->part[idx].part_cfg,
				card->part[idx].size >> 9,
				card->part[idx].force_ro,
				card->part[idx].name,
				card->part[idx].area_type);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static void mmc_blk_remove_req(struct mmc_blk_data *md)
{
	struct mmc_card *card;

	if (md) {
		card = md->queue.card;
		device_remove_file(disk_to_dev(md->disk),
				   &md->num_wr_reqs_to_start_packing);
		if (md->disk->flags & GENHD_FL_UP) {
			device_remove_file(disk_to_dev(md->disk), &md->force_ro);
			if ((md->area_type & MMC_BLK_DATA_AREA_BOOT) &&
					card->ext_csd.boot_ro_lockable)
				device_remove_file(disk_to_dev(md->disk),
					&md->power_ro_lock);

			
			del_gendisk(md->disk);
		}

		
		mmc_cleanup_queue(&md->queue);
		mmc_blk_put(md);
	}
}

static void mmc_blk_remove_parts(struct mmc_card *card,
				 struct mmc_blk_data *md)
{
	struct list_head *pos, *q;
	struct mmc_blk_data *part_md;

	__clear_bit(md->name_idx, name_use);
	list_for_each_safe(pos, q, &md->part) {
		part_md = list_entry(pos, struct mmc_blk_data, part);
		list_del(pos);
		mmc_blk_remove_req(part_md);
	}
}

static int mmc_add_disk(struct mmc_blk_data *md)
{
	int ret;
	struct mmc_card *card = md->queue.card;

	add_disk(md->disk);
	md->force_ro.show = force_ro_show;
	md->force_ro.store = force_ro_store;
	sysfs_attr_init(&md->force_ro.attr);
	md->force_ro.attr.name = "force_ro";
	md->force_ro.attr.mode = S_IRUGO | S_IWUSR;
	ret = device_create_file(disk_to_dev(md->disk), &md->force_ro);
	if (ret)
		goto force_ro_fail;

	if ((md->area_type & MMC_BLK_DATA_AREA_BOOT) &&
	     card->ext_csd.boot_ro_lockable) {
		umode_t mode;

		if (card->ext_csd.boot_ro_lock & EXT_CSD_BOOT_WP_B_PWR_WP_DIS)
			mode = S_IRUGO;
		else
			mode = S_IRUGO | S_IWUSR;

		md->power_ro_lock.show = power_ro_lock_show;
		md->power_ro_lock.store = power_ro_lock_store;
		sysfs_attr_init(&md->power_ro_lock.attr);
		md->power_ro_lock.attr.mode = mode;
		md->power_ro_lock.attr.name =
					"ro_lock_until_next_power_on";
		ret = device_create_file(disk_to_dev(md->disk),
				&md->power_ro_lock);
		if (ret)
			goto power_ro_lock_fail;
	}

	md->num_wr_reqs_to_start_packing.show =
		num_wr_reqs_to_start_packing_show;
	md->num_wr_reqs_to_start_packing.store =
		num_wr_reqs_to_start_packing_store;
	sysfs_attr_init(&md->num_wr_reqs_to_start_packing.attr);
	md->num_wr_reqs_to_start_packing.attr.name =
		"num_wr_reqs_to_start_packing";
	md->num_wr_reqs_to_start_packing.attr.mode = S_IRUGO | S_IWUSR;
	ret = device_create_file(disk_to_dev(md->disk),
				 &md->num_wr_reqs_to_start_packing);
	if (ret)
		goto power_ro_lock_fail;

	return ret;

power_ro_lock_fail:
		device_remove_file(disk_to_dev(md->disk), &md->force_ro);
force_ro_fail:
		del_gendisk(md->disk);

	return ret;
}

#define CID_MANFID_SANDISK	0x2
#define CID_MANFID_TOSHIBA	0x11
#define CID_MANFID_MICRON	0x13

static const struct mmc_fixup blk_fixups[] =
{
	MMC_FIXUP("SEM02G", CID_MANFID_SANDISK, 0x100, add_quirk,
		  MMC_QUIRK_INAND_CMD38),
	MMC_FIXUP("SEM04G", CID_MANFID_SANDISK, 0x100, add_quirk,
		  MMC_QUIRK_INAND_CMD38),
	MMC_FIXUP("SEM08G", CID_MANFID_SANDISK, 0x100, add_quirk,
		  MMC_QUIRK_INAND_CMD38),
	MMC_FIXUP("SEM16G", CID_MANFID_SANDISK, 0x100, add_quirk,
		  MMC_QUIRK_INAND_CMD38),
	MMC_FIXUP("SEM32G", CID_MANFID_SANDISK, 0x100, add_quirk,
		  MMC_QUIRK_INAND_CMD38),

	MMC_FIXUP("MMC08G", CID_MANFID_TOSHIBA, CID_OEMID_ANY, add_quirk_mmc,
		  MMC_QUIRK_BLK_NO_CMD23),
	MMC_FIXUP("MMC16G", CID_MANFID_TOSHIBA, CID_OEMID_ANY, add_quirk_mmc,
		  MMC_QUIRK_BLK_NO_CMD23),
	MMC_FIXUP("MMC32G", CID_MANFID_TOSHIBA, CID_OEMID_ANY, add_quirk_mmc,
		  MMC_QUIRK_BLK_NO_CMD23),

	MMC_FIXUP(CID_NAME_ANY, CID_MANFID_MICRON, 0x200, add_quirk_mmc,
		  MMC_QUIRK_LONG_READ_TIME),

	
	MMC_FIXUP("SEM04G", 0x45, CID_OEMID_ANY, add_quirk_mmc,
		  MMC_QUIRK_INAND_DATA_TIMEOUT),

	END_FIXUP
};

static int mmc_blk_probe(struct mmc_card *card)
{
	struct mmc_blk_data *md, *part_md;
	char cap_str[10];

	if (!(card->csd.cmdclass & CCC_BLOCK_READ))
		return -ENODEV;

	md = mmc_blk_alloc(card);
	if (IS_ERR(md))
		return PTR_ERR(md);

	string_get_size((u64)get_capacity(md->disk) << 9, STRING_UNITS_2,
			cap_str, sizeof(cap_str));
	pr_info("%s: %s %s %s %s\n",
		md->disk->disk_name, mmc_card_id(card), mmc_card_name(card),
		cap_str, md->read_only ? "(ro)" : "");

	if (mmc_blk_alloc_parts(card, md))
		goto out;

	mmc_set_drvdata(card, md);
	mmc_fixup_device(card, blk_fixups);

	mmc_init_bus_resume_flags(card->host);
#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	mmc_set_bus_resume_policy(card->host, 1);
#endif
	if (mmc_add_disk(md))
		goto out;

	list_for_each_entry(part_md, &md->part, part) {
		if (mmc_add_disk(part_md))
			goto out;
	}
	card->mmcblk_dev = disk_to_dev(md->disk);
	return 0;

 out:
	mmc_blk_remove_parts(card, md);
	mmc_blk_remove_req(md);
	return 0;
}

static void mmc_blk_remove(struct mmc_card *card)
{
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	mmc_blk_remove_parts(card, md);
	mmc_claim_host(card->host);
	mmc_blk_part_switch(card, md);
	mmc_release_host(card->host);
	mmc_blk_remove_req(md);
	mmc_set_drvdata(card, NULL);
#ifdef CONFIG_MMC_BLOCK_DEFERRED_RESUME
	mmc_set_bus_resume_policy(card->host, 0);
#endif
}

#ifdef CONFIG_PM
static int mmc_blk_suspend(struct mmc_card *card)
{
	struct mmc_blk_data *part_md;
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	if (md) {
		mmc_queue_suspend(&md->queue);
		list_for_each_entry(part_md, &md->part, part) {
			mmc_queue_suspend(&part_md->queue);
		}
	}
	return 0;
}

static int mmc_blk_resume(struct mmc_card *card)
{
	struct mmc_blk_data *part_md;
	struct mmc_blk_data *md = mmc_get_drvdata(card);

	if (md) {
		md->part_curr = md->part_type;
		mmc_queue_resume(&md->queue);
		list_for_each_entry(part_md, &md->part, part) {
			mmc_queue_resume(&part_md->queue);
		}
	}
	return 0;
}
#else
#define	mmc_blk_suspend	NULL
#define mmc_blk_resume	NULL
#endif

static struct mmc_driver mmc_driver = {
	.drv		= {
		.name	= "mmcblk",
	},
	.probe		= mmc_blk_probe,
	.remove		= mmc_blk_remove,
	.suspend	= mmc_blk_suspend,
	.resume		= mmc_blk_resume,
};

static int __init mmc_blk_init(void)
{
	int res;

	if (perdev_minors != CONFIG_MMC_BLOCK_MINORS)
		pr_info("mmcblk: using %d minors per device\n", perdev_minors);

	/* set mmc0_write_prot_type so wp_mod.ko doesn't crash */
	set_mmc0_write_protection_type(0);
	
	max_devices = 256 / perdev_minors;

	res = register_blkdev(MMC_BLOCK_MAJOR, "mmc");
	if (res)
		goto out;

	res = mmc_register_driver(&mmc_driver);
	if (res)
		goto out2;

	return 0;
 out2:
	unregister_blkdev(MMC_BLOCK_MAJOR, "mmc");
 out:
	return res;
}

static void __exit mmc_blk_exit(void)
{
	mmc_unregister_driver(&mmc_driver);
	unregister_blkdev(MMC_BLOCK_MAJOR, "mmc");
}

module_init(mmc_blk_init);
module_exit(mmc_blk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Multimedia Card (MMC) block device driver");

