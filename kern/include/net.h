/* Copyright (c) 2010 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * David Zhu <yuzhu@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Arch independent networking infrastructure */

#ifndef ROS_KERN_NET_H
#define ROS_KERN_NET_H

#include <bits/netinet.h>
#include <stdio.h>

/* network internal error code */
#define ERR_BUF 

/* A few other useful standard defines.  Note the IP header can change size. */
#define ETH_HDR_SZ 14 // without padding, 16 with padding
#define UDP_HDR_SZ 8
#define IP_ETH_TYPE 0x0800
#define IP_HDR_SZ 20
/* ROS defaults: They really should be netif specific*/
#define DEFAULT_TTL 64
#define DEFAULT_MTU 1500
// is this network order already?
#define IP_ADDR 0x0A000002  //lookout for address order


/* Don't forget the bytes are in network order */
struct ethernet_hdr {
	/* might want to pad to increase access speed? */
	uint8_t           padding[2];
	uint8_t						dst_mac[6];
	uint8_t						src_mac[6];
	uint16_t					eth_type;
	/* might be an optional 802.1q tag here */
} __attribute__((packed));


/* For the bit-enumerated fields, note that you need to read "backwards" through
 * the byte (first bits in memory are the "LSB" of the byte).  Can't seem to be
 * able to do it with flags/fragments (3/13 bits each...). */
struct ip_hdr {
	/* TODO: Are these accesses slower? */
	unsigned					hdr_len : 4;
	unsigned					version : 4;
	uint8_t						tos;

	uint16_t					packet_len;
	/* ip header id is used for fragmentation reassembly */
	uint16_t					id;  // 1 index this?
	/* flags controlling fragmentation(do not fragment etc) */
	uint16_t					flags_frags;
	/* statically set to a constatnt right now */
	uint8_t						ttl;
	uint8_t						protocol;
	uint16_t					checksum;
	uint32_t					src_addr;
	uint32_t					dst_addr;
	/* Options could be here (depends on the hdr length) */
} __attribute__((packed));

struct udp_hdr {
	uint16_t					src_port;
	uint16_t					dst_port;
	uint16_t					length;
	uint16_t					checksum;
};

uint16_t __ip_checksum(void *buf, unsigned int len, uint32_t sum);
uint16_t ip_checksum(struct ip_hdr *ip_hdr);
uint16_t udp_checksum(struct ip_hdr *ip_hdr, struct udp_hdr *udp_hdr);

// TODO: Move this to a better location
void dumppacket(unsigned char *buff, size_t len);
#endif /* ROS_KERN_NET_H */
