#include <generated/csr.h>
#ifdef CSR_ETHMAC_BASE

#include <stdio.h>
#include <system.h>
#include <crc.h>
#include <hw/flags.h>
#include <hw/ethmac_mem.h>

#include <net/microudp.h>

//#define DEBUG_MICROUDP_TX
//#define DEBUG_MICROUDP_RX

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800

#ifdef CSR_ETHMAC_PREAMBLE_CRC_ADDR
#define HW_PREAMBLE_CRC
#endif

struct ethernet_header {
#ifndef HW_PREAMBLE_CRC
	unsigned char preamble[8];
#endif
	unsigned char destmac[6];
	unsigned char srcmac[6];
	unsigned short ethertype;
} __attribute__((packed));

static void fill_eth_header(struct ethernet_header *h, const unsigned char *destmac, const unsigned char *srcmac, unsigned short ethertype)
{
	int i;

#ifndef HW_PREAMBLE_CRC
	for(i=0;i<7;i++)
		h->preamble[i] = 0x55;
	h->preamble[7] = 0xd5;
#endif
	for(i=0;i<6;i++)
		h->destmac[i] = destmac[i];
	for(i=0;i<6;i++)
		h->srcmac[i] = srcmac[i];
	h->ethertype = ethertype;
}

#define ARP_HWTYPE_ETHERNET 0x0001
#define ARP_PROTO_IP        0x0800
#ifndef HW_PREAMBLE_CRC
#define ARP_PACKET_LENGTH 68
#else
#define ARP_PACKET_LENGTH 60
#endif

#define ARP_OPCODE_REQUEST  0x0001
#define ARP_OPCODE_REPLY    0x0002

struct arp_frame {
	unsigned short hwtype;
	unsigned short proto;
	unsigned char hwsize;
	unsigned char protosize;
	unsigned short opcode;
	unsigned char sender_mac[6];
	unsigned int sender_ip;
	unsigned char target_mac[6];
	unsigned int target_ip;
	unsigned char padding[18];
} __attribute__((packed));

#define IP_IPV4			0x45
#define IP_DONT_FRAGMENT	0x4000
#define IP_TTL			64
#define IP_PROTO_UDP		0x11

struct ip_header {
	unsigned char version;
	unsigned char diff_services;
	unsigned short total_length;
	unsigned short identification;
	unsigned short fragment_offset;
	unsigned char ttl;
	unsigned char proto;
	unsigned short checksum;
	unsigned int src_ip;
	unsigned int dst_ip;
} __attribute__((packed));

struct udp_header {
	unsigned short src_port;
	unsigned short dst_port;
	unsigned short length;
	unsigned short checksum;
} __attribute__((packed));

struct udp_frame {
	struct ip_header ip;
	struct udp_header udp;
	char payload[];
} __attribute__((packed));

struct ethernet_frame {
	struct ethernet_header eth_header;
	union {
		struct arp_frame arp;
		struct udp_frame udp;
	} contents;
} __attribute__((packed));

typedef union {
	struct ethernet_frame frame;
	unsigned char raw[1532];
} ethernet_buffer;


static unsigned int rxslot;
static unsigned int rxlen;
static ethernet_buffer *rxbuffer;
static ethernet_buffer *rxbuffer0;
static ethernet_buffer *rxbuffer1;
static unsigned int txslot;
static unsigned int txlen;
static ethernet_buffer *txbuffer;
static ethernet_buffer *txbuffer0;
static ethernet_buffer *txbuffer1;

static void send_packet(void)
{
#ifndef HW_PREAMBLE_CRC
	unsigned int crc;
	crc = crc32(&txbuffer->raw[8], txlen-8);
	txbuffer->raw[txlen  ] = (crc & 0xff);
	txbuffer->raw[txlen+1] = (crc & 0xff00) >> 8;
	txbuffer->raw[txlen+2] = (crc & 0xff0000) >> 16;
	txbuffer->raw[txlen+3] = (crc & 0xff000000) >> 24;
	txlen += 4;
#endif

#ifdef DEBUG_MICROUDP_TX
	int j;
	printf(">>>> txlen : %d\n", txlen);
	for(j=0;j<txlen;j++)
		printf("%02x",txbuffer->raw[j]);
	printf("\n");
#endif

	ethmac_sram_reader_slot_write(txslot);
	ethmac_sram_reader_length_write(txlen);
	while(!(ethmac_sram_reader_ready_read()));
	ethmac_sram_reader_start_write(1);
	txslot = (txslot+1)%2;
	if (txslot)
		txbuffer = txbuffer1;
	else
		txbuffer = txbuffer0;
}

static unsigned char my_mac[6];
static unsigned int my_ip;

/* ARP cache - one entry only */
static unsigned char cached_mac[6];
static unsigned int cached_ip;

static void process_arp(void)
{
	const struct arp_frame *rx_arp = &rxbuffer->frame.contents.arp;
	struct arp_frame *tx_arp = &txbuffer->frame.contents.arp;

	if(rxlen < ARP_PACKET_LENGTH) return;
	if(rx_arp->hwtype != ARP_HWTYPE_ETHERNET) return;
	if(rx_arp->proto != ARP_PROTO_IP) return;
	if(rx_arp->hwsize != 6) return;
	if(rx_arp->protosize != 4) return;
	if(rx_arp->opcode == ARP_OPCODE_REPLY) {
		if(rx_arp->sender_ip == cached_ip) {
			int i;
			for(i=0;i<6;i++)
				cached_mac[i] = rx_arp->sender_mac[i];
		}
		return;
	}
	if(rx_arp->opcode == ARP_OPCODE_REQUEST) {
		if(rx_arp->target_ip == my_ip) {
			int i;

			fill_eth_header(&txbuffer->frame.eth_header,
				rx_arp->sender_mac,
				my_mac,
				ETHERTYPE_ARP);
			txlen = ARP_PACKET_LENGTH;
			tx_arp->hwtype = ARP_HWTYPE_ETHERNET;
			tx_arp->proto = ARP_PROTO_IP;
			tx_arp->hwsize = 6;
			tx_arp->protosize = 4;
			tx_arp->opcode = ARP_OPCODE_REPLY;
			tx_arp->sender_ip = my_ip;
			for(i=0;i<6;i++)
				tx_arp->sender_mac[i] = my_mac[i];
			tx_arp->target_ip = rx_arp->sender_ip;
			for(i=0;i<6;i++)
				tx_arp->target_mac[i] = rx_arp->sender_mac[i];
			send_packet();
		}
		return;
	}
}

static const unsigned char broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

int microudp_arp_resolve(unsigned int ip)
{
	struct arp_frame *arp = &txbuffer->frame.contents.arp;
	int i;
	int tries;
	int timeout;

	if(cached_ip == ip) {
		for(i=0;i<6;i++)
			if(cached_mac[i]) return 1;
	}
	cached_ip = ip;
	for(i=0;i<6;i++)
		cached_mac[i] = 0;

	for(tries=0;tries<5;tries++) {
		/* Send an ARP request */
		fill_eth_header(&txbuffer->frame.eth_header,
				broadcast,
				my_mac,
				ETHERTYPE_ARP);
		txlen = ARP_PACKET_LENGTH;
		arp->hwtype = ARP_HWTYPE_ETHERNET;
		arp->proto = ARP_PROTO_IP;
		arp->hwsize = 6;
		arp->protosize = 4;
		arp->opcode = ARP_OPCODE_REQUEST;
		arp->sender_ip = my_ip;
		for(i=0;i<6;i++)
			arp->sender_mac[i] = my_mac[i];
		arp->target_ip = ip;
		for(i=0;i<6;i++)
			arp->target_mac[i] = 0;
		send_packet();

		/* Do we get a reply ? */
		for(timeout=0;timeout<2000000;timeout++) {
			microudp_service();
			for(i=0;i<6;i++)
				if(cached_mac[i]) return 1;
		}
	}

	return 0;
}

static unsigned short ip_checksum(unsigned int r, void *buffer, unsigned int length, int complete)
{
	unsigned char *ptr;
	unsigned int i;

	ptr = (unsigned char *)buffer;
	length >>= 1;

	for(i=0;i<length;i++)
		r += ((unsigned int)(ptr[2*i]) << 8)|(unsigned int)(ptr[2*i+1]) ;

	/* Add overflows */
	while(r >> 16)
		r = (r & 0xffff) + (r >> 16);

	if(complete) {
		r = ~r;
		r &= 0xffff;
		if(r == 0) r = 0xffff;
	}
	return r;
}

void *microudp_get_tx_buffer(void)
{
	return txbuffer->frame.contents.udp.payload;
}

struct pseudo_header {
	unsigned int src_ip;
	unsigned int dst_ip;
	unsigned char zero;
	unsigned char proto;
	unsigned short length;
} __attribute__((packed));

int microudp_send(unsigned short src_port, unsigned short dst_port, unsigned int length)
{
	struct pseudo_header h;
	unsigned int r;

	if((cached_mac[0] == 0) && (cached_mac[1] == 0) && (cached_mac[2] == 0)
		&& (cached_mac[3] == 0) && (cached_mac[4] == 0) && (cached_mac[5] == 0))
		return 0;

	txlen = length + sizeof(struct ethernet_header) + sizeof(struct udp_frame);
	if(txlen < ARP_PACKET_LENGTH) txlen = ARP_PACKET_LENGTH;

	fill_eth_header(&txbuffer->frame.eth_header,
		cached_mac,
		my_mac,
		ETHERTYPE_IP);

	txbuffer->frame.contents.udp.ip.version = IP_IPV4;
	txbuffer->frame.contents.udp.ip.diff_services = 0;
	txbuffer->frame.contents.udp.ip.total_length = length + sizeof(struct udp_frame);
	txbuffer->frame.contents.udp.ip.identification = 0;
	txbuffer->frame.contents.udp.ip.fragment_offset = IP_DONT_FRAGMENT;
	txbuffer->frame.contents.udp.ip.ttl = IP_TTL;
	h.proto = txbuffer->frame.contents.udp.ip.proto = IP_PROTO_UDP;
	txbuffer->frame.contents.udp.ip.checksum = 0;
	h.src_ip = txbuffer->frame.contents.udp.ip.src_ip = my_ip;
	h.dst_ip = txbuffer->frame.contents.udp.ip.dst_ip = cached_ip;
	txbuffer->frame.contents.udp.ip.checksum = ip_checksum(0, &txbuffer->frame.contents.udp.ip,
		sizeof(struct ip_header), 1);

	txbuffer->frame.contents.udp.udp.src_port = src_port;
	txbuffer->frame.contents.udp.udp.dst_port = dst_port;
	h.length = txbuffer->frame.contents.udp.udp.length = length + sizeof(struct udp_header);
	txbuffer->frame.contents.udp.udp.checksum = 0;

	h.zero = 0;
	r = ip_checksum(0, &h, sizeof(struct pseudo_header), 0);
	if(length & 1) {
		txbuffer->frame.contents.udp.payload[length] = 0;
		length++;
	}
	r = ip_checksum(r, &txbuffer->frame.contents.udp.udp,
		sizeof(struct udp_header)+length, 1);
	txbuffer->frame.contents.udp.udp.checksum = r;

	send_packet();

	return 1;
}

static udp_callback rx_callback;

static void process_ip(void)
{
	if(rxlen < (sizeof(struct ethernet_header)+sizeof(struct udp_frame))) return;
	/* We don't verify UDP and IP checksums and rely on the Ethernet checksum solely */
	if(rxbuffer->frame.contents.udp.ip.version != IP_IPV4) return;
	// check disabled for QEMU compatibility
	//if(rxbuffer->frame.contents.udp.ip.diff_services != 0) return;
	if(rxbuffer->frame.contents.udp.ip.total_length < sizeof(struct udp_frame)) return;
	// check disabled for QEMU compatibility
	//if(rxbuffer->frame.contents.udp.ip.fragment_offset != IP_DONT_FRAGMENT) return;
	if(rxbuffer->frame.contents.udp.ip.proto != IP_PROTO_UDP) return;
	if(rxbuffer->frame.contents.udp.ip.dst_ip != my_ip) return;
	if(rxbuffer->frame.contents.udp.udp.length < sizeof(struct udp_header)) return;

	if(rx_callback)
		rx_callback(rxbuffer->frame.contents.udp.ip.src_ip, rxbuffer->frame.contents.udp.udp.src_port, rxbuffer->frame.contents.udp.udp.dst_port, rxbuffer->frame.contents.udp.payload, rxbuffer->frame.contents.udp.udp.length-sizeof(struct udp_header));
}

void microudp_set_callback(udp_callback callback)
{
	rx_callback = callback;
}

static void process_frame(void)
{
	flush_cpu_dcache();

#ifdef DEBUG_MICROUDP_RX
	int j;
	printf("<<< rxlen : %d\n", rxlen);
	for(j=0;j<rxlen;j++)
		printf("%02x", rxbuffer->raw[j]);
	printf("\n");
#endif

#ifndef HW_PREAMBLE_CRC
	int i;
	for(i=0;i<7;i++)
		if(rxbuffer->frame.eth_header.preamble[i] != 0x55) return;
	if(rxbuffer->frame.eth_header.preamble[7] != 0xd5) return;
#endif

#ifndef HW_PREAMBLE_CRC
	unsigned int received_crc;
	unsigned int computed_crc;
	received_crc = ((unsigned int)rxbuffer->raw[rxlen-1] << 24)
		|((unsigned int)rxbuffer->raw[rxlen-2] << 16)
		|((unsigned int)rxbuffer->raw[rxlen-3] <<  8)
		|((unsigned int)rxbuffer->raw[rxlen-4]);
	computed_crc = crc32(&rxbuffer->raw[8], rxlen-12);
	if(received_crc != computed_crc) return;

	rxlen -= 4; /* strip CRC here to be consistent with TX */
#endif

	if(rxbuffer->frame.eth_header.ethertype == ETHERTYPE_ARP) process_arp();
	else if(rxbuffer->frame.eth_header.ethertype == ETHERTYPE_IP) process_ip();
}

void microudp_start(const unsigned char *macaddr, unsigned int ip)
{
	int i;
	ethmac_sram_reader_ev_pending_write(ETHMAC_EV_SRAM_READER);
	ethmac_sram_writer_ev_pending_write(ETHMAC_EV_SRAM_WRITER);

	rxbuffer0 = (ethernet_buffer *)ETHMAC_RX0_BASE;
	rxbuffer1 = (ethernet_buffer *)ETHMAC_RX1_BASE;
	txbuffer0 = (ethernet_buffer *)ETHMAC_TX0_BASE;
	txbuffer1 = (ethernet_buffer *)ETHMAC_TX1_BASE;

	rxslot = 0;
	txslot = 0;

	rxbuffer = rxbuffer0;
	txbuffer = txbuffer0;

	for(i=0;i<6;i++)
		my_mac[i] = macaddr[i];
	my_ip = ip;

	cached_ip = 0;
	for(i=0;i<6;i++)
		cached_mac[i] = 0;

	rx_callback = (udp_callback)0;
}

void microudp_service(void)
{
	if(ethmac_sram_writer_ev_pending_read() & ETHMAC_EV_SRAM_WRITER) {
		rxslot = ethmac_sram_writer_slot_read();
		rxlen = ethmac_sram_writer_length_read();
		if (rxslot)
			rxbuffer = rxbuffer1;
		else
			rxbuffer = rxbuffer0;
		process_frame();
		ethmac_sram_writer_ev_pending_write(ETHMAC_EV_SRAM_WRITER);
	}
}

static void busy_wait(unsigned int ds)
{
	timer0_en_write(0);
	timer0_reload_write(0);
	timer0_load_write(SYSTEM_CLOCK_FREQUENCY/10*ds);
	timer0_en_write(1);
	timer0_update_value_write(1);
	while(timer0_value_read()) timer0_update_value_write(1);
}

void eth_init(void)
{
	ethphy_crg_reset_write(0);
	busy_wait(2);
	/* that pesky ethernet PHY needs two resets at times... */
	ethphy_crg_reset_write(1);
	busy_wait(2);
	ethphy_crg_reset_write(0);
	busy_wait(2);
}

#ifdef CSR_ETHPHY_MODE_DETECTION_MODE_ADDR
void eth_mode(void)
{
	printf("Ethernet phy mode: ");
	if (ethphy_mode_detection_mode_read())
		printf("MII");
	else
		printf("GMII");
	printf("\n");
}
#endif

#endif
