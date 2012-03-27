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

#include "ftl.h"

/* read a single page from the disk
 * @page: user should allocate buffer for the phys_page struct 
 * and initiate the block number and page offset
 */
static void read_phys_page(struct gendisk * disk, struct phys_page * page) {
}

static void read_phys_block(struct gendisk * disk, struct phys_block * block) {
}

static void read_phys_area(struct gendisk * disk, unsigned int start, 
        unsigned int count, struct phys_page * pages) {
}

static struct phys_page * alloc_phys_page(void) {
    return NULL;
}

static void add_page_to_block(struct phys_page * page, struct phys_block * block) {
    list_add(&page->list, &block->plist);
}

static void init_phys_block(struct phys_block * block , unsigned int pbn) {
    INIT_LIST_HEAD(&block->plist);
    block->pbn = pbn;
}

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

static void init_mapping_region(struct gendisk * disk) {
    struct ssd_disk * sdkp = ssd_disk(disk);
    unsigned int i;
    for (i = MAP_REGION_START; i < MAP_REGION_START + MAP_REGION_SIZE; i++) {
        struct map_region_block * mr_block = alloc_map_region_block();
        sdkp->m_region.rg[i] = mr_block;
        mr_block->block.pnb = i;
        read_phys_block(disk, &mr_block->block);
    }
}
