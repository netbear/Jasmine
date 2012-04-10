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
#include <linux/rwsem.h>

#include "ftl.h"
#include "ssd.h"

static void ftl_bio_destructor(struct bio * bio)
{
    struct bio_set * bs = bio->bi_private;

    bio_free(bio, bs);
}

static void read_endio(struct bio * bio, int error)
{
    struct phys_page * page = bio->bi_private;
    struct ssd_disk * sdk = ssd_disk(page->disk);

    up_write(&page->rw_sem);
    bio->bi_private = sdk->bs;
    bio_put(bio);
}

/* read a single page from the disk
 * @page: user should allocate buffer for the phys_page struct 
 * and initiate the block number and page offset
 */
static void read_phys_page(struct phys_page * page, bio_end_io_t bio_end)
{
    struct gendisk * disk = page->disk;
    struct ssd_disk * sdk = ssd_disk(disk);
    struct bio * bio = bio_alloc_bioset(GFP_NOIO, PHYS_PAGE_SIZE / MEM_PAGE_SIZE, sdk->bs);
    int i;

    if (!bio) {
        page->retval = -NO_BIO_RESOURCE;
        return;
    }

    bio->bi_sector = PAGE_TO_SECTOR(page->ppn);
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

    /*if (!bio->bi_bdev || !bio->bi_bdev->bd_disk) {
        page->retval = - NO_BLK_DEV;
        if (!bio->bi_bdev->bd_disk)
        printk(KERN_ERR "ftl: no block device can be found on bdev part %d\n", i);
        return;
    }*/

    bio->bi_idx = 0;
    bio->bi_destructor = ftl_bio_destructor;

    bio->bi_end_io = bio_end;
    bio->bi_private = page;
    bio->bi_flags |= (1 << BIO_CLONED);

    down_write(&page->rw_sem);

    generic_make_request(bio);
}

/*static void read_phys_block(struct gendisk * disk, struct phys_block * block) {
}

static void read_phys_area(struct gendisk * disk, unsigned int start, 
        unsigned int count, struct phys_page * pages) {
}*/

static struct phys_page * alloc_phys_page(void)
{
    struct phys_page * page = kmalloc(sizeof(struct phys_page), GFP_KERNEL);
    int i;

    if (!page)
        return NULL;
    init_rwsem(&page->rw_sem);

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

static void free_phys_page(struct phys_page * page)
{
    int i;
    for (i = 0; i < HW_TO_MEM_PAGE; i++) {
        if (page->data[i])
            __free_page(page->data[i]);
    }
    kfree(page->oob);
    kfree(page);
}

static void add_page_to_block(struct phys_page * page, struct phys_block * block)
{
    list_add(&page->list, &block->plist);
}

/*static void init_phys_block(struct phys_block * block , unsigned int pbn) {
    INIT_LIST_HEAD(&block->plist);
    block->pbn = pbn;
}*/

pfn_t get_page_dir(struct ssd_disk * sdk, pfn_t lpn)
{
    unsigned int idx, offset;
    struct gdir_entry * entry;
    pfn_t * pdir;
    pfn_t ret;

    idx = LPN_TO_MDIR(lpn) >> 10;
    offset = LPN_TO_MDIR(lpn) & 0x3ff;
    entry = &sdk->gmt.el[idx];
    down_read(&entry->hw_page->rw_sem);
    pdir = page_address(entry->page);
    ret = pdir[offset];
    up_read(&entry->hw_page->rw_sem);

    return ret;
}

/*
 * This two functions assume that the appropriated lock in cmt entry
 * is acquired and should not sleep.
 */
static inline pfn_t search_hash_mapping(pfn_t lpn, struct cmt_entry * ent)
{
    pfn_t lpdn,lpdo, ret = 0;
    struct global_mapping_page * mpage = NULL;

    lpdn = LPN_TO_MDIR(lpn);
    lpdo = LPN_TO_MOFF(lpn);

    if (!list_empty(&ent->hlist)) {
        list_for_each_entry(mpage, &ent->hlist, next) {
            if (mpage->lpdn == lpdn) {
                //ret = mpage->mlist[lpdo];
                ret = PAGE_PFN_ENTRY(mpage->pg, lpdo);
                return ret;
            }
        }
    }

    return 0;
}

static inline bool search_set_hash_mapping(pfn_t lpn, pfn_t ppn, struct cmt_entry * ent)
{
    pfn_t lpdn,lpdo;
    struct global_mapping_page * mpage = NULL;

    lpdn = LPN_TO_MDIR(lpn);
    lpdo = LPN_TO_MOFF(lpn);

    if (!list_empty(&ent->hlist)) {
        list_for_each_entry(mpage, &ent->hlist, next) {
            if (mpage->lpdn == lpdn) {
                //mpage->mlist[lpdo] = ppn;
                PAGE_PFN_ENTRY(mpage->pg, lpdo) = ppn;
                if (!mpage->dirty) {
                    mpage->dirty = true;
                    ent->dirty ++;
                }
                return true;
            }
        }
    }
    return false;
}

/*
 * Translate the logical page number in @disk into physcial page number
 * The mapping page may not exist, thus if @create is greater than zero,
 * the function will allocate memory mapping page and update the gmt.
 *
 * Return: zero when mapping does not exist (either no mapping page or
 * the related entry in the mapping page is empty) and the corresponding
 * ppn.
 */
pfn_t get_phys_ppn(struct gendisk * disk, pfn_t lpn, int create)
{
    struct ssd_disk * sdk = ssd_disk(disk);
    pfn_t lpdn,lpdo,hidx,dir,ret = 0;
    struct cmt_entry * ent;
    struct global_mapping_page * mpage = NULL;
    unsigned long flags;

    lpdn = LPN_TO_MDIR(lpn);
    lpdo = LPN_TO_MOFF(lpn);
    hidx = CMT_HASH_MASK(lpdn);
    ent = &sdk->cmt.el[hidx];

    dir = get_page_dir(sdk, lpn);
    if (!dir && !create)
        return 0;

    /*
     * seems that we don't need to disable interrupts
     */
    read_lock_irqsave(&ent->rw_lock, flags);
    ret = search_hash_mapping(lpn, ent);
    read_unlock_irqrestore(&ent->rw_lock, flags);
    if (ret)
        return ret;

    mpage = kmalloc(sizeof(struct global_mapping_page), GFP_KERNEL);
    mpage->pg =  alloc_phys_page();
    mpage->lpdn = lpdn;
    mpage->dirty = false;
    mpage->pg->ppn = dir;
    mpage->pg->disk = disk;
    read_phys_page(mpage->pg, read_endio);

    /*
     * cmt may be updated when reading mapping pages. so before we add the mapping page,
     * check whether the cmt contains the requested page.
     */
    write_lock_irqsave(&ent->rw_lock, flags);
    ret = search_hash_mapping(lpn, ent);
    if (ret) {
        write_unlock_irqrestore(&ent->rw_lock, flags);
        free_phys_page(mpage->pg);
        kfree(mpage);
        return ret;
    }

    list_add(&mpage->next, &ent->hlist);
    //ret = mpage->mlist[lpdo];
    ret = PAGE_PFN_ENTRY(mpage->pg, lpdo);

    write_unlock_irqrestore(&ent->rw_lock, flags);

    return ret;
}

void set_phys_ppn(struct gendisk * disk, pfn_t lpn, pfn_t ppn)
{
    struct ssd_disk * sdk = ssd_disk(disk);
    pfn_t lpdn,lpdo,hidx,dir;
    struct cmt_entry * ent;
    struct global_mapping_page * mpage = NULL;
    unsigned long flags;
    bool ret;

    lpdn = LPN_TO_MDIR(lpn);
    lpdo = LPN_TO_MOFF(lpn);
    hidx = CMT_HASH_MASK(lpdn);
    ent = &sdk->cmt.el[hidx];

    dir = get_page_dir(sdk, lpn);

    if (dir) {
        write_lock_irqsave(&ent->rw_lock, flags);
        ret = search_set_hash_mapping(lpn, ppn, ent);
        write_unlock_irqrestore(&ent->rw_lock, flags);
        if (ret)
            return;
    }

    mpage = kmalloc(sizeof(struct global_mapping_page), GFP_KERNEL);
    mpage->pg = alloc_phys_page();
    mpage->lpdn = lpdn;
    mpage->dirty = true;
    mpage->pg->ppn = dir;
    mpage->pg->disk = disk;
    if (dir)
        read_phys_page(mpage->pg, read_endio);

    /*
     * cmt may be updated when reading mapping pages. so before we add the mapping page,
     * check whether the cmt contains the requested page.
     */
    write_lock_irqsave(&ent->rw_lock, flags);
    ret = search_set_hash_mapping(lpn, ppn, ent);
    if (ret) {
        write_unlock_irqrestore(&ent->rw_lock, flags);
        free_phys_page(mpage->pg);
        kfree(mpage);
        return;
    }

    list_add(&mpage->next, &ent->hlist);
    PAGE_PFN_ENTRY(mpage->pg, lpdo) = ppn;
    ent->dirty ++;

    write_unlock_irqrestore(&ent->rw_lock, flags);
}

void flush_mapping_pages(struct gendisk * disk)
{
    int i;
    unsigned long flags;
    struct cmt_entry * ent;
    struct ssd_disk * sdk = ssd_disk(disk);
    struct global_mapping_page * mpage = NULL;

    /* TODO:
     * We should prevent the data updating ops to update the mapping entries
     * before the flush opertations finished
     */

    /*
     * Add dirty pages to flush list, clear the dirty flag
     */
    for (i = 0; i < CMT_ENTRY_SIZE; i++) {
        ent = &sdk->cmt.el[i];
        if (ent->dirty) {
            write_lock_irqsave(&ent->rw_lock, flags);
            if (!list_empty(&ent->hlist)) {
                list_for_each_entry(mpage, &ent->hlist, next) {
                    if (mpage->dirty) {
                        list_add(&mpage->list, &sdk->mflush_list);
                        mpage->dirty = false;
                    }
                }
            }
            ent->dirty = 0;
            write_unlock_irqrestore(&ent->rw_lock, flags);
        }
    }
}

void init_mapping_dir(struct gendisk * disk)
{
    struct ssd_disk * sdk = ssd_disk(disk);
    u32 i, pi;
    int err;
    u64 nr_pages = sdk->capacity / (PAGE_SECTOR * PHYS_PAGE_SIZE / sizeof(void *));
    struct blk_plug plug;

    nr_pages /= (PHYS_PAGE_SIZE / sizeof(void *));
    SDEBUG("GMT: %llx pages, with capacity %llx sectors\n", nr_pages, sdk->capacity);

    sdk->gmt.el = vmalloc(sizeof(struct gdir_entry) * nr_pages * HW_TO_MEM_PAGE);
    sdk->cmt.el = vmalloc(sizeof(struct cmt_entry) * CMT_ENTRY_SIZE);

    if (!sdk->gmt.el) {
        printk(KERN_ERR "ftl: cannot vmalloc gmt entries!\n");
        return;
    }

    if (!sdk->cmt.el) {
        printk(KERN_ERR "ftl: cannot vmalloc cmt entries\n");
        return;
    }

    memset(sdk->cmt.el, 0, sizeof(struct cmt_entry) * CMT_ENTRY_SIZE);
    for (i = 0; i < CMT_ENTRY_SIZE; i++) {
        INIT_LIST_HEAD(&sdk->cmt.el[i].hlist);
        rwlock_init(&sdk->cmt.el[i].rw_lock);
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

    blk_start_plug(&plug);
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
        page->ppn = i;
        read_phys_page(page, read_endio);
    }

    blk_finish_plug(&plug);

    if (sdk->bdev && !(sdk->bdev_err < 0)) {
        SDEBUG("put blk device\n");
        blkdev_put(sdk->bdev, FMODE_READ);
    }

    /*if (sdk->bdev)
        bdput(sdk->bdev);*/
}

void exit_mapping_dir(struct gendisk * disk)
{
    struct ssd_disk * sdk = ssd_disk(disk);
    struct list_head * ptr, * next;
    struct phys_page * page = NULL;
    SDEBUG("GMT: exit mapping dir\n");
    
    list_for_each_safe(ptr, next, &sdk->gmt.list) {
        page = list_entry(ptr, typeof(*page), list);
        free_phys_page(page);
    }

    if (sdk->cmt.el)
        vfree(sdk->cmt.el);

    if (sdk->gmt.el)
        vfree(sdk->gmt.el);
}
