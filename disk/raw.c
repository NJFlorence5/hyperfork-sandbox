#include "kvm/disk-image.h"

#include <linux/err.h>

#ifdef CONFIG_HAS_AIO
#include <libaio.h>
#endif

ssize_t raw_image__read(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;

#ifdef CONFIG_HAS_AIO
	struct iocb iocb;

	return aio_preadv(disk->ctx, &iocb, disk->fd, iov, iovcount, offset,
				disk->evt, param);
#else
	return preadv_in_full(disk->fd, iov, iovcount, offset);
#endif
}

ssize_t raw_image__write(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;

#ifdef CONFIG_HAS_AIO
	struct iocb iocb;

	return aio_pwritev(disk->ctx, &iocb, disk->fd, iov, iovcount, offset,
				disk->evt, param);
#else
	return pwritev_in_full(disk->fd, iov, iovcount, offset);
#endif
}

ssize_t raw_image__read_mmap(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	ssize_t total = 0;

	while (iovcount--) {
		memcpy(iov->iov_base, disk->priv + offset, iov->iov_len);

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		offset	+= iov->iov_len;
		total	+= iov->iov_len;
		iov++;
	}

	return total;
}

ssize_t raw_image__write_mmap(struct disk_image *disk, u64 sector, const struct iovec *iov,
				int iovcount, void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	ssize_t total = 0;

	while (iovcount--) {
		memcpy(disk->priv + offset, iov->iov_base, iov->iov_len);

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		offset	+= iov->iov_len;
		total	+= iov->iov_len;
		iov++;
	}

	return total;
}

int raw_image__close(struct disk_image *disk)
{
	int ret = 0;

	if (disk->priv != MAP_FAILED)
		ret = munmap(disk->priv, disk->size);

	close(disk->evt);

#ifdef CONFIG_HAS_VIRTIO
	io_destroy(disk->ctx);
#endif

	return ret;
}

int raw_image__post_copy(struct disk_image *disk, struct kvm *kvm)
{
	char proc_fd_path[PATH_MAX];
	char orig_path[PATH_MAX];
	char new_path[PATH_MAX];
	int dst_fd;

	if (!disk->clone)
		return 0;

	fsync(disk->fd);

	snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", disk->fd);
	ssize_t len = readlink(proc_fd_path, orig_path, sizeof(orig_path) - 1);
	if (len > 0) {
		orig_path[len] = '\0';
		snprintf(new_path, sizeof(new_path), "%s_child_%d.img", orig_path, getpid());
	} else {
		snprintf(new_path, sizeof(new_path), "clone_child_%d.img", getpid());
	}

	dst_fd = open(new_path, O_RDWR | O_CREAT, 0644);
	if (dst_fd >= 0) {
		if (ioctl(dst_fd, FICLONE, disk->fd) < 0) {
			pr_warning("FICLONE failed (errno=%d), falling back to full copy. This may take a while.", errno);
			lseek(disk->fd, 0, SEEK_SET);
			char buf[65536];
			ssize_t n;
			while ((n = read(disk->fd, buf, sizeof(buf))) > 0) {
				write_in_full(dst_fd, buf, n);
			}
		}
		close(disk->fd);
		disk->fd = dst_fd;
	}

	return 0;
}

/*
 * multiple buffer based disk image operations
 */
static struct disk_image_operations raw_image_regular_ops = {
	.read	= raw_image__read,
	.write	= raw_image__write,
	.post_copy = raw_image__post_copy,
};

struct disk_image_operations ro_ops = {
	.read	= raw_image__read_mmap,
	.write	= raw_image__write_mmap,
	.close	= raw_image__close,
};

struct disk_image_operations ro_ops_nowrite = {
	.read	= raw_image__read,
};

struct disk_image *raw_image__probe(int fd, struct stat *st, bool readonly)
{
	struct disk_image *disk;

	if (readonly) {
		/*
		 * Use mmap's MAP_PRIVATE to implement non-persistent write
		 * FIXME: This does not work on 32-bit host.
		 */
		struct disk_image *disk;

		disk = disk_image__new(fd, st->st_size, &ro_ops, DISK_IMAGE_MMAP);
		if (IS_ERR_OR_NULL(disk)) {
			disk = disk_image__new(fd, st->st_size, &ro_ops_nowrite, DISK_IMAGE_REGULAR);
#ifdef CONFIG_HAS_AIO
			if (!IS_ERR_OR_NULL(disk))
				disk->async = 1;
#endif
		}

		return disk;
	} else {
		/*
		 * Use read/write instead of mmap
		 */
		disk = disk_image__new(fd, st->st_size, &raw_image_regular_ops, DISK_IMAGE_REGULAR);
#ifdef CONFIG_HAS_AIO
		if (!IS_ERR_OR_NULL(disk))
			disk->async = 1;
#endif
		return disk;
	}
}
