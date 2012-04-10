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
#define BLOCK_BITMAP_SIZE PAGE_NUM_BLOCK/8
#define GMT_START  1

#define MEM_PAGE_SIZE 4096
#define HW_TO_MEM_PAGE (PHYS_PAGE_SIZE / MEM_PAGE_SIZE)

#define PAGE_NUM_BLOCK_SHIFT 7
#define PAGE_NUM_BLOCK  (1 << PAGE_NUM_BLOCK_SHIFT)

#define PAGE_TO_SECTOR(p)   (p << 3)
#define PAGE_TO_BLOCK(p)    (p >> PAGE_NUM_BLOCK_SHIFT)
#define PAGE_BLK_IDX(p)     (P & (PAGE_NUM_BLOCK - 1))

#define MDIR_SHIFT 10
#define LPN_TO_MDIR(lpn)    (lpn >> MDIR_SHIFT)
#define LPN_TO_MOFF(lpn)    (lpn & 0x3ff);

#define CMT_ENTRY_SHIFT 10
#define CMT_ENTRY_SIZE (1 << (CMT_ENTRY_SHIFT))
#define CMT_HASH_MASK(pfn)  (pfn & 0x3ff)

//#define PAGE_TO_SECTOR(block, offset) (((sector_t)block) * PAGE_NUM_BLOCK * PAGE_SECTOR + (offset) * PAGE_SECTOR )

#define PAGE_PFN_ENTRY(page, idx) ((pfn_t * )page_address((page)->data[(idx) >> 10]))[(idx) & 0x3ff]

#define NO_BIO_RESOURCE 1
#define NOT_ENOUGH_MEM  2
#define NO_BLK_DEV      3

typedef u32 pfn_t;

struct phys_page {
    struct list_head list;  // pages in the same block
    struct page ** data;    // page data
    u8 nents;               // number of mem pages
    u8 * oob;               // out of band data
    u32 ppn;                // physcial page number
    u16 rev;                // version number; a single page can be written for tens of thousands of times , so u16 is enough
    u8 pflag;
    int retval;
    struct gendisk * disk;
    struct rw_semaphore rw_sem;  // rw_sem for gdir memory entry access
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
    struct phys_page * pg;    // physical page
    bool dirty;
    pfn_t * mlist;          // mappings
    unsigned int nents;     // number of mapping entries in a single page
    u32 pbn;                // physcial block number
    u16 pbo;                // page index in physical block
};

struct global_mapping_block {
    struct list_head plist; // mapping pages list in the block
    struct list_head list;  // mapping block list
    struct phys_block block;// physcial block
};

struct cmt_entry {
    struct list_head hlist;
    u16 dirty;
    rwlock_t rw_lock;  // rw_sem for gdir memory entry access
};

struct cached_mapping_table {
    struct cmt_entry * el;
};

struct meta_root {
    u32 map_update_block;       // block address for mapping update region block
    u32 data_udpate_rg_list;    // block address for data updating region list
    u32 gc_start_page;          // page address in meta-data region for the gc list
};

struct hw_meta_root {
    struct phys_page * page;
    struct meta_root * mroot;
};

extern void init_mapping_dir(struct gendisk * disk);
extern void exit_mapping_dir(struct gendisk * disk);

#endif
