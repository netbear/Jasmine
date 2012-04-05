/*
 * =====================================================================================
 *
 *       Filename:  ftl.h
 *
 *    Description:  header file for flash transltion layer module
 *                  define structs such as global mapping directory(GMD), global mapping table
 *                  (GMT), update mapping table and etc.  
 *
 *        Version:  1.0
 *        Created:  03/14/2012 05:12:46 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Xiaolin Guo (guoxl), ringoguo@gmail.com
 *        Company:  Tsinghua Univ
 *
 * =====================================================================================
 */

#ifndef _FTL_H_
#define _FTL_H_

#define MAP_FLG_IVD  0x1

#define PHYS_PAGE_SIZE 4096
#define PHYS_OOB_SIZE  16*8
#define PAGE_SECTOR (PHYS_PAGE_SIZE >> 9)
#define PAGE_SECTOR_MASK ~0x7L
#define MAP_REGION_LIST_SIZE 64
#define GMT_START  1
#define PAGE_NUM_BLOCK_BIT 7
#define PAGE_NUM_BLOCK  (1 << PAGE_NUM_BLOCK_BIT)
#define BLOCK_BITMAP_SIZE PAGE_NUM_BLOCK/8

#define MEM_PAGE_SIZE 4096
#define HW_TO_MEM_PAGE (PHYS_PAGE_SIZE / MEM_PAGE_SIZE)

#define PAGE_TO_SECTOR(block, offset) (((sector_t)block) * PAGE_NUM_BLOCK * PAGE_SECTOR + (offset) * PAGE_SECTOR )

#define NO_BIO_RESOURCE 1
#define NOT_ENOUGH_MEM  2
#define NO_BLK_DEV      3

struct phys_page {
    struct list_head list;  // pages in the same block
    struct page ** data;    // page data
    u8 nents;               // number of mem pages
    u8 * oob;               // out of band data
    u32 block;              // phys block no
    u16 offset;             // offset inside the block
    u16 rev;                // version number; a single page can be written for tens of thousands of times , so u16 is enough
    u8 pflag;
    int retval;
    struct gendisk * disk;
};

struct phys_block {
    struct list_head plist; // pages list
    u32 pbn;                // block no
    u8 unused;              // the first page that is unused in the block
    u8 inv_cnt;             // the number of invalid pages
};

struct block_info {
    u32 block;
    u8 bitmap[BLOCK_BITMAP_SIZE];
};

struct block_container {
    struct block_info ** blist;
    u32 nents;
};

struct gdir_entry {
    struct phys_page * hw_page;  // page of mapping dir in hardware
    bool dirty;             // write back flag
    struct page * page;     // mem page
};

struct global_mapping_dir {
    struct list_head list;
    struct gdir_entry * el;
    unsigned int nents;
};

struct global_mapping_page {
    struct list_head list;  // global list for mapping pages
    struct list_head next;  // list used by hash table
    unsigned int lpdn;      // logical page directory number , index in global dir
    u32 mflags;             // flags
    struct phys_page pg;    // physical page
    u32 * mlist;            // mappings
    unsigned int nents;     // number of mapping entries in a single page
    u32 pbn;                // physcial block number
    u16 pbo;                // page index in physical block
};

struct global_mapping_block {
    struct list_head plist; // mapping pages list in the block
    struct list_head list;  // mapping block list
    struct phys_block block;// physcial block
};

struct map_region_block {
    struct phys_block block;
    struct phys_page * pages[PAGE_NUM_BLOCK];
};

struct mapping_region {
    u32 * blist;            // mapping block list
    u8 half;                // use first half or second half
    struct map_region_block * rg[MAP_REGION_LIST_SIZE];
    u16 total_pages;        // total pages number in mapping list
    u16 cur;                // the first page which contains unused mapping region 
    u8 offset;              // the offset of the entry in the above page
};

extern void init_mapping_dir(struct gendisk * disk);
extern void exit_mapping_dir(struct gendisk * disk);

#endif
