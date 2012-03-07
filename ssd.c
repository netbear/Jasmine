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
make_request_fn * mrf = NULL;

const char * ssds[] = { "/dev/sda", "kkk",};

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

static int format_disk_name(char * prefix, int index, char * buf, int len)
{
    const int base = 26;
    char * begin = buf + strlen(prefix);
    char * end = buf + len;
    char * p = end - 1;
    *p = '\0';

    while (--p >= begin) {
        *p = 'a' + (index % base);
        index = (index / base) - 1;
        if (index < 0)
            break;
    }

    if (p < begin)
        return -EINVAL;

    memmove(begin, p, end - p);
    memcpy(buf, prefix, strlen(prefix));

    return 0;
}

static int ss_make_request_fn(struct request_queue * q, struct bio * bio)
{
    SDEBUG("Issue Request %llx %x sectors\n", bio->bi_sector, bio_sectors(bio));
    return mrf(q,bio);
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
    int i, found = 0, partno;
    struct block_device * bdev;
    struct ssd_disk * sdk;
    struct gendisk * gd, * oldgd;

    for (i = 0; i < sizeof(ssds) / sizeof(ssds[0]); i++) {
        bdev = lookup_bdev(ssds[i]);
        if (!IS_ERR_OR_NULL(bdev)) {
            SDEBUG("%s as ssd wi soft-ftl found!\n", ssds[i]);
            sdk = kzalloc(sizeof(struct ssd_disk), GFP_KERNEL);
            list_add(&sdk->list, &ssd_list);
            sdk->bdev = bdev;
            sdk->name = ssds[i];
            gd = alloc_disk(SSD_MINORS);
            if (!gd) {
                printk(KERN_ERR "ss: cannot alloc gendisk!\n");
                continue;
            }
            format_disk_name("ss", i, gd->disk_name, DISK_NAME_LEN);
            SDEBUG("disk %s created\n", gd->disk_name);

            oldgd = get_gendisk(bdev->bd_dev, &partno);
            if (!oldgd)
                printk(KERN_ERR "ss: cannot get gendisk associated with the block device!\n");
            else
                gd->queue = oldgd->queue;

            if (!gd->queue)
                printk(KERN_ERR "ss: cannot get request queue !\n");
            else {
                sdk->old_request_fn = gd->queue->make_request_fn;
                gd->queue->make_request_fn = ss_make_request_fn;
                mrf = sdk->old_request_fn;
            }

            gd->fops = &ss_fops;
            sdk->gd = gd;
            //add_disk(gd);
            found ++;
        }
    }

    return found;
}

static void destroy_disk(void)
{
    struct list_head * ptr, *next;
    struct ssd_disk * sdk = NULL;

    list_for_each_safe(ptr, next, &ssd_list) {
        sdk = list_entry(ptr, typeof(*sdk), list);
        SDEBUG("%s freed\n", sdk->gd->disk_name);
        sdk->gd->queue->make_request_fn = sdk->old_request_fn;
        //del_gendisk(sdk->gd);
        list_del(ptr);
        if (sdk == NULL)
            printk(KERN_ERR "ss: null ssd_disk!\n");
        else
            kfree(sdk);
    } 
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

    if (!find_and_init_disk()) {
        err = -ENODEV;
        goto err_out;
    }

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
    destroy_disk();
    for (i = 0; i < SSD_MAJOR; i ++)
        unregister_blkdev(ssd_major[i], "ss");
}

module_init(init_ssd);
module_exit(exit_ssd);
