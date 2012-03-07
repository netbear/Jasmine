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
#define CMT_SIZE  8192

#ifdef SSD_DEBUG
    #define SDEBUG(fmt, args...) printk( KERN_DEBUG "ss: " fmt, ##args)
#else
    #define SDEBUG(fmt, args...)
#endif

struct ssd_disk {
    struct list_head list;
    struct gendisk * gd;
    struct block_device * bdev;
    const char * name;
    make_request_fn * old_request_fn;
};

#endif
