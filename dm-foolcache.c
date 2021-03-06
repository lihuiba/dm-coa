/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/dm-kcopyd.h>
#include <linux/dm-io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fiemap.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/delay.h>

//#include <arch/x86/include/asm/atomic.h>
#include "ioctl.h"

#define DM_MSG_PREFIX "foolcache"

// DECLARE_DM_KCOPYD_THROTTLE_WITH_MODULE_PARM(fc_cor,
// 		"A percentage of time allocated for Copy-On-Read");

const static char SIGNATURE[]="FOOLCACHE";
struct header {
	char signature[sizeof(SIGNATURE)];
	unsigned int block_size;
};

struct foolcache_c {
	struct dm_dev* cache;
	struct dm_dev* origin;
	struct dm_io_client* io_client;
	unsigned int bypassing;
	sector_t sectors, last_caching_sector;
	unsigned long size, blocks;
	unsigned int block_size;		// block (chunk) size, in sector
	unsigned int block_shift;
	unsigned int block_mask;
	unsigned long* bitmap;
	unsigned long* copying;
	unsigned long bitmap_modified;
	unsigned long bitmap_last_sync;
	struct header* header;
	unsigned int bitmap_sectors;
	struct completion copied;
	struct dm_kcopyd_client* kcopyd_client;
	atomic64_t cached_blocks, hits, misses, ts;
	atomic_t kcopyd_jobs;
};

struct job_kcopyd {
	struct bio* bio;
	struct foolcache_c* fcc;
	unsigned long copying_block, end_block;
	struct dm_io_region origin, cache;
};

static struct proc_dir_entry* fcdir_proc;
static inline void proc_new_entry(struct foolcache_c* fcc);
static inline void proc_remove_entry(struct foolcache_c* fcc);

static inline unsigned long sector2block(struct foolcache_c* fcc, sector_t sector)
{
	return sector >> fcc->block_shift;
}

static inline sector_t block2sector(struct foolcache_c* fcc, unsigned long block)
{
	return block << fcc->block_shift;
}

inline unsigned char cout_bits_uchar(unsigned char x)
{
	return (x&1) + ((x>>1)&1) + ((x>>2)&1) + ((x>>3)&1)
		 + ((x>>4)&1) + ((x>>5)&1) + ((x>>6)&1) + ((x>>7));
}

static unsigned long count_bits(void* buf, unsigned long size)
{
	unsigned long i, r;
	unsigned char table[256];
	unsigned char* _buf = (unsigned char*)buf;
	for (i=0; i<256; ++i)
	{
		table[i] = cout_bits_uchar(i);
	}
	for (i=r=0; i<size/sizeof(_buf[0]); ++i)
	{
		r+=table[_buf[i]];
	}
	for (i*=sizeof(_buf[0]); i<size; ++i)
	{
		r+=test_bit(i, buf);
	}
	return r;
}

// static unsigned long count_bits_offset(void* buf, 
// 	unsigned long offset, unsigned long size)
// {
// 	unsigned long r;
// 	unsigned char i=offset%8;
// 	buf = (char*)buf + offset/8;
// 	if (i==0) return count_bits(buf, size);

// 	r = 0;
// 	size -= (8-i);
// 	for (; i<8; ++i)
// 	{
// 		r += test_bit(i, buf);
// 	}
// 	buf = (char*)buf +1;
// 	return r + count_bits(buf, size);
// }

static void write_bitmap_callback(unsigned long error, void *context)
{

}

static int write_bitmap(struct foolcache_c* fcc, io_notify_fn callback)
{
	int r;
	struct dm_io_region region;
	struct dm_io_request io_req;
	
	if (!fcc->bitmap_modified)
	{
		return 0;
	}

	region.bdev = fcc->cache->bdev;
	region.sector = fcc->sectors - (fcc->bitmap_sectors + 1);
	region.count = fcc->bitmap_sectors;
	io_req.bi_rw = WRITE;
	io_req.mem.type = DM_IO_VMA;
	io_req.mem.ptr.vma = fcc->bitmap;
	io_req.notify.fn = callback;
	io_req.notify.context = fcc;
	io_req.client = fcc->io_client;

	r = dm_io(&io_req, 1, &region, NULL);
	if (r!=0) return r;
	fcc->bitmap_modified = 0;
	return 0;
}

static int write_header(struct foolcache_c* fcc)
{
	int r;
	struct dm_io_region region = {
		.bdev = fcc->cache->bdev,
		.sector = fcc->sectors - 1,
		.count = 1,
	};
	struct dm_io_request io_req = {
		.bi_rw = WRITE,
		.mem.type = DM_IO_VMA,
		.mem.ptr.vma = fcc->header,
		// .notify.fn = ,
		// .notify.context = ,
		.client = fcc->io_client,
	};

	memcpy(fcc->header->signature, SIGNATURE, sizeof(SIGNATURE));
	fcc->header->block_size = fcc->block_size;
	r = dm_io(&io_req, 1, &region, NULL);
	return r;
}

static inline int write_ender(struct foolcache_c* fcc)
{
	return write_bitmap(fcc, NULL) || write_header(fcc);
}

static int read_ender(struct foolcache_c* fcc)
{
	int r;
	struct dm_io_region region = {
		.bdev = fcc->cache->bdev,
		.sector = fcc->sectors - 1,
		.count = 1,
	};
	struct dm_io_request io_req = {
		.bi_rw = READ,
		.mem.type = DM_IO_VMA,
		.mem.ptr.vma = fcc->header,
		// .notify.fn = ,
		// .notify.context = ,
		.client = fcc->io_client,
	};
	r=dm_io(&io_req, 1, &region, NULL);
	if (r!=0) return r;

	r=strncmp(fcc->header->signature, SIGNATURE, sizeof(SIGNATURE)-1);
	if (r!=0) return r;
	if (fcc->header->block_size != fcc->block_size) return -EINVAL;

	io_req.mem.ptr.addr = fcc->bitmap;
	region.sector = fcc->last_caching_sector + 1;
	region.count = fcc->bitmap_sectors;
	r = dm_io(&io_req, 1, &region, NULL);
	if (r==0) fcc->bitmap_modified = 0;
	return r;
}

static void do_read_async_callback(unsigned long error, void* context)
{
	struct job_kcopyd* job = context;
//	struct foolcache_c* fcc = job->fcc;
	struct bio* bio = job->bio;
	
	kfree(job);
	bio_endio(bio, unlikely(error) ? -EIO : 0);
}

static void do_read_async(struct job_kcopyd* job, struct dm_dev* target)
{
	struct foolcache_c* fcc = job->fcc;
	struct bio* bio = job->bio;
	struct dm_io_region region;
	struct dm_io_request io_req;
	
	region.bdev = target->bdev;
	region.sector = bio->bi_sector;
	region.count = bio->bi_size/512;
	io_req.bi_rw = bio->bi_rw;
	io_req.mem.type = DM_IO_BVEC;
	io_req.mem.ptr.bvec = bio->bi_io_vec + bio->bi_idx;
	io_req.notify.fn = do_read_async_callback;
	io_req.notify.context = job;
	io_req.client = fcc->io_client;

	dm_io(&io_req, 1, &region, NULL);
}

static inline unsigned long find_next_copying_block(
	struct foolcache_c* fcc, unsigned long start, unsigned long end)
{
	for (;start<=end; ++start)
	{
		if (likely(test_bit(start, fcc->bitmap)))
		{
			atomic64_inc(&fcc->hits);
		}
		else
		{
			atomic64_inc(&fcc->misses);
			return start;
		}
	}
	return -1;
}

static int ensure_block_async(struct job_kcopyd* job);
static void ensure_block_async_callback(int read_err, 
	unsigned long write_err, void *context)
{
	struct job_kcopyd* job = context;
	struct foolcache_c* fcc = job->fcc;
	unsigned long block = job->copying_block;

	if (unlikely(read_err || write_err))
	{
		fcc->bypassing = 1;
	}
	else
	{
		set_bit(block, fcc->bitmap);
		fcc->bitmap_modified = 1;
	}
	smp_mb__before_clear_bit();
	clear_bit(block, fcc->copying);
	smp_mb__after_clear_bit();
	complete_all(&fcc->copied);

	if (fcc->bypassing)
	{
		do_read_async(job, fcc->origin);
		return;
	}

	block = find_next_copying_block(fcc, block, job->end_block);
	if (block >= job->end_block)
	{
		do_read_async(job, fcc->cache);
		return;
	}	
	job->copying_block = block;
	ensure_block_async(job);
}

static void ensure_block_async_callback_dec(int read_err, 
	unsigned long write_err, void *context)
{
	struct job_kcopyd* job = context;
	struct foolcache_c* fcc = job->fcc;
	atomic_dec(&fcc->kcopyd_jobs);
	ensure_block_async_callback(read_err, write_err, context);
}

static int ensure_block_async(struct job_kcopyd* job)
{
	struct foolcache_c* fcc = job->fcc;
	unsigned long block = job->copying_block;
	if (fcc->bypassing)
	{
		do_read_async(job, fcc->origin);
		return 0;
	}

	// before copying
	if (test_and_set_bit(block, fcc->copying))
	{	// the block is being copied by another thread, let's just wait
		atomic64_inc(&fcc->hits);		// it's really a hit, 
		atomic64_dec(&fcc->misses);		// instead of a miss
wait:
		//printk("dm-foolcache: pre-wait\n");
		wait_for_completion_timeout(&fcc->copied, 1*HZ);
		atomic64_set(&fcc->ts, get_jiffies_64());
		//printk("dm-foolcache: post-wait\n");
		if (fcc->bypassing)
		{
			do_read_async(job, fcc->origin);
			return 0;
		}
		if (test_bit(block, fcc->copying))
		{
			goto wait;
		}
		ensure_block_async_callback(0, 0, job);
		return 0;
	}

	if (test_bit(block, fcc->bitmap))
	{
		atomic64_inc(&fcc->hits);		// it's really a hit, 
		atomic64_dec(&fcc->misses);		// instead of a miss
		smp_mb__before_clear_bit();
		clear_bit(block, fcc->copying);
		smp_mb__before_clear_bit();
		ensure_block_async_callback(0, 0, job);
		return 0;
	}

	while(atomic_read(&fcc->kcopyd_jobs)>100)
	{
		msleep(100);
	}
	// do copying
	atomic_inc(&fcc->kcopyd_jobs);
	job->origin.bdev = fcc->origin->bdev;
	job->cache.bdev = fcc->cache->bdev;
	job->origin.sector = job->cache.sector = block2sector(fcc, block);
	job->origin.count = job->cache.count = fcc->block_size;	
	dm_kcopyd_copy(fcc->kcopyd_client, &job->origin, 1, &job->cache, 
		0, ensure_block_async_callback_dec, job);
	atomic64_inc(&fcc->cached_blocks);
	return 0;
}

static int map_async(struct foolcache_c* fcc, struct bio* bio)
{
	sector_t last_sector;
	u64 now = get_jiffies_64();
	if (fcc->bitmap_last_sync+HZ*16 < now || now < fcc->bitmap_last_sync)
	{
		write_bitmap(fcc, write_bitmap_callback);
	}

	if (bio_data_dir(bio) == WRITE)
	{
		return -EIO;
	}

	last_sector = bio->bi_sector + bio->bi_size/512 - 1;
	if (unlikely(fcc->bypassing || last_sector > fcc->last_caching_sector))
	{
		unsigned long blocks = sector2block(fcc, last_sector) - sector2block(fcc, bio->bi_sector) + 1;
		atomic64_add(blocks, &fcc->misses);
		bio->bi_bdev = fcc->origin->bdev;
		return DM_MAPIO_REMAPPED;
	}
	else
	{	// preparing the cache, followed by remapping
		struct job_kcopyd* job;
		unsigned long end_block = sector2block(fcc, last_sector);
		unsigned long start_block = sector2block(fcc, bio->bi_sector);
		start_block = find_next_copying_block(fcc, start_block, end_block);
		//printk("dm-foolcache: reading block %lu to %lu\n", start_block, end_block);

		if (start_block == -1)
		{	//all blocks are hit
			bio->bi_bdev = fcc->cache->bdev;
			return DM_MAPIO_REMAPPED;
		}

		job = kmalloc(sizeof(*job), __KERNEL__);
		job->copying_block = start_block;
		job->end_block = end_block;
		job->bio = bio;
		job->fcc = fcc;
		ensure_block_async(job);
		return DM_MAPIO_SUBMITTED;
	}
}

static inline bool isorder2(unsigned int x)
{
	return (x & (x-1)) == 0;
}

static inline unsigned long DIV(unsigned long a, unsigned long b)
{
	return a/b + (a%b > 0);
}

/*
 * Construct a foolcache mapping
 *      origin cache block_size [create]
 */
static int foolcache_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct foolcache_c *fcc;
	unsigned int bs, bitmap_size, r;

	if (argc<2) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	fcc = vzalloc(sizeof(*fcc));
	if (fcc == NULL) {
		ti->error = "dm-foolcache: Cannot allocate foolcache context";
		return -ENOMEM;
	}

	if (dm_get_device(ti, argv[0], FMODE_READ, &fcc->origin)) {
		ti->error = "dm-foolcache: Device lookup failed";
		goto bad1;
	}

	if (dm_get_device(ti, argv[1], FMODE_READ|FMODE_WRITE, &fcc->cache)) {
		ti->error = "dm-foolcache: Device lookup failed";
		goto bad2;
	}

	fcc->size = i_size_read(fcc->origin->bdev->bd_inode);
	fcc->sectors = (fcc->size >> SECTOR_SHIFT);
	if (fcc->size != i_size_read(fcc->cache->bdev->bd_inode))
	{
		ti->error = "dm-foolcache: Device sub-device size mismatch";
		goto bad3;
	}

	if (sscanf(argv[2], "%u", &bs)!=1 || bs<4 || bs>1024*1024 || !isorder2(bs)) {
		ti->error = "dm-foolcache: Invalid block size";
		goto bad3;
	}
	printk("dm-foolcache: bs %uKB\n", bs);

	bs*=(1024/512); // KB to sector
	fcc->blocks = DIV(fcc->sectors, bs);
	fcc->block_size = bs;
	fcc->block_shift = ffs(bs)-1;
	fcc->block_mask = ~(bs-1);
	printk("dm-foolcache: bshift %u, bmask %u\n", fcc->block_shift, fcc->block_mask);
	fcc->bitmap_sectors = DIV(fcc->blocks, 8*512); 	// sizeof bitmap, in sector
	fcc->last_caching_sector = fcc->sectors - 1 - 1 - max(fcc->bitmap_sectors, bs);
	bitmap_size = fcc->bitmap_sectors*512;
	fcc->bitmap = vzalloc(bitmap_size);
	fcc->copying = vzalloc(bitmap_size);
	fcc->header = vzalloc(512);
	if (fcc->bitmap==NULL || fcc->copying==NULL || fcc->header==NULL)
	{
		ti->error = "dm-foolcache: Cannot allocate bitmaps";
		goto bad4;
	}

	fcc->io_client = dm_io_client_create();
	if (IS_ERR(fcc->io_client)) 
	{
		ti->error = "dm-foolcache: dm_io_client_create() error";
		goto bad4;
	}

	fcc->kcopyd_client = dm_kcopyd_client_create();
	// fcc->kcopyd_client = dm_kcopyd_client_create(&dm_kcopyd_throttle);
	if (IS_ERR(fcc->kcopyd_client))
	{
		ti->error = "dm-foolcache: dm_kcopyd_client_create() error";
		goto bad5;
	}

	atomic_set(&fcc->kcopyd_jobs, 0);
	atomic64_set(&fcc->hits, 0);
	atomic64_set(&fcc->misses, 0);
	memset(fcc->copying, 0, bitmap_size);
	if (argc>=4 && strcmp(argv[3], "create")==0)
	{	// create new cache
		atomic64_set(&fcc->cached_blocks, 0);
		memset(fcc->bitmap, 0, bitmap_size);
		fcc->bitmap_modified = 1;
		r = write_ender(fcc);
		if (r!=0)
		{
			ti->error = "dm-foolcache: ender write error";
			goto bad6;
		}
	}
	else
	{	// open existing cache
		r = read_ender(fcc);
		atomic64_set(&fcc->cached_blocks, 
			count_bits(fcc->bitmap, fcc->sectors));
		if (r!=0)
		{
			ti->error = "dm-foolcache: ender read error";
			goto bad6;
		}
	}
	fcc->bitmap_last_sync = jiffies;

	init_completion(&fcc->copied);
	proc_new_entry(fcc);

	ti->num_flush_requests = 1;
	ti->num_discard_requests = 1;
	ti->private = fcc;
	printk("dm-foolcache: ctor succeeed\n");
	return 0;

bad6:
	dm_kcopyd_client_destroy(fcc->kcopyd_client);
bad5:
	dm_io_client_destroy(fcc->io_client);
bad4:
	if (fcc->bitmap) vfree(fcc->bitmap);
	if (fcc->copying) vfree(fcc->copying);
	if (fcc->header) vfree(fcc->header);
bad3:
	dm_put_device(ti, fcc->cache);
bad2:
	dm_put_device(ti, fcc->origin);
bad1:
	vfree(fcc);
	printk("dm-foolcache: ctor failed\n");
	return -EINVAL;
}

static void foolcache_dtr(struct dm_target *ti)
{
	struct foolcache_c *fcc = ti->private;
	write_bitmap(fcc, NULL);
	vfree(fcc->bitmap);
	vfree(fcc->copying);
	vfree(fcc->header);
	proc_remove_entry(fcc);
	dm_kcopyd_client_destroy(fcc->kcopyd_client);
	dm_io_client_destroy(fcc->io_client);
	dm_put_device(ti, fcc->origin);
	dm_put_device(ti, fcc->cache);
	vfree(fcc);
}

static void foolcache_status(struct dm_target *ti, status_type_t type,
		char *result, unsigned int maxlen)
{
	struct foolcache_c *fcc = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		snprintf(result, maxlen, "%s %s %u", fcc->origin->name, 
			fcc->cache->name, fcc->block_size*512/1024);
		break;
	}
}

static inline int foolcache_fibmap(struct foolcache_c *fcc, int __user *p)
{
	int res, block;
	res = get_user(block, p);
	if (res) return res;
	if (block >= fcc->blocks) return -1;
	block = test_bit(block, fcc->bitmap)!=0;
	return put_user(block, p);
}

static inline int foolcache_figetbsz(struct foolcache_c *fcc, int __user *p)
{
	return put_user(fcc->block_size*512, p);
}

static int fiemap_check_ranges(struct foolcache_c *fcc,
			       u64 start, u64 len, u64 *new_len)
{
	u64 maxbytes = (u64) fcc->size;

	*new_len = len;

	if (len == 0)
		return -EINVAL;

	if (start > maxbytes)
		return -EFBIG;

	/*
	 * Shrink request scope to what the fs can actually handle.
	 */
	if (len > maxbytes || (maxbytes - len) < start)
		*new_len = maxbytes - start;

	return 0;
}

// static int ext4_fill_fiemap_extents(struct inode *inode,
// 				    ext4_lblk_t block, ext4_lblk_t num,
// 				    struct fiemap_extent_info *fieinfo)
// {

// }

int fiemap_fill_next_extent(struct fiemap_extent_info *fieinfo, u64 logical,
			    u64 phys, u64 len, u32 flags);


int foolcache_do_fiemap(struct foolcache_c *fcc, struct fiemap_extent_info *fieinfo,
	__u64 start, __u64 len)
{
	unsigned long i, c, shift;
	shift = fcc->block_shift + 9;
	for (i=c=0; i<fcc->blocks; ++i) 
	{
		if (test_bit(i, fcc->bitmap))
		{
			c++;
			fiemap_fill_next_extent(fieinfo, i<<shift, i<<shift, 1<<shift, 
				(c==atomic64_read(&fcc->cached_blocks)) ? FIEMAP_EXTENT_LAST : 0);
		}
	}
	return 0;
}

#define FIEMAP_MAX_EXTENTS	(UINT_MAX / sizeof(struct fiemap_extent))
static int foolcache_fiemap(struct foolcache_c *fcc, int __user *p)
{
	struct fiemap fiemap;
	struct fiemap __user *ufiemap = (struct fiemap __user *) p;
	struct fiemap_extent_info fieinfo = { 0, };
	u64 len;
	int error;

	if (copy_from_user(&fiemap, ufiemap, sizeof(fiemap)))
		return -EFAULT;

	if (fiemap.fm_extent_count > FIEMAP_MAX_EXTENTS)
		return -EINVAL;

	error = fiemap_check_ranges(fcc, fiemap.fm_start, fiemap.fm_length,
				    &len);
	if (error)
		return error;

	fieinfo.fi_flags = fiemap.fm_flags;
	fieinfo.fi_extents_max = fiemap.fm_extent_count;
	fieinfo.fi_extents_start = ufiemap->fm_extents;

	if (fiemap.fm_extent_count != 0 &&
	    !access_ok(VERIFY_WRITE, fieinfo.fi_extents_start,
		       fieinfo.fi_extents_max * sizeof(struct fiemap_extent)))
		return -EFAULT;

	error = foolcache_do_fiemap(fcc, &fieinfo, fiemap.fm_start, len);
	fiemap.fm_flags = fieinfo.fi_flags;
	fiemap.fm_mapped_extents = fieinfo.fi_extents_mapped;

	if (copy_to_user(ufiemap, &fiemap, sizeof(fiemap)))
		error = -EFAULT;

	return error;
}

static int foolcache_ioctl(struct dm_target *ti, unsigned int cmd,
			unsigned long arg)
{
	struct foolcache_c *fcc = ti->private;
	int __user *p = (int __user *)arg;
	//printk("dm-foolcache: ioctl cmd=0x%x\n", cmd);

	switch (cmd)
	{
	case FOOLCACHE_GETBSZ:
		return foolcache_figetbsz(fcc, p);

	case FOOLCACHE_FIBMAP:
		return foolcache_fibmap(fcc, p);

	case FOOLCACHE_FIEMAP:
		return foolcache_fiemap(fcc, p);

	default:
		return -ENOTTY;
	}
}
/*
static int foolcache_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			struct bio_vec *biovec, int max_size)
{
	struct foolcache_c *fcc = ti->private;
	struct request_queue *q = bdev_get_queue(fcc->dev->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = fcc->dev->bdev;
	bvm->bi_sector = linear_map_sector(ti, bvm->bi_sector);

	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}
*/
static int foolcache_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	int r;
	struct foolcache_c *fcc = ti->private;
	r = fn(ti, fcc->origin, 0, fcc->sectors, data);
	if (r) return r;
	r = fn(ti, fcc->cache, 0, fcc->sectors, data);
	return r;
}

static int foolcache_map(struct dm_target *ti, struct bio *bio,
		      union map_info *map_context)
{
	struct foolcache_c *fcc = ti->private;
	return map_async(fcc, bio);
}

static struct target_type foolcache_target = {
	.name   = "foolcache",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = foolcache_ctr,
	.dtr    = foolcache_dtr,
	.map    = foolcache_map,
	.status = foolcache_status,
	.ioctl  = foolcache_ioctl,
//	.merge  = foolcache_merge,
	.iterate_devices = foolcache_iterate_devices,
};

static inline void print_percent(struct seq_file *m, const char* title, 
	unsigned long a, unsigned long b)
{
	unsigned int x = b ? a*100/b : 0;
	unsigned int y = b ? (a*1000/b)%10 : 0;
	seq_printf(m, "%s: %lu/%lu (%u.%u%%)\n", title, a, b, x, y);
}

static int foolcache_proc_show(struct seq_file* m, void* v)
{
	unsigned long hits;
	struct foolcache_c *fcc = m->private;
	// seq_puts(m, "Foolcache\n");
	seq_printf(m, "Bypassing: %u\n", fcc->bypassing);
	seq_printf(m, "Origin: %s\n", fcc->origin->name);
	seq_printf(m, "Cache: %s\n", fcc->cache->name);
	seq_printf(m, "BlockSize: %uKB\n", fcc->block_size*512/1024);
	seq_printf(m, "Last Timedout at: %lu\n", atomic64_read(&fcc->ts));
	seq_printf(m, "Kcopyd jobs: %u\n", atomic_read(&fcc->kcopyd_jobs));
	hits = atomic64_read(&fcc->hits);
	print_percent(m, "Hit", hits, hits + atomic64_read(&fcc->misses));
	print_percent(m, "Fullfillment", atomic64_read(&fcc->cached_blocks), fcc->blocks);
	return 0;
}

static int foolcache_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, foolcache_proc_show, PDE(inode)->data);
}

static const struct file_operations foolcache_proc_fops = {
	.open		= foolcache_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static inline void proc_new_entry(struct foolcache_c* fcc)
{
// proc_create_data(const char *name, umode_t mode,
// 					struct proc_dir_entry *parent,
// 					const struct file_operations *proc_fops,
// 					void *data)
	proc_create_data(fcc->origin->name, 
		S_IRUGO, fcdir_proc, &foolcache_proc_fops, fcc);
}

static inline void proc_remove_entry(struct foolcache_c* fcc)
{
	remove_proc_entry(fcc->origin->name, fcdir_proc);
}

int __init dm_foolcache_init(void)
{
	int r;
	r = dm_register_target(&foolcache_target);
	if (r < 0)
	{
		DMERR("register failed %d", r);
	}

	fcdir_proc = proc_mkdir("foolcache", NULL);

	return r;
}

void dm_foolcache_exit(void)
{
	dm_unregister_target(&foolcache_target);
	remove_proc_entry("foolcache", NULL);
}

/* Module hooks */
module_init(dm_foolcache_init);
module_exit(dm_foolcache_exit);

MODULE_DESCRIPTION(DM_NAME " foolcache target");
MODULE_AUTHOR("Huiba Li <lihuiba@gmail.com>");
MODULE_LICENSE("GPL");

