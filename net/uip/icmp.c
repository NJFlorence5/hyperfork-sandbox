#include "kvm/uip.h"

#include <stdio.h>
#include <arpa/inet.h>

int uip_tx_do_ipv4_icmp(struct uip_tx_arg *arg)
{
	struct uip_ip *ip, *ip2;
	struct uip_icmp *icmp, *icmp2;
	struct uip_buf *buf;

	icmp		= (struct uip_icmp *)(arg->eth);
	ip		= (struct uip_ip *)(arg->eth);

	{
		struct in_addr s, d;
		char sip_str[INET_ADDRSTRLEN], dip_str[INET_ADDRSTRLEN];
		s.s_addr = ip->sip;
		d.s_addr = ip->dip;
		strncpy(sip_str, inet_ntoa(s), sizeof(sip_str));
		strncpy(dip_str, inet_ntoa(d), sizeof(dip_str));
		FILE *f = fopen("/tmp/uip_debug.log", "a");
		if (f) {
			fprintf(f, "ICMP: type=%d code=%d sip=%s dip=%s\n",
				icmp->type, icmp->code, sip_str, dip_str);
			fflush(f);
			fclose(f);
		}
	}

	buf		= uip_buf_clone(arg);

	icmp2		= (struct uip_icmp *)(buf->eth);
	ip2		= (struct uip_ip *)(buf->eth);

	ip2->sip	= ip->dip;
	ip2->dip	= ip->sip;
	ip2->csum	= 0;
	/*
	 * ICMP reply: 0
	 */
	icmp2->type	= 0;
	icmp2->csum	= 0;
	ip2->csum	= uip_csum_ip(ip2);
	icmp2->csum	= uip_csum_icmp(icmp2);

	uip_buf_set_used(arg->info, buf);

	return 0;
}
