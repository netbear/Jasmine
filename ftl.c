/*
 * =====================================================================================
 *
 *       Filename:  ftl.c
 *
 *    Description:  flash translation layer
 *
 *        Version:  1.0
 *        Created:  03/15/2012 08:00:11 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Xiaolin Guo (guoxl), ringoguo@gmail.com
 *        Company:  Tsinghua Univ
 *
 * =====================================================================================
 */

#include <linux/list.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/mempool.h>

#include "ftl.h"
#include "ssd.h"

static void ftl_bio_destructor(struct bio * bio)
{
    struct bio_set * bs = bio->bi_private;

    //SDEBUG("ftl bio freed : sector %llx\n", bio->bi_sector);
    bio_free(bio, bs);
}

static void read_endio(struct bio * bio, int error)
{
    struct phys_page * page = bio->bi_private;
    struct ssd_disk * sdk = ssd_disk(page->disk);

    bio->bi_private = sdk->bs;
    bio_put(bio);
}

/* read a single page from the disk
 * @page: user should allocate buffer for the phys_page struct 
 * and initiate the block number and page offset
 */
static void read_phys_page(struct phys_page * page, bio_end_io_t bio_end) {
    struct gendisk * disk = page->disk;
    struct ssd_disk * sdk = ssd_disk(disk);
    struct bio * bio = bio_alloc_bioset(GFP_NOIO, PHYS_PAGE_SIZE / MEM_PAGE_SIZE, sdk->bs);
    int i;

    if (!bio) {
        page->retval = -NO_BIO_RESOURCE;
        return;
    }

    bio->bi_sector = PAGE_TO_SECTOR(page->block, page->offset);
    bio->bi_size = PHYS_PAGE_SIZE;
    bio->bi_vcnt = PHYS_PAGE_SIZE / MEM_PAGE_SIZE;

    if (!page->data || bio->bi_vcnt > page->nents) {
        page->retval = - NOT_ENOUGH_MEM;
        return;
    }

    for (i = 0; i < bio->bi_vcnt; i++) {
        bio->bi_io_vec[i].bv_page = page->data[i];
        bio->bi_io_vec[i].bv_offset = 0;
        bio->bi_io_vec[i].bv_len = MEM_PAGE_SIZE;
    }

    bio->bi_bdev = sdk->bdev;

    if (!bio->bi_bdev || !bio->bi_bdev->bd_disk) {
        page->retval = - NO_BLK_DEV;
        if (!bio->bi_bdev->bd_disk)
        printk(KERN_ERR "ftl: no block device can be found on bdev part %d\n", i);
        return;
    }

    bio->bi_idx = 0;
    bio->bi_destructor = ftl_bio_destructor;

    bio->bi_end_io = bio_end;
    bio->bi_private = page;
    bio->bi_flags |= (1 << BIO_CLONED);

    generic_make_request(bio);
}

/*static void read_phys_block(struct gendisk * disk, struct phys_block * block) {
}

static void read_phys_area(struct gendisk * disk, unsigned int start, 
        unsigned int count, struct phys_page * pages) {
}*/

static struct phys_page * alloc_phys_page(void) {
    struct phys_page * page = kmalloc(sizeof(struct phys_page), GFP_KERNEL);
    int i;

    if (!page)
        return NULL;

    page->data = kmalloc(sizeof(struct page *) * HW_TO_MEM_PAGE, GFP_KERNEL);

    if (!page->data) {
        printk(KERN_ERR "ftl: not enough memory\n");
        kfree(page);
        return NULL;
    }

    for (i = 0; i < HW_TO_MEM_PAGE; i ++) {
        page->data[i] = alloc_page(GFP_KERNEL);
        if (!page->data[i])
            goto err_out;
    }
    page->nents = HW_TO_MEM_PAGE;

    page->oob = kmalloc(PHYS_OOB_SIZE, GFP_KERNEL);
    if (!page->oob)
        goto err_out;

    return page;

err_out:
    for (i = 0; i < HW_TO_MEM_PAGE; i++) {
        if (page->data[i])
            __free_page(page->data[i]);
    }
    kfree(page);
    return NULL;
}

static void free_phys_page(struct phys_page * page) {
    int i;
    for (i = 0; i < HW_TO_MEM_PAGE; i++) {
        if (page->data[i])
            __free_page(page->data[i]);
    }
    kfree(page->oob);
    kfree(page);
}

static void add_page_to_block(struct phys_page * page, struct phys_block * block) {
    list_add(&page->list, &block->plist);
}

/*static void init_phys_block(struct phys_block * block , unsigned int pbn) {
    INIT_LIST_HEAD(&block->plist);
    block->pbn = pbn;
}*/

static struct map_region_block * alloc_map_region_block(void) {
    struct map_region_block * mr_block = kzalloc(sizeof(struct map_region_block), GFP_KERNEL);
    int i;
    INIT_LIST_HEAD(&mr_block->block.plist);
    for (i = 0; i < PAGE_NUM_BLOCK; i++) {
        mr_block->pages[i] = alloc_phys_page();
        add_page_to_block(mr_block->pages[i], &mr_block->block);
    }
    return mr_block;
}

void init_mapping_dir(struct gendisk * disk) {
    struct ssd_disk * sdk = ssd_disk(disk);
    u32 i, pi;
    int err;
    u64 nr_pages = sdk->capacity / (PAGE_SECTOR * PHYS_PAGE_SIZE / sizeof(void *)) + 1;
    SDEBUG("GMT: %llx pages, with capacity %llx sectors\n", nr_pages, sdk->capacity);

    sdk->gmt.el = vmalloc(sizeof(struct gdir_entry) * nr_pages * HW_TO_MEM_PAGE);

    if (!sdk->gmt.el) {
        printk(KERN_ERR "ftl: cannot vmalloc gmt entries!\n");
        return;
    }

    sdk->bdev = bdget_disk(disk, 0);
    if (!sdk->bdev) {
        printk(KERN_ERR "ftl: cannot get bdev from gendisk!\n");
        return;
    }

    err = blkdev_get(sdk->bdev, FMODE_READ, NULL);
    if (err < 0) {
        sdk->bdev_err = err;
        printk(KERN_ERR "ftl: cannot get block device!\n");
        return;
    }

    INIT_LIST_HEAD(&sdk->gmt.list);
    for (i = 0; i < nr_pages; i++) {
        struct phys_page * page = alloc_phys_page();
        list_add(&page->list, &sdk->gmt.list);
        for (pi = i * HW_TO_MEM_PAGE; pi < (i + 1) * HW_TO_MEM_PAGE; pi ++) {
            sdk->gmt.el[pi].hw_page = page;
            sdk->gmt.el[pi].page = page->data[pi - i * HW_TO_MEM_PAGE];
            sdk->gmt.el[pi].dirty = 0;
        }
        page->disk = disk;
        page->block = i / PAGE_NUM_BLOCK;
        page->offset = i % PAGE_NUM_BLOCK;
        read_phys_page(page, read_endio);
    }
    if (sdk->bdev && !(sdk->bdev_err < 0)) {
        SDEBUG("put blk device\n");
        blkdev_put(sdk->bdev, FMODE_READ);
    }
    /*if (sdk->bdev)
        bdput(sdk->bdev);*/
}

void exit_mapping_dir(struct gendisk * disk) {
    struct ssd_disk * sdk = ssd_disk(disk);
    struct list_head * ptr, * next;
    struct phys_page * page = NULL;
    SDEBUG("GMT: exit mapping dir\n");
    
    list_for_each_safe(ptr, next, &sdk->gmt.list) {
        page = list_entry(ptr, typeof(*page), list);
        free_phys_page(page);
    }

    if (sdk->gmt.el)
        vfree(sdk->gmt.el);
}
