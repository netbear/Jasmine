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
#define MAP_REGION_START 1
#define PAGE_NUM_BLOCK  128
#define BLOCK_BITMAP_SIZE PAGE_NUM_BLOCK/8

struct phys_page {
    struct list_head list;  // pages in the same block
    void * data;            // page data
    u8 * oob;               // out of band data
    u32 block;              // phys block no
    u16 offset;             // offset inside the block
    u16 rev;                // version number; a single page can be written for tens of thousands of times , so u16 is enough
    u8 pflag;
    bool invalid;
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
    u32 ppn;
    u16 rev;
};

struct global_mapping_dir {
    struct list_head list;
    struct gdir_entry ** entry_list;
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

#endif
