#include "kvm/uip.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/virtio_net.h>

struct uip_buf *uip_buf_get_used(struct uip_info *info)
{
	struct uip_buf *buf;
	bool found = false;

	mutex_lock(&info->buf_lock);

	while (!(info->buf_used_nr > 0))
		pthread_cond_wait(&info->buf_used_cond, &info->buf_lock.mutex);

	list_for_each_entry(buf, &info->buf_head, list) {
		if (buf->status == UIP_BUF_STATUS_USED) {
			/*
			 * Set status to INUSE immediately to prevent
			 * someone from using this buf until we free it
			 */
			buf->status = UIP_BUF_STATUS_INUSE;
			info->buf_used_nr--;
			found = true;
			break;
		}
	}

	mutex_unlock(&info->buf_lock);

	return found ? buf : NULL;
}

struct uip_buf *uip_buf_get_free(struct uip_info *info)
{
	struct uip_buf *buf;
	bool found = false;

	mutex_lock(&info->buf_lock);

	while (!(info->buf_free_nr > 0))
		pthread_cond_wait(&info->buf_free_cond, &info->buf_lock.mutex);

	list_for_each_entry(buf, &info->buf_head, list) {
		if (buf->status == UIP_BUF_STATUS_FREE) {
			/*
			 * Set status to INUSE immediately to prevent
			 * someone from using this buf until we free it
			 */
			buf->status = UIP_BUF_STATUS_INUSE;
			info->buf_free_nr--;
			found = true;
			break;
		}
	}

	mutex_unlock(&info->buf_lock);

	return found ? buf : NULL;
}

struct uip_buf *uip_buf_set_used(struct uip_info *info, struct uip_buf *buf)
{
	mutex_lock(&info->buf_lock);

	buf->status = UIP_BUF_STATUS_USED;
	info->buf_used_nr++;
	pthread_cond_signal(&info->buf_used_cond);

	mutex_unlock(&info->buf_lock);

	return buf;
}

struct uip_buf *uip_buf_set_free(struct uip_info *info, struct uip_buf *buf)
{
	mutex_lock(&info->buf_lock);

	buf->status = UIP_BUF_STATUS_FREE;
	info->buf_free_nr++;
	pthread_cond_signal(&info->buf_free_cond);

	mutex_unlock(&info->buf_lock);

	return buf;
}

struct uip_buf *uip_buf_clone(struct uip_tx_arg *arg)
{
	struct uip_buf *buf;
	struct uip_eth *eth2;
	struct uip_info *info;

	info = arg->info;

	/*
	 * Get buffer from device to guest
	 */
	buf = uip_buf_get_free(info);

	/*
	 * Clone buffer
	 */
	memcpy(buf->vnet, arg->vnet, arg->vnet_len);
	memcpy(buf->eth, arg->eth, arg->eth_len);
	buf->vnet_len	= arg->vnet_len;
	buf->eth_len	= arg->eth_len;

	// Let's modify the cloned vnet header.
	// Since we compute full checksums in UIP (e.g. in ICMP, TCP, UDP),
	// the RX packet we are sending to the guest has a 100% correct checksum.
	// Therefore, we should either set flags = VIRTIO_NET_HDR_F_DATA_VALID
	// or flags = 0, and clear gso_type.
	if (buf->vnet_len >= 10) {
		struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf->vnet;
		struct virtio_net_hdr *orig_hdr = (struct virtio_net_hdr *)arg->vnet;
		
		FILE *f = fopen("/tmp/uip_debug.log", "a");
		if (f) {
			fprintf(f, "uip_buf_clone: orig_flags=%d orig_gso=%d orig_csum_start=%d orig_csum_offset=%d\n",
				orig_hdr->flags, orig_hdr->gso_type, orig_hdr->csum_start, orig_hdr->csum_offset);
			fflush(f);
			fclose(f);
		}

		hdr->flags = VIRTIO_NET_HDR_F_DATA_VALID;
		hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;
		hdr->hdr_len = 0;
		hdr->gso_size = 0;
		hdr->csum_start = 0;
		hdr->csum_offset = 0;
	}

	eth2		= (struct uip_eth *)buf->eth;
	eth2->src	= info->host_mac;
	eth2->dst	= arg->eth->src;

	return buf;
}
