/*
 * =====================================================================================
 *
 *       Filename:  ssd.c
 *
 *    Description:  Block device driver for solid state disk without hardware ftl. 
 *
 *        Version:  1.0
 *        Created:  03/04/2012 09:20:49 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Xiaolin Guo (guoxl), ringoguo@gmail.com
 *        Company:  Tsinghua Univ
 *
 * =====================================================================================
 */

#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fs.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_device.h>

#include "ssd.h"

MODULE_AUTHOR("Xiaolin Guo");
MODULE_DESCRIPTION("driver for ssd wi soft-ftl");
MODULE_LICENSE("GPL");

/*static int  ss_probe(struct device *);
static int  ss_remove(struct device *);
static void ss_shutdown(struct device *);
static int ss_suspend(struct device *, pm_message_t state);
static int ss_resume(struct device *);
static void ss_rescan(struct device *);
static int ss_done(struct scsi_cmnd *);
static struct scsi_driver ss_template = {
    .owner          = THIS_MODULE,
    .gendrv = {
        .name       = "ss",
        .probe      = ss_probe,
        .remove     = ss_remove,
        .suspend    = ss_suspend,
        .resume     = ss_resume,
        .shutdown   = ss_shutdown,
    },
    .rescan         = ss_rescan,
    .done           = ss_done,
};*/

unsigned int ssd_major[SSD_MAJOR];

struct kmem_cache * ss_cmt_cache;

char * ssds[] = { "/dev/sda", };

LIST_HEAD(ssd_list);

static int ss_open(struct block_device *bdev, fmode_t mode) {
    //struct scsi_disk *sdkp = scsi_disk_get(bdev->bd_disk);
    //struct scsi_device *sdev;
    return 0;
}

static int ss_release(struct gendisk * disk, fmode_t mode) {
    //struct scsi_disk *sdkp = scsi_disk(disk);

    //scsi_disk_put(sdkp);
    return 0;
}

static int ss_ioctl(struct block_device *bdev, fmode_t mode,
        unsigned int cmd, unsigned long arg)
{
    return 0;
}

static int ss_media_changed(struct gendisk *disk)
{
    return 0;
}

static int ss_revalidate_disk(struct gendisk *disk)
{
    return 0;
}

static const struct block_device_operations ss_fops = {
        .owner      = THIS_MODULE,
        .open       = ss_open,
        .release    = ss_release,
        .locked_ioctl =  ss_ioctl,
        .getgeo     = NULL,
        .media_changed = ss_media_changed,
        .revalidate_disk = ss_revalidate_disk,
        .unlock_native_capacity = NULL,
};

static int find_and_init_disk(void)
{
    int i, found = 0;
    struct block_device * bdev;
    struct ssd_disk * sdk;

    for (i = 0; i < sizeof(ssds); i++) {
        bdev = lookup_bdev(ssds[i]);
        if (bdev) {
            SDEBUG("%s as ssd wi soft-ftl found!\n", ssds[i]);
            found ++;
            sdk = kzalloc(sizeof(struct ssd_disk), GFP_KERNEL);
            list_add(&sdk->list, &ssd_list);
            sdk->bdev = bdev;
        }
    }

    return found;
}


static int __init init_ssd(void)
{
    int major, i, err = 0;
    major = 0;

    for (i = 0; i < SSD_MAJOR; i++)
        if ((ssd_major[i] = register_blkdev(ssd_major[i], "ss")) >= 0)
            major ++;
    if (!major)
        return -ENODEV;

    find_and_init_disk();

    ss_cmt_cache = kmem_cache_create("ss_cmt_cache", CMT_SIZE, 0, 0, NULL);

    if (!ss_cmt_cache) {
        printk(KERN_ERR "ss: can't init cmt cache\n");
        goto err_out;
    }

    return 0;

err_out:
    for (i = 0; i < SSD_MAJOR; i++)
        unregister_blkdev(ssd_major[i], "ss");
    return err;
}

static void __exit exit_ssd(void)
{
    int i;

    kmem_cache_destroy(ss_cmt_cache);
    for (i = 0; i < SSD_MAJOR; i ++)
        unregister_blkdev(ssd_major[i], "ss");
}

module_init(init_ssd);
module_exit(exit_ssd);
