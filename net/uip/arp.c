#include "kvm/uip.h"

#include <stdio.h>
#include <arpa/inet.h>

int uip_tx_do_arp(struct uip_tx_arg *arg)
{
	struct uip_arp *arp, *arp2;
	struct uip_info *info;
	struct uip_buf *buf;

	info = arg->info;
	buf = uip_buf_clone(arg);

	arp	 = (struct uip_arp *)(arg->eth);
	arp2	 = (struct uip_arp *)(buf->eth);

	{
		struct in_addr s, d, h;
		char sip_str[INET_ADDRSTRLEN], dip_str[INET_ADDRSTRLEN], hip_str[INET_ADDRSTRLEN];
		s.s_addr = arp->sip;
		d.s_addr = arp->dip;
		h.s_addr = htonl(info->host_ip);
		strncpy(sip_str, inet_ntoa(s), sizeof(sip_str));
		strncpy(dip_str, inet_ntoa(d), sizeof(dip_str));
		strncpy(hip_str, inet_ntoa(h), sizeof(hip_str));
		FILE *f = fopen("/tmp/uip_debug.log", "a");
		if (f) {
			fprintf(f, "ARP: op=%d sip=%s dip=%s host_ip=%s smac=%02x:%02x:%02x:%02x:%02x:%02x host_mac=%02x:%02x:%02x:%02x:%02x:%02x match=%d\n",
				ntohs(arp->op),
				sip_str, dip_str, hip_str,
				arp->smac.addr[0], arp->smac.addr[1], arp->smac.addr[2],
				arp->smac.addr[3], arp->smac.addr[4], arp->smac.addr[5],
				info->host_mac.addr[0], info->host_mac.addr[1], info->host_mac.addr[2],
				info->host_mac.addr[3], info->host_mac.addr[4], info->host_mac.addr[5],
				(arp->dip == htonl(info->host_ip)));
			fflush(f);
			fclose(f);
		}
	}

	/*
	 * ARP replay code: 2
	 */
	arp2->op   = htons(0x2);
	arp2->dmac = arp->smac;
	arp2->dip  = arp->sip;

	if (arp->dip == htonl(info->host_ip)) {
		arp2->smac = info->host_mac;
		arp2->sip = htonl(info->host_ip);

		uip_buf_set_used(info, buf);
	}

	return 0;
}
