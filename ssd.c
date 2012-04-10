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
#include <asm/unaligned.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

//include sd module function, should be removed in the future;
#include <../drivers/scsi/sd.h>
#include <../drivers/scsi/scsi_logging.h>

#include "ftl.h"
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

static struct kmem_cache * ss_cdb_cache;
static struct kmem_cache * ss_io_cache;
static mempool_t * ss_cdb_pool;

make_request_fn * mrf = NULL;

const char * ssds[] = { "/dev/sdb",};

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

struct ss_io {
    int error;
    atomic_t io_count;
    unsigned int start_time;
    struct bio * bio;
    struct ssd_disk * sd;
    spinlock_t endio_lock;
};

struct clone_info {
    struct bio * bio;
    struct ss_io * io;
    sector_t sector;
    sector_t sector_count;
    unsigned int idx;
};

static struct ss_io * alloc_io(struct ssd_disk * sdk)
{
    return mempool_alloc(sdk->io_pool, GFP_NOIO);
}

static void free_io(struct ssd_disk * sdk, struct ss_io * io)
{
    mempool_free(io, sdk->io_pool);
}

static void ss_bio_destructor(struct bio * bio)
{
    struct bio_set * bs = bio->bi_private;

    SDEBUG("ss bio destructor: execute %llx\n", bio->bi_sector);
    bio_free(bio, bs);
}

static void dec_pending(struct ss_io * io, int error)
{
    unsigned long flags;
    struct bio * bio;
    int io_error;
    /*
     * we are supposed to push back any error bio here
     * should be added in the future
     */
    if (unlikely(error)) {
        spin_lock_irqsave(&io->endio_lock, flags);
        io->error = error;
        spin_unlock_irqrestore(&io->endio_lock, flags);
    }

    if (atomic_dec_and_test(&io->io_count)) {
        bio = io->bio;
        io_error = io->error;
        free_io(io->sd, io);
        bio_endio(bio, io_error);
    }
}

static void clone_endio(struct bio * bio, int error)
{
    struct ss_io * sio = bio->bi_private;
    struct ssd_disk * sd = sio->sd;

    bio->bi_private = sd->bs;
    bio_put(bio);
    dec_pending(sio, error);
}

/*
 * clone a bio from existing bio, starting from @sector with @len sects
 * bio_vec used starts from @idx in the bio_vec. @idx will be updated to
 * be set to the uncloned entry with @offset also updated. 
 */
static struct bio * clone_bio(struct bio * bio, sector_t sector, unsigned int * idx, sector_t * offset, sector_t len, struct bio_set * bs)
{
    struct bio * clone = NULL;
    struct bio_vec * bv = bio->bi_io_vec + *idx, * nbv;
    sector_t remaining = len;
    clone = bio_alloc_bioset(GFP_NOIO, bio->bi_max_vecs, bs);

    if (!clone)
        return NULL;

    nbv = clone->bi_io_vec;

    for (; bv < bio->bi_io_vec + bio->bi_vcnt; bv ++) {
        if (to_sector(bv->bv_len) - *offset > remaining)
            break;
        nbv->bv_page = bv->bv_page;
        nbv->bv_offset = bv->bv_offset + to_bytes(*offset);
        nbv->bv_len = bv->bv_len - to_bytes(*offset);
        nbv ++;
        remaining -= (to_sector(bv->bv_len) - *offset);
        *offset = 0;
    }

    if (remaining) {
        /*
         *  if there is any io_vec left , 
         *  we should not reach the end of the io vector list
         */
        BUG_ON(bv == bio->bi_io_vec + bio->bi_vcnt);

        nbv->bv_page = bv->bv_page;
        nbv->bv_offset = bv->bv_offset + to_bytes(*offset);
        nbv->bv_len = to_bytes(remaining);
        nbv ++;
        *offset += remaining;
    } else
        *offset = 0;

    clone->bi_vcnt = nbv - clone->bi_io_vec;
    clone->bi_sector = sector;
    clone->bi_size = to_bytes(len);
    clone->bi_destructor = ss_bio_destructor;
    clone->bi_bdev = bio->bi_bdev;
    clone->bi_rw = bio->bi_rw;
    clone->bi_idx = 0;
    // we have no idea what flags we should use, should check in the future
    clone->bi_flags = bio->bi_flags | (1 << BIO_CLONED);

    *idx = bv - bio->bi_io_vec;

    return clone;
}

static void map_bio(struct bio * clone, struct ss_io * sio)
{
    clone->bi_end_io = clone_endio;
    clone->bi_private = sio;

    atomic_inc(&sio->io_count);
    generic_make_request(clone);
}

static int __clone_and_map(struct clone_info * ci)
{
    struct bio * clone, *bio = ci->bio;
    struct bio_set * bs =  ci->io->sd->bs;

    sector_t ns, len, offset;
    ns = (ci->sector + PAGE_SECTOR) & PAGE_SECTOR_MASK;
    if (ns - ci->sector > ci->sector_count)
        len = ci->sector_count;
    else
        len = ns - ci->sector;

    offset = 0;
    while (ci->sector_count) {
        SDEBUG("Clone Request %llx with %llx sectors\n", ci->sector, len);
        ci->sector_count -= len;

        clone = clone_bio(bio, ci->sector, &ci->idx, &offset, len, bs);
        map_bio(clone, ci->io);

        if (ci->sector_count < PAGE_SECTOR)
            len = ci->sector_count;
        else
            len = PAGE_SECTOR;
        ci->sector += len;
    }
    return 0;
}

static void ss_prot_op(struct scsi_cmnd *scmd, unsigned int dif)
{
    unsigned int prot_op = SCSI_PROT_NORMAL;
    unsigned int dix = scsi_prot_sg_count(scmd);

    if (scmd->sc_data_direction == DMA_FROM_DEVICE) {
        if (dif && dix)
            prot_op = SCSI_PROT_READ_PASS;
        else if (dif && !dix)
            prot_op = SCSI_PROT_READ_STRIP;
        else if (!dif && dix)
            prot_op = SCSI_PROT_READ_INSERT;
    } else {
        if (dif && dix)
            prot_op = SCSI_PROT_WRITE_PASS;
        else if (dif && !dix)
            prot_op = SCSI_PROT_WRITE_INSERT;
        else if (!dif && dix)
            prot_op = SCSI_PROT_WRITE_STRIP;
    }

    scsi_set_prot_op(scmd, prot_op);
    scsi_set_prot_type(scmd, dif);
}

/**
 * scsi_setup_discard_cmnd - unmap blocks on thinly provisioned device
 * @sdp: scsi device to operate one
 * @rq: Request to prepare
 *
 * Will issue either UNMAP or WRITE SAME(16) depending on preference
 * indicated by target device.
 **/
static int scsi_setup_discard_cmnd(struct scsi_device *sdp, struct request *rq)
{
    struct ssd_disk *sdkp = ssd_disk(rq->rq_disk);
    struct bio *bio = rq->bio;
    sector_t sector = bio->bi_sector;
    unsigned int nr_sectors = bio_sectors(bio);
    unsigned int len;
    int ret;
    char *buf;
    struct page *page;

    if (sdkp->device->sector_size == 4096) {
        sector >>= 3;
        nr_sectors >>= 3;
    }

    rq->timeout = SSD_TIMEOUT;

    memset(rq->cmd, 0, rq->cmd_len);

    page = alloc_page(GFP_ATOMIC | __GFP_ZERO);
    if (!page)
        return BLKPREP_DEFER;

    switch (sdkp->provisioning_mode) {
    case SD_LBP_UNMAP:
        buf = page_address(page);

        rq->cmd_len = 10;
        rq->cmd[0] = UNMAP;
        rq->cmd[8] = 24;

        put_unaligned_be16(6 + 16, &buf[0]);
        put_unaligned_be16(16, &buf[2]);
        put_unaligned_be64(sector, &buf[8]);
        put_unaligned_be32(nr_sectors, &buf[16]);

        len = 24;
        break;

    case SD_LBP_WS16:
        rq->cmd_len = 16;
        rq->cmd[0] = WRITE_SAME_16;
        rq->cmd[1] = 0x8; /* UNMAP */
        put_unaligned_be64(sector, &rq->cmd[2]);
        put_unaligned_be32(nr_sectors, &rq->cmd[10]);

        len = sdkp->device->sector_size;
        break;

    case SD_LBP_WS10:
    case SD_LBP_ZERO:
        rq->cmd_len = 10;
        rq->cmd[0] = WRITE_SAME;
        if (sdkp->provisioning_mode == SD_LBP_WS10)
            rq->cmd[1] = 0x8; /* UNMAP */
        put_unaligned_be32(sector, &rq->cmd[2]);
        put_unaligned_be16(nr_sectors, &rq->cmd[7]);

        len = sdkp->device->sector_size;
        break;

    default:
        ret = BLKPREP_KILL;
        goto out;
    }

    blk_add_request_payload(rq, page, len);
    ret = scsi_setup_blk_pc_cmnd(sdp, rq);
    rq->buffer = page_address(page);

out:
    if (ret != BLKPREP_OK) {
        __free_page(page);
        rq->buffer = NULL;
    }
    return ret;
}

static int scsi_setup_flush_cmnd(struct scsi_device *sdp, struct request *rq)
{
    rq->timeout = SD_FLUSH_TIMEOUT;
    rq->retries = SD_MAX_RETRIES;
    rq->cmd[0] = SYNCHRONIZE_CACHE;
    rq->cmd_len = 10;

    return scsi_setup_blk_pc_cmnd(sdp, rq);
}


static void ss_make_request_fn(struct request_queue * q, struct bio * bio)
{
    struct gendisk * gdisk;
    struct ssd_disk * sdk;
    struct clone_info ci;
    int error;
    BUG_ON(bio == NULL);
    /*if (bio->bi_flags & (1<<BIO_QUIET)) {
        bio_endio(bio, -EIO);
        return;
    }*/

    if (bio->bi_bdev && !(bio->bi_flags & (1 << BIO_CLONED))) {
        SDEBUG("Issue Rquest %llx %x sectors flg %lx\n", bio->bi_sector, bio_sectors(bio), bio->bi_flags);
        gdisk = bio->bi_bdev->bd_disk;
        sdk = ssd_disk(gdisk);
        ci.bio = bio;
        ci.io = alloc_io(sdk);
        ci.io->sd = sdk;
        ci.io->bio = bio;
        ci.io->error = 0;
        atomic_set(&ci.io->io_count, 1);
        spin_lock_init(&ci.io->endio_lock);
        ci.sector = bio->bi_sector;
        ci.idx = bio->bi_idx;
        ci.sector_count = bio_sectors(bio);
        error = __clone_and_map(&ci);

        // bio split done, drop the extra ref count
        dec_pending(ci.io, error);
    } else {
        if (!bio->bi_bdev)
            SDEBUG("Issue Rquest %llx %x sectors, no block dev\n", bio->bi_sector, bio_sectors(bio));
        blk_queue_bio(q,bio);
    }
}

/* Build a scsi command and initiate the block address, including the flash device address
 * translation. Will call the ftl module.
 *
 * Returns 1 if successful and 0 if error
 */
static int ss_prep_rq_fn(struct request_queue * q, struct request * rq)
{
    struct scsi_device * sdp = q->queuedata;
    struct ssd_disk * sdkp = ssd_disk(rq->rq_disk);
    struct scsi_cmnd *SCpnt;
    struct gendisk *disk = rq->rq_disk;
    sector_t block = blk_rq_pos(rq);
    sector_t threshold;
    unsigned int this_count = blk_rq_sectors(rq);
    int ret, host_dif;
    unsigned char protect;

    BUG_ON(!sdp->host);

    /*
     * Discard request come in as REQ_TYPE_FS but we turn them into
     * block PC requests to make life easier.
     */

    if (rq->cmd_flags & REQ_DISCARD) {
        ret = scsi_setup_discard_cmnd(sdp, rq);
        goto out;
    } else if (rq->cmd_flags & REQ_FLUSH) {
        ret = scsi_setup_flush_cmnd(sdp, rq);
        goto out;
    } else  if (rq->cmd_type == REQ_TYPE_BLOCK_PC) {
        ret = scsi_setup_blk_pc_cmnd(sdp, rq);
        goto out;
    } else if (rq->cmd_type != REQ_TYPE_FS) {
        ret = BLKPREP_KILL;
        goto out;
    }
    ret = scsi_setup_fs_cmnd(sdp, rq);
    if (ret != BLKPREP_OK)
        goto out;
    SCpnt = rq->special;

    /* from here on until we're complete, any goto out
     * is used for a killable error condition */
    ret = BLKPREP_KILL;

    SCSI_LOG_HLQUEUE(1, scmd_printk(KERN_INFO, SCpnt,
                    "sd_init_command: block=%llu, "
                    "count=%d\n",
                    (unsigned long long)block,
                    this_count));

    if (!sdp || !scsi_device_online(sdp) ||
        block + blk_rq_sectors(rq) > get_capacity(disk)) {
        SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
                        "Finishing %u sectors\n",
                        blk_rq_sectors(rq)));
        SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
                        "Retry with 0x%p\n", SCpnt));
        goto out;
    }

    if (sdp->changed) {
        /*
         * quietly refuse to do anything to a changed disc until 
         * the changed bit has been reset
         */
        /* printk("SCSI disk has been changed or is not present. Prohibiting further I/O.\n"); */
        goto out;
    }

    /*
     * Some SD card readers can't handle multi-sector accesses which touch
     * the last one or two hardware sectors.  Split accesses as needed.
     */
    threshold = get_capacity(disk) - SD_LAST_BUGGY_SECTORS *
        (sdp->sector_size / 512);

    if (unlikely(sdp->last_sector_bug && block + this_count > threshold)) {
        if (block < threshold) {
            /* Access up to the threshold but not beyond */
            this_count = threshold - block;
        } else {
            /* Access only a single hardware sector */
            this_count = sdp->sector_size / 512;
        }
    }

    SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt, "block=%llu\n",
                    (unsigned long long)block));

    /*
     * If we have a 1K hardware sectorsize, prevent access to single
     * 512 byte sectors.  In theory we could handle this - in fact
     * the scsi cdrom driver must be able to handle this because
     * we typically use 1K blocksizes, and cdroms typically have
     * 2K hardware sectorsizes.  Of course, things are simpler
     * with the cdrom, since it is read-only.  For performance
     * reasons, the filesystems should be able to handle this
     * and not force the scsi disk driver to use bounce buffers
     * for this.
     */
    if (sdp->sector_size == 1024) {
        if ((block & 1) || (blk_rq_sectors(rq) & 1)) {
            scmd_printk(KERN_ERR, SCpnt,
                    "Bad block number requested\n");
            goto out;
        } else {
            block = block >> 1;
            this_count = this_count >> 1;
        }
    }
    if (sdp->sector_size == 2048) {
        if ((block & 3) || (blk_rq_sectors(rq) & 3)) {
            scmd_printk(KERN_ERR, SCpnt,
                    "Bad block number requested\n");
            goto out;
        } else {
            block = block >> 2;
            this_count = this_count >> 2;
        }
    }
    if (sdp->sector_size == 4096) {
        if ((block & 7) || (blk_rq_sectors(rq) & 7)) {
            scmd_printk(KERN_ERR, SCpnt,
                    "Bad block number requested\n");
            goto out;
        } else {
            block = block >> 3;
            this_count = this_count >> 3;
        }
    }
    if (rq_data_dir(rq) == WRITE) {
        if (!sdp->writeable) {
            goto out;
        }
        SCpnt->cmnd[0] = WRITE_6;
        SCpnt->sc_data_direction = DMA_TO_DEVICE;

        if (blk_integrity_rq(rq))
            goto out;

    } else if (rq_data_dir(rq) == READ) {
        SCpnt->cmnd[0] = READ_6;
        SCpnt->sc_data_direction = DMA_FROM_DEVICE;
    } else {
        scmd_printk(KERN_ERR, SCpnt, "Unknown command %x\n", rq->cmd_flags);
        goto out;
    }

    SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
                    "%s %d/%u 512 byte blocks.\n",
                    (rq_data_dir(rq) == WRITE) ?
                    "writing" : "reading", this_count,
                    blk_rq_sectors(rq)));

    /* Set RDPROTECT/WRPROTECT if disk is formatted with DIF */
    host_dif = scsi_host_dif_capable(sdp->host, sdkp->protection_type);
    if (host_dif)
        protect = 1 << 5;
    else
        protect = 0;

    if (host_dif == SD_DIF_TYPE2_PROTECTION) {
        SCpnt->cmnd = mempool_alloc(ss_cdb_pool, GFP_ATOMIC);

        if (unlikely(SCpnt->cmnd == NULL)) {
            ret = BLKPREP_DEFER;
            goto out;
        }

        SCpnt->cmd_len = SD_EXT_CDB_SIZE;
        memset(SCpnt->cmnd, 0, SCpnt->cmd_len);
        SCpnt->cmnd[0] = VARIABLE_LENGTH_CMD;
        SCpnt->cmnd[7] = 0x18;
        SCpnt->cmnd[9] = (rq_data_dir(rq) == READ) ? READ_32 : WRITE_32;
        SCpnt->cmnd[10] = protect | ((rq->cmd_flags & REQ_FUA) ? 0x8 : 0);

        /* LBA */
        SCpnt->cmnd[12] = sizeof(block) > 4 ? (unsigned char) (block >> 56) & 0xff : 0;
        SCpnt->cmnd[13] = sizeof(block) > 4 ? (unsigned char) (block >> 48) & 0xff : 0;
        SCpnt->cmnd[14] = sizeof(block) > 4 ? (unsigned char) (block >> 40) & 0xff : 0;
        SCpnt->cmnd[15] = sizeof(block) > 4 ? (unsigned char) (block >> 32) & 0xff : 0;
        SCpnt->cmnd[16] = (unsigned char) (block >> 24) & 0xff;
        SCpnt->cmnd[17] = (unsigned char) (block >> 16) & 0xff;
        SCpnt->cmnd[18] = (unsigned char) (block >> 8) & 0xff;
        SCpnt->cmnd[19] = (unsigned char) block & 0xff;

        /* Expected Indirect LBA */
        SCpnt->cmnd[20] = (unsigned char) (block >> 24) & 0xff;
        SCpnt->cmnd[21] = (unsigned char) (block >> 16) & 0xff;
        SCpnt->cmnd[22] = (unsigned char) (block >> 8) & 0xff;
        SCpnt->cmnd[23] = (unsigned char) block & 0xff;

        /* Transfer length */
        SCpnt->cmnd[28] = (unsigned char) (this_count >> 24) & 0xff;
        SCpnt->cmnd[29] = (unsigned char) (this_count >> 16) & 0xff;
        SCpnt->cmnd[30] = (unsigned char) (this_count >> 8) & 0xff;
        SCpnt->cmnd[31] = (unsigned char) this_count & 0xff;
    } else if (block > 0xffffffff) {
        SCpnt->cmnd[0] += READ_16 - READ_6;
        SCpnt->cmnd[1] = protect | ((rq->cmd_flags & REQ_FUA) ? 0x8 : 0);
        SCpnt->cmnd[2] = sizeof(block) > 4 ? (unsigned char) (block >> 56) & 0xff : 0;
        SCpnt->cmnd[3] = sizeof(block) > 4 ? (unsigned char) (block >> 48) & 0xff : 0;
        SCpnt->cmnd[4] = sizeof(block) > 4 ? (unsigned char) (block >> 40) & 0xff : 0;
        SCpnt->cmnd[5] = sizeof(block) > 4 ? (unsigned char) (block >> 32) & 0xff : 0;
        SCpnt->cmnd[6] = (unsigned char) (block >> 24) & 0xff;
        SCpnt->cmnd[7] = (unsigned char) (block >> 16) & 0xff;
        SCpnt->cmnd[8] = (unsigned char) (block >> 8) & 0xff;
        SCpnt->cmnd[9] = (unsigned char) block & 0xff;
        SCpnt->cmnd[10] = (unsigned char) (this_count >> 24) & 0xff;
        SCpnt->cmnd[11] = (unsigned char) (this_count >> 16) & 0xff;
        SCpnt->cmnd[12] = (unsigned char) (this_count >> 8) & 0xff;
        SCpnt->cmnd[13] = (unsigned char) this_count & 0xff;
        SCpnt->cmnd[14] = SCpnt->cmnd[15] = 0;
    } else if ((this_count > 0xff) || (block > 0x1fffff) ||
           scsi_device_protection(SCpnt->device) ||
           SCpnt->device->use_10_for_rw) {
        if (this_count > 0xffff)
            this_count = 0xffff;

        SCpnt->cmnd[0] += READ_10 - READ_6;
        SCpnt->cmnd[1] = protect | ((rq->cmd_flags & REQ_FUA) ? 0x8 : 0);
        SCpnt->cmnd[2] = (unsigned char) (block >> 24) & 0xff;
        SCpnt->cmnd[3] = (unsigned char) (block >> 16) & 0xff;
        SCpnt->cmnd[4] = (unsigned char) (block >> 8) & 0xff;
        SCpnt->cmnd[5] = (unsigned char) block & 0xff;
        SCpnt->cmnd[6] = SCpnt->cmnd[9] = 0;
        SCpnt->cmnd[7] = (unsigned char) (this_count >> 8) & 0xff;
        SCpnt->cmnd[8] = (unsigned char) this_count & 0xff;
    } else {
        if (unlikely(rq->cmd_flags & REQ_FUA)) {
            /*
             * This happens only if this drive failed
             * 10byte rw command with ILLEGAL_REQUEST
             * during operation and thus turned off
             * use_10_for_rw.
             */
            scmd_printk(KERN_ERR, SCpnt,
                    "FUA write on READ/WRITE(6) drive\n");
            goto out;
        }

        SCpnt->cmnd[1] |= (unsigned char) ((block >> 16) & 0x1f);
        SCpnt->cmnd[2] = (unsigned char) ((block >> 8) & 0xff);
        SCpnt->cmnd[3] = (unsigned char) block & 0xff;
        SCpnt->cmnd[4] = (unsigned char) this_count;
        SCpnt->cmnd[5] = 0;
    }
    SCpnt->sdb.length = this_count * sdp->sector_size;

    /* If DIF or DIX is enabled, tell HBA how to handle request */
    if (host_dif || scsi_prot_sg_count(SCpnt))
        ss_prot_op(SCpnt, host_dif);

    /*
     * We shouldn't disconnect in the middle of a sector, so with a dumb
     * host adapter, it's safe to assume that we can at least transfer
     * this many bytes between each connect / disconnect.
     */
    SCpnt->transfersize = sdp->sector_size;
    SCpnt->underflow = this_count << 9;
    SCpnt->allowed = SD_MAX_RETRIES;

    /*
     * This indicates that the command is ready from our end to be
     * queued.
     */
    ret = BLKPREP_OK;
 out:
    return scsi_prep_return(q, rq, ret);
}

static const struct block_device_operations ss_fops = {
        .owner      = THIS_MODULE,
        .open       = ss_open,
        .release    = ss_release,
        //.locked_ioctl =  ss_ioctl,
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
    struct scsi_device * sdp;
    struct scsi_disk * sdkp;

    for (i = 0; i < sizeof(ssds) / sizeof(ssds[0]) && i < SSD_MAJOR; i++) {
        bdev = lookup_bdev(ssds[i]);
        if (!IS_ERR_OR_NULL(bdev)) {
            SDEBUG("%s as ssd wi soft-ftl found!\n", ssds[i]);
            sdk = kzalloc(sizeof(struct ssd_disk), GFP_KERNEL);
            INIT_LIST_HEAD(&sdk->mflush_list);
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
            else {
                gd->queue = oldgd->queue;
                sdkp = scsi_disk(oldgd);
                sdk->protection_type = sdkp->protection_type;
                sdk->provisioning_mode = sdkp->provisioning_mode;
                sdk->device = sdkp->device;
            }

            if (!gd->queue)
                printk(KERN_ERR "ss: cannot get request queue !\n");
            else {
                sdk->old_make_request_fn = gd->queue->make_request_fn;
                sdk->old_prep_fn = gd->queue->prep_rq_fn;
                sdk->old_request_fn = gd->queue->request_fn;
                gd->queue->make_request_fn = ss_make_request_fn;

                mrf = sdk->old_make_request_fn;
                if (mrf != blk_queue_bio)
                    SDEBUG("make_request_fn is not blk_queue_bio!\n");

                gd->queue->prep_rq_fn = ss_prep_rq_fn;
                sdp = gd->queue->queuedata;
                if (!sdp->host) {
                    printk(KERN_ERR "ss: host in queue is null!\n");
                    break;
                }
            }

            sdk->bs = bioset_create(MEMPOOL_SIZE, 0);
            sdk->io_pool = mempool_create_slab_pool(MEMPOOL_SIZE, ss_io_cache);

            gd->fops = &ss_fops;
            gd->major = ssd_major[i];
            gd->first_minor = 0;
            gd->minors = SSD_MINORS;
            gd->private_data = &sdk->list;
            set_capacity(gd, oldgd->part0.nr_sects);
            sdk->gd = gd;
            sdk->capacity = oldgd->part0.nr_sects;

            add_disk(gd);
            SDEBUG("disk %s added successfully!\n", gd->disk_name);
            init_mapping_dir(gd);

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
        exit_mapping_dir(sdk->gd);
        sdk->gd->queue->make_request_fn = sdk->old_make_request_fn;
        sdk->gd->queue->prep_rq_fn = sdk->old_prep_fn;
        bioset_free(sdk->bs);
        mempool_destroy(sdk->io_pool);
        del_gendisk(sdk->gd);
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

    ss_cdb_cache = kmem_cache_create("ss_cdb_cache", CDB_SIZE, 0, 0, NULL);
    ss_io_cache = kmem_cache_create("ss_io_cache", sizeof(struct ss_io), 0, 0, NULL);

    if (!ss_cdb_cache) {
        printk(KERN_ERR "ss: can't init cdb cache\n");
        goto err_out;
    }

    if (!ss_io_cache) {
        printk(KERN_ERR "ss: can't init io cache\n");
        goto err_cache;
    }

    ss_cdb_pool = mempool_create_slab_pool(MEMPOOL_SIZE, ss_cdb_cache);

    if (!ss_cdb_pool) {
        printk(KERN_ERR "ss: can't init cdb pool\n");
        goto err_io;
    }

    if (!find_and_init_disk()) {
        err = -ENODEV;
        goto err_pool;
    }

    return 0;
err_pool:
    mempool_destroy(ss_cdb_pool);
err_io:
    kmem_cache_destroy(ss_io_cache);
err_cache:
    kmem_cache_destroy(ss_cdb_cache);
err_out:
    for (i = 0; i < SSD_MAJOR; i++)
        unregister_blkdev(ssd_major[i], "ss");
    return err;
}

static void __exit exit_ssd(void)
{
    int i;

    mempool_destroy(ss_cdb_pool);
    kmem_cache_destroy(ss_cdb_cache);
    destroy_disk();
    kmem_cache_destroy(ss_io_cache);
    for (i = 0; i < SSD_MAJOR; i ++)
        unregister_blkdev(ssd_major[i], "ss");
}

module_init(init_ssd);
module_exit(exit_ssd);
