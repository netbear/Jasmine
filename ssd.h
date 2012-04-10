/*
 * =====================================================================================
 *
 *       Filename:  ssd.h
 *
 *    Description:  header for ssd.c
 *
 *        Version:  1.0
 *        Created:  03/06/2012 11:34:53 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Xiaolin Guo (guoxl), ringoguo@gmail.com
 *        Company:  Tsinghua Univ
 *
 * =====================================================================================
 */

#ifndef _SSD_H_
#define _SSD_H_

#include <linux/list.h>

#define SSD_DEBUG

#define SSD_MAJOR 4
#define SSD_MINORS 16

#define SSD_TIMEOUT (30 * HZ)
#define SECTOR_SHIFT 9

enum {
    CDB_SIZE=32, /* scsi cmd size */
    SSD_BIOS=256, /* ssd bio pool size */
    MEMPOOL_SIZE=4, /* pool size */
};

enum {
	SSD_LBP_FULL = 0,	/* Full logical block provisioning */
	SSD_LBP_UNMAP,		/* Use UNMAP command */
	SSD_LBP_WS16,		/* Use WRITE SAME(16) with UNMAP bit */
	SSD_LBP_WS10,		/* Use WRITE SAME(10) with UNMAP bit */
	SSD_LBP_ZERO,		/* Use WRITE SAME(10) with zero payload */
	SSD_LBP_DISABLE,		/* Discard disabled due to failed cmd */
};

#ifdef SSD_DEBUG
    #define SDEBUG(fmt, args...) printk( KERN_INFO "ss: " fmt, ##args)
#else
    #define SDEBUG(fmt, args...)
#endif

struct ssd_disk {
    struct list_head list;
    struct list_head mflush_list;
    struct gendisk * gd;
    struct scsi_device * device;
    struct block_device * bdev;
    const char * name;
    make_request_fn * old_make_request_fn;
    prep_rq_fn * old_prep_fn;
    request_fn_proc * old_request_fn;
    u8 protection_type;
    u8 provisioning_mode;
    sector_t capacity;
    struct hw_meta_root root;
    struct global_mapping_dir gmt;
    struct cached_mapping_table cmt;
    int bdev_err;

    struct bio_set * bs;
    mempool_t * io_pool;
};

static inline struct ssd_disk * ssd_disk(struct gendisk * disk) {
    return container_of(disk->private_data, struct ssd_disk, list);
}

static inline sector_t to_sector(unsigned long n)
{
    return (n >> SECTOR_SHIFT);
}

static inline unsigned long to_bytes(sector_t n)
{
    return (n << SECTOR_SHIFT);
}

#endif
