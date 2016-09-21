#include <stdio.h>
#include <string.h>
#include <gmalloc.h>
#include <net/ni.h>
#include <net/arp.h>
#include <net/packet.h>

#include <timer.h>
#include <net/interface.h>
#include <net/ether.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <net/checksum.h>
#include <util/list.h>
#include <util/map.h>

#define MAX_SEQNUM	2147483648
#define ADDRESS 0xc0a8640a	// only for develop version
#define MAX_WNDSIZE 1073741824	// 1GB

#define TCP_CLOSED		0	
#define TCP_LISTEN 		1
#define TCP_SYN_RCVD		2
#define TCP_SYN_SENT		3
#define TCP_ESTABLISHED		4
#define TCP_CLOSE_WAIT		5
#define TCP_LAST_ACK		6
#define TCP_FIN_WAIT_1		7
#define TCP_FIN_WAIT_2		8
#define TCP_CLOSING		9
#define TCP_TIME_WAIT		10
#define TCP_TCB_CREATED		11

#define FIN	0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10
#define URG 0x20
#define ECE 0x40
#define CWR 0x80

#define SND_WND_MAX 43690
#define RECV_WND_MAX 1500
#define ACK_TIMEOUT 2000000	// 2 sec
#define MSL 10000000	// 10 sec 
#define SCALE 128
 
typedef struct {
	uint32_t sequence;
	uint32_t len;
	uint64_t timeout;
	Packet* packet;
} Segment;

typedef struct {
	uint32_t sip;
	uint16_t sport;
	
	uint64_t dmac;
	uint32_t dip;
	uint32_t dport;

	int state;
	uint32_t sequence;
	uint32_t acknowledgement;
	uint64_t timer_id;
	uint64_t timeout;
	TCPCallback* callback;
	void* context;

	List* unack_list;	// sent segment but no ack.
	uint32_t snd_wnd_max;
	uint32_t snd_wnd_cur;
	uint32_t recv_wnd_max;
	uint32_t last_ack;
	NetworkInterface* ni;
} TCB;

typedef struct {
	TCB* tcb;
	uint64_t timeout;
} Callback;

static bool counter;
static uint32_t ip_id;
static Map* tcbs;
static List* time_wait_list;
static List* conn_try_list;

static Packet* packet_create(TCB* tcb, uint8_t flags, const void* data, int len);
static bool packet_out(TCB* tcb, Packet* packet, uint16_t len); 

static uint32_t tcp_init_seqnum() {
	uint64_t time;
	uint32_t* p = (uint32_t*)&time;
	asm volatile("rdtsc" : "=a"(p[0]), "=d"(p[1]));
	return time % MAX_SEQNUM;
}

static void ip_init_id() {
	//TODO: needs something different
	ip_id = 0x8000;
}

static uint32_t ip_get_id(int ack) {
	if(ack == 1) {
		return ip_id;
	} else {
		ip_id++;
	}
	return	ip_id;
}

TCB* tcb_get(uint32_t tcb_key) {
	TCB* tcb = map_get(tcbs, (void*)(uintptr_t)tcb_key);

	if(tcb == NULL) {
		printf("tcb is null \n");
		return NULL;
	}
	return tcb;
}

void tcp_set_callback(int32_t socket, TCPCallback* callback, void* context) {
	TCB* tcb = tcb_get(socket);
	tcb->callback = callback;
}

TCPCallback* tcp_callback_create() {
	return (TCPCallback*)gmalloc(sizeof(TCPCallback));
}

void tcp_callback_destroy(TCPCallback* callback) {
	gfree(callback);
}

TCPCallback* tcp_get_callback(int32_t socket) {
	TCB* tcb = tcb_get(socket);

	if(tcb->callback == NULL) {
		return tcp_callback_create();	
	} else {
		return tcb->callback;
	}
}

bool tcp_init() {
	count2 = 0;
	counter = false;
	tcbs = map_create(4, map_uint64_hash, map_uint64_equals, NULL);
	if(!tcbs) {
		printf("tcbs create fail\n");

		return false;
	}
	
	time_wait_list = list_create(NULL);
	if(!time_wait_list) {
		printf("time_wait_list create fail\n");
		
		map_destroy(tcbs);

		return false;
	}

	conn_try_list = list_create(NULL);
	if(!conn_try_list) {
		printf("conn_try_list create fail\n");
		
		list_destroy(time_wait_list);
		map_destroy(tcbs);

		return false;
	}

	return true;
}

// TODO: Need something other than this
static uint32_t tcb_key_create(uint32_t sip, uint16_t sport) {
	uint32_t tcb_key = (sip & 0xffff);
	tcb_key |= (sport & 0xff) ;
	return tcb_key;
}

static TCB* tcb_create(NetworkInterface* ni, uint32_t sip, uint32_t dip, uint16_t dport, TCPCallback* callback) {
	TCB* tcb = (TCB*)gmalloc(sizeof(TCB));
	if(tcb == NULL)
		printf("tcb NULL\n");
	
	tcb->unack_list = list_create(NULL);
	if(tcb->unack_list == NULL) {
		printf("list_create error\n");
		gfree(tcb);
		return NULL;
	}

	tcb->sip = sip; 
	tcb->sport = tcp_port_alloc(ni, sip);
	tcb->dip = dip;
	tcb->dport = dport;
	
	tcb->state = TCP_CLOSED;
	tcb->sequence = tcp_init_seqnum();
	tcb->acknowledgement = 0;
	tcb->callback = callback;
	
	tcb->recv_wnd_max = RECV_WND_MAX;
	tcb->snd_wnd_max = 0;
	tcb->snd_wnd_cur = 0;

	tcb->ni = ni;
	return tcb;
}

static bool tcb_destroy(TCB* tcb) {
	tcp_port_free(tcb->ni, tcb->sip, tcb->sport);
	
	ListIterator iter;
	list_iterator_init(&iter, tcb->unack_list);

	while(list_iterator_has_next(&iter)) {
		Segment* seg = list_iterator_next(&iter);
		ni_free(seg->packet);
		list_iterator_remove(&iter);
		gfree(seg);
	}

	list_destroy(tcb->unack_list);
	
	uint32_t tcb_key = tcb_key_create(endian32(tcb->sip), endian16(tcb->sport));
	void* result = map_remove(tcbs, (void*)(uintptr_t)tcb_key);
	if(!result)
		printf("map_remove error\n");
	
	tcp_callback_destroy(tcb->callback);
	gfree(tcb);
	printf("tcb_destory!!\n");
	
	return true;
}



// TODO: maybe need routing func that finds src_ip from ni.
static uint32_t route(NetworkInterface* ni, uint32_t dst_addr, uint16_t des_port) {
	return ADDRESS;
}

bool tcp_try_connect(TCB* tcb) {
	uint64_t mac = arp_get_mac(tcb->ni, tcb->dip, tcb->sip);
	
	if(mac != 0xffffffffffff) {
		tcb->dmac = mac;
		
		if(!ni_output_available(tcb->ni))
			return false;

		Packet* packet = packet_create(tcb, SYN, NULL, 0);
		if(!packet)
			return false;

		if(!packet_out(tcb, packet, 0))
			return false;
		
		tcb->state = TCP_SYN_SENT;
		tcb->sequence += 1;
		return true;
	}

	return false;
}

uint32_t tcp_connect(NetworkInterface* ni, uint32_t dst_addr, uint16_t dst_port, TCPCallback* tcp_callback, void* context) {
	uint32_t src_addr = route(ni, dst_addr, dst_port);

	TCB* tcb = tcb_create(ni, src_addr, dst_addr, dst_port, tcp_callback);
	if(tcb == NULL) {
		printf("tcb NULL\n");
		return 0;
	}

	uint32_t tcb_key = tcb_key_create(endian32(tcb->sip), endian16(tcb->sport));	
	if(!map_put(tcbs, (void*)(uintptr_t)tcb_key, (void*)(uintptr_t) tcb)) {
		printf("map_put error\n");
		return 0;
	}

	ip_init_id();	// TODO: check this function's role
	uint64_t mac = arp_get_mac(ni, tcb->dip, tcb->sip);

	if(mac == 0xffffffffffff) {
		Callback* callback = gmalloc(sizeof(Callback));
		callback->tcb = tcb;
		callback->timeout = timer_us() + 3 * 1000 * 1000;

		if(!list_add(conn_try_list, (void*)callback))
			return 0;	// error
	} else {
		if(!tcp_try_connect(tcb)) {
			tcb_destroy(tcb);
			
			return 0;
		}
	}  

	return tcb_key;
}

bool tcp_close(uint32_t socket) {
	TCB* tcb = tcb_get(socket);
	if(!tcb)
		return false;

	if(!packet_out(tcb, NULL, 0))
		return false;
	
	tcb->state = TCP_FIN_WAIT_1;
	tcb->sequence += 1;

	return true;
}

int32_t tcp_send(uint32_t socket, void* data, const uint32_t len) {
	if(len == 0)
		return 0;

	TCB* tcb = tcb_get(socket);
	if(!tcb)
		return -1;

	if(tcb->snd_wnd_max < tcb->snd_wnd_cur || tcb->snd_wnd_max	- tcb->snd_wnd_cur < len)
		return -2;

	if(!ni_output_available(tcb->ni))
		return -3;
		
	Packet* packet = packet_create(tcb, ACK | PSH, data, len);
	if(!packet)
		return -4;

	if(!packet_out(tcb, packet, len)) {
		tcb->snd_wnd_cur -= len;

		return -4;
	}
	
	tcb->snd_wnd_cur += len;
	tcb->sequence += len;

	return len;	
}

bool tcp_process(Packet* packet) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
	if(endian16(ether->type) != ETHER_TYPE_IPv4)
		return false;

	IP* ip = (IP*)ether->payload;

	if(!ni_ip_get(packet->ni, endian32(ip->destination)))
		return false;
	
	if(ip->protocol != IP_PROTOCOL_TCP)
		return false;
	
	TCP* tcp = (TCP*)ip->body;

	uint32_t tcb_key = tcb_key_create(ip->destination,tcp->destination);

	TCB* tcb = tcb_get(tcb_key);
	if(!tcb) {
		printf("tcb null\n");
		return false;
	}

	if(endian16(tcp->source) != tcb->dport) {
		return false;
	}

	switch(tcb->state) {
		case TCP_CLOSED:
			printf("Connection is closed\n");
			break; 

		case TCP_SYN_SENT:
			printf("proc syn_sent\n");
			if(tcp->syn == 1 && tcp->ack == 1) {
				tcb->acknowledgement = endian32(tcp->sequence) + 1;
				tcb->last_ack = endian32(tcp->ack);

				tcb->snd_wnd_max = endian16(tcp->window); //* SCALE;
				tcb->snd_wnd_cur = 0;
				
				Packet* packet = packet_create(tcb, ACK, NULL, 0);
				if(!packet)
					printf("send ack fail in syn\n");

				if(packet_out(tcb, packet, 0)) {	// send ack
					tcb->state = TCP_ESTABLISHED;
					tcb->callback->connected(tcb_key, tcb->dip, tcb->dport, tcb->context);
				}
			} else if (tcp->rst == 1) {
				//TODO: timeout
				tcb->state = TCP_CLOSED;
			}
			break;

		case TCP_SYN_RCVD:
			//TODO: server side	
			break;

		case TCP_ESTABLISHED:
			//printf("proc establish\n");
			if(tcp->ack == 1) {
				uint32_t tmp_ack = endian32(tcp->acknowledgement);
				count2 = tcb->snd_wnd_cur;

				if(tcb->last_ack <= tcb->sequence) {
					if(tcb->last_ack <= tmp_ack && tmp_ack <= tcb->sequence) {
						tcb->snd_wnd_max = endian16(tcp->window) * SCALE;
						ListIterator iter;
						list_iterator_init(&iter, tcb->unack_list);

						tcb->last_ack = tmp_ack;

						while(list_iterator_has_next(&iter)) {
							Segment* seg = list_iterator_next(&iter);

							list_iterator_remove(&iter);
							tcb->snd_wnd_cur -= seg->len;

							tcb->callback->sent(tcb_key, seg->len, tcb->context);

							ni_free(seg->packet);

							if(seg->sequence + seg->len == tmp_ack) {
								gfree(seg);
								break;
							} else {
								gfree(seg);
							}
						}
					}
				} else {
					if(tcb->last_ack <= tmp_ack || tmp_ack <= tcb->sequence) {
						tcb->snd_wnd_max = endian16(tcp->window) * SCALE;
						ListIterator iter;
						list_iterator_init(&iter, tcb->unack_list);

						tcb->last_ack = tmp_ack;

						while(list_iterator_has_next(&iter)) {
							Segment* seg = list_iterator_next(&iter);

							list_iterator_remove(&iter);
							tcb->snd_wnd_cur -= seg->len;

							tcb->callback->sent(tcb_key, seg->len, tcb->context);

							ni_free(seg->packet);

							if(seg->sequence + seg->len == tmp_ack) {
								gfree(seg);
								break;
							} else {
								gfree(seg);
							}
						}
					}
				}
				uint16_t len = endian16(ip->length) - ip->ihl * 4 - tcp->offset * 4;
				if(len > 0 && tcb->acknowledgement == endian32(tcp->sequence)) {
					tcb->acknowledgement += len;

					counter = !counter;
					if(counter) {
						Packet* packet = packet_create(tcb, ACK, NULL, 0);
						if(!packet)
							printf("sending ack fail in est\n");

						if(!packet_out(tcb, packet, 0))	//send ack
							printf("send ack fail\n");
						// TODO : if packet_out is failed, maybe ack will never send again...
					}

					tcb->callback->received(tcb_key, (uint8_t*)tcp + tcp->offset * 4, len, tcb->context);	// TODO: check last arg(context).
				}
			} else {
				//TODO: no logic decided
			}
			break;
		case TCP_LISTEN:
			//TODO: no logic decided 
			break;
		case TCP_FIN_WAIT_1:
			if(tcp->fin && tcp->ack) {
				ListIterator iter;
				list_iterator_init(&iter, tcb->unack_list);

				while(list_iterator_has_next(&iter)) {
					Segment* seg = list_iterator_next(&iter);

					if(seg->sequence < endian32(tcp->acknowledgement)) {
						list_iterator_remove(&iter);
						tcb->snd_wnd_cur -= seg->len;

						tcb->callback->sent(tcb_key, seg->len, tcb->context);

						ni_free(seg->packet);
						gfree(seg);
					}
				}

				uint16_t len = endian16(ip->length) - ip->ihl * 4 - tcp->offset * 4;
				if(len > 0 && tcb->sequence >= endian32(tcp->acknowledgement)) {
					tcb->acknowledgement += len;
					// TODO: need packet_Create
					packet_out(tcb, NULL, 0);	//send ack

					tcb->callback->received(tcb_key, (uint8_t*)tcp + tcp->offset * 4, len, tcb->context);
				} else if(tcb->acknowledgement <= endian32(tcp->sequence)) {
					tcb->acknowledgement++;
					// TODO: need packet_create
					packet_out(tcb, NULL, 0);	//send ack
					
					tcb->state = TCP_CLOSING;
				}

				if(tcb->sequence == endian32(tcp->acknowledgement)) {
					tcb->state = TCP_CLOSING;
					tcb->state = TCP_TIME_WAIT;
					tcb->timeout = timer_us() +  MSL;
					
					list_add(time_wait_list, tcb);
				}
			} else if(tcp->ack) {
				ListIterator iter;
				list_iterator_init(&iter, tcb->unack_list);

				while(list_iterator_has_next(&iter)) {
					Segment* seg = list_iterator_next(&iter);

					if(seg->sequence < endian32(tcp->acknowledgement)) {
						list_iterator_remove(&iter);
						tcb->snd_wnd_cur -= seg->len;

						tcb->callback->sent(tcb_key, seg->len, tcb->context);

						ni_free(seg->packet);
						gfree(seg);
					}
				}

				uint16_t len = endian16(ip->length) - ip->ihl * 4 - tcp->offset * 4;
				if(len > 0 && tcb->sequence >= endian32(tcp->acknowledgement)) {
					tcb->acknowledgement += len;
						
					// TODO:need packet_create
					packet_out(tcb, NULL, 0);	//send ack

					tcb->callback->received(tcb_key, (uint8_t*)tcp + tcp->offset * 4, len, tcb->context);
				}

				if(tcb->sequence == endian32(tcp->acknowledgement)) {
					tcb->state = TCP_FIN_WAIT_2;
				}
			}
			break;

		case TCP_FIN_WAIT_2:
			if(tcp->ack) {
				ListIterator iter;
				list_iterator_init(&iter, tcb->unack_list);

				while(list_iterator_has_next(&iter)) {
					Segment* seg = list_iterator_next(&iter);

					if(seg->sequence < endian32(tcp->acknowledgement)) {
						list_iterator_remove(&iter);
						tcb->snd_wnd_cur -= seg->len;

						tcb->callback->sent(tcb_key, seg->len, tcb->context);

						ni_free(seg->packet);
						gfree(seg);
					}
				}
				
				uint16_t len = endian16(ip->length) - ip->ihl * 4 - tcp->offset * 4;
				if(len > 0 && tcb->acknowledgement == endian32(tcp->sequence)) {
					tcb->acknowledgement += len;

					tcb->state = TCP_CLOSED;
					packet_out(tcb, NULL, 0);	//send ack

					tcb->callback->received(tcb_key, (uint8_t*)tcp + tcp->offset * 4, len, tcb->context);	// TODO: check last arg(context).
				}
				
				if(tcp->fin && tcb->sequence == endian32(tcp->acknowledgement)) {
					tcb->acknowledgement = endian32(tcp->sequence) + 1;
					packet_out(tcb, NULL, 0); //send ack
					tcb->state = TCP_TIME_WAIT;	// TODO need to implement time wait
					tcb->timeout = timer_us() +  MSL;
					
					list_add(time_wait_list, tcb);
				}
			}
			break;
		case TCP_CLOSING:
			if(tcp->ack) {
				if(tcb->sequence == endian32(tcp->acknowledgement)) {
					tcb->state = TCP_TIME_WAIT;	//TODO need to implement time wait
					tcb->timeout = timer_us() +  MSL;
					
					list_add(time_wait_list, tcb);
				}
			}
			//TODO: no logic decided 
			break;
		case TCP_TIME_WAIT:
			if(tcb->timeout < timer_us()) {
				tcb->state = TCP_CLOSED;
				tcb_destroy(tcb);
			}

			break;
		default:
			return false;
	}
	ni_free(packet);
	return true;
}

static Packet* packet_create(TCB* tcb, uint8_t flags, const void* data, int len) {
	NetworkInterface* ni = tcb->ni;

	Packet* packet;

	if(flags & SYN)
		packet = ni_alloc(ni, sizeof(Ether) + sizeof(IP) + sizeof(TCP) + 4 + 4/* option */ + len);
	else
		packet = ni_alloc(ni, sizeof(Ether) + sizeof(IP) + sizeof(TCP) + len);

	if(!packet)
		return NULL;

	Ether* ether = (Ether*)(packet->buffer + packet->start);
	ether->dmac = endian48(tcb->dmac);
	ether->smac = endian48(ni->mac);
	ether->type = endian16(ETHER_TYPE_IPv4);
	
	IP* ip = (IP*)ether->payload;
	ip->ihl = endian8(5);
	ip->version = endian8(4);
	ip->ecn = endian8(0); 
	ip->dscp = endian8(0);

	ip_id = ip_get_id((flags & ACK) >> 4);
	ip->id = endian16(ip_id);
	ip->flags_offset = 0x40;
	ip->ttl = endian8(IP_TTL);
	ip->protocol = endian8(IP_PROTOCOL_TCP);
	ip->source = endian32(tcb->sip);
	ip->destination = endian32(tcb->dip);
	
	TCP* tcp = (TCP*)ip->body;
	tcp->source = endian16(tcb->sport);
	tcp->destination = endian16(tcb->dport);
	tcp->sequence = endian32(tcb->sequence);
	tcp->acknowledgement = endian32(tcb->acknowledgement);
	tcp->ns = endian8(0);
	tcp->reserved = endian8(0);
	tcp->fin = flags & FIN; 
	tcp->syn = (flags & SYN) >> 1;
	tcp->rst = (flags & SYN) >> 2;
	tcp->psh = (flags & PSH) >> 3;
	tcp->ack = (flags & ACK) >> 4;
	tcp->urg = (flags & URG) >> 5;
	tcp->ece = (flags & ECE) >> 6;
	tcp->cwr = (flags & CWR) >> 7;
	tcp->window = endian16(tcb->recv_wnd_max);
	tcp->urgent = endian16(0);
	
	if(tcp->syn) {
		tcp->offset = endian8(7);
		uint32_t mss_option = endian32(0x020405b4);
		uint32_t win_option = endian32(0x01030307);

		memcpy(tcp->payload, &mss_option, 4);
		memcpy((uint8_t*)(tcp->payload) + 4, &win_option, 4);

		tcp_pack(packet, len + 4 + 4);
	} else {
		tcp->offset = endian8(5);
		memcpy((uint8_t*)tcp + tcp->offset * 4, data, len);

		tcp_pack(packet, len);
	}
	
	return packet;
}

static bool packet_out(TCB* tcb, Packet* packet, uint16_t len) {
	NetworkInterface* ni = packet->ni;

	if(len == 0)
		return ni_output(ni, packet);

	if(ni_output_dup(ni, packet)) {
		Segment* segment = gmalloc(sizeof(Segment));
		if(!segment) {
			printf("seg malloc fail\n");
			return false;
		}

		segment->timeout = timer_us() + ACK_TIMEOUT;
		segment->len = len;
		segment->sequence = tcb->sequence;
		segment->packet = packet;

		if(!list_add(tcb->unack_list, segment)) {
			printf("list add fail\n");
			return false;
		}

		return true;
	} else {
		ni_free(packet);

		return false;
	}
}

// TODO: function naming. and maybe need some modulization.
//static bool packet_out(TCB* tcb, uint8_t flags, const void* data, int len) {
//	NetworkInterface* ni = tcb->ni;
//
//	Packet* packet;
//
//	if(syn) {
//		packet = ni_alloc(ni, sizeof(Ether) + sizeof(IP) + sizeof(TCP) + 4 + 4/* option */ + len);
//	} else {
//		packet = ni_alloc(ni, sizeof(Ether) + sizeof(IP) + sizeof(TCP) + len);
//	}
//	
//	if(!packet)
//		return false;
//
//	Ether* ether = (Ether*)(packet->buffer + packet->start);
//	ether->dmac = endian48(arp_get_mac(ni,tcb->dip,tcb->sip));
//	ether->smac = endian48(ni->mac);
//	ether->type = endian16(ETHER_TYPE_IPv4);
//	
//	IP* ip = (IP*)ether->payload;
//	ip->ihl = endian8(5);
//	ip->version = endian8(4);
//	ip->ecn = endian8(0); 
//	ip->dscp = endian8(0);
//
//	ip_id = ip_get_id(ack);
//	ip->id = endian16(ip_id);
//	ip->flags_offset = 0x40;
//	ip->ttl = endian8(IP_TTL);
//	ip->protocol = endian8(IP_PROTOCOL_TCP);
//	ip->source = endian32(tcb->sip);
//	ip->destination = endian32(tcb->dip);
//	
//	TCP* tcp = (TCP*)ip->body;
//	tcp->source = endian16(tcb->sport);
//	tcp->destination = endian16(tcb->dport);
//	tcp->sequence = endian32(tcb->sequence);
//	tcp->acknowledgement = endian32(tcb->acknowledgement);
//	tcp->ns = endian8(0);
//	tcp->reserved = endian8(0);
//	tcp->fin = flags & FIN; 
//	tcp->syn = (flags & SYN) >> 1;
//	tcp->rst = (flags & SYN) >> 2;
//	tcp->psh = (flags & PSH) >> 3;
//	tcp->ack = (flags & ACK) >> 4;
//	tcp->urg = (flags & URG) >> 5;
//	tcp->ece = (flags & ECE) >> 6;
//	tcp->cwr = (flags & CWR) >> 7;
//	tcp->window = endian16(tcb->recv_wnd_max);
//	tcp->urgent = endian16(0);
//	
//	if(syn) {
//		tcp->offset = endian8(7);
//		uint32_t mss_option = endian32(0x020405b4);
//		uint32_t win_option = endian32(0x01030307);
//
//		memcpy(tcp->payload, &mss_option, 4);
//		memcpy((uint8_t*)(tcp->payload) + 4, &win_option, 4);
//
//		tcp_pack(packet, len + 4 + 4);
//	} else {
//		tcp->offset = endian8(5);
//		memcpy((uint8_t*)tcp + tcp->offset * 4, data, len);
//
//		tcp_pack(packet, len);
//	}
//
//	//printf("packet out!!\n");
//	if(ni_output_available(ni)) {
//		//printf("send fail\n");
//		if(ni_output(ni, packet)) {
//
//			if(len != 0) {
//				Segment* segment = gmalloc(sizeof(Segment));
//				if(!segment)
//					printf("seg malloc error\n");
//
//				segment->sequence = tcb->sequence;
//				segment->syn = endian8(syn);
//				segment->ack = endian8(ack);
//				segment->psh = endian8(psh);
//				segment->fin = endian8(fin);
//				segment->len = len;
//				segment->data = gmalloc(len);
//				if(!(segment->data))
//					printf("data mallco error\n");
//
//				segment->timeout = timer_us() + ACK_TIMEOUT;
//
//				memcpy(segment->data, data, len);
//				if(!list_add(tcb->unack_list, segment))
//					printf("list add fail\n");
//			}
//			
//			tcb->sequence += len;
//			return true;
//		} else {
//			ni_free(packet);
//			return false;
//		}
//	} else {
//		ni_free(packet);
//		return false;
//	}
//}

/*
 * iterate tcbs and it's seg unacked list. compare timeout. if timeout is over, resend segment.
 * 
 */
bool tcp_timer(void* context) {
	uint64_t current = timer_us();

	/*
	if(tcbs) {
		MapIterator map_iter;
		map_iterator_init(&map_iter, tcbs);

		while(map_iterator_has_next(&map_iter)) {
			MapEntry* entry = map_iterator_next(&map_iter); 
			TCB* tcb = entry->data;

			if(!tcb->unack_list) 
				continue;

			ListIterator seg_iter;
			list_iterator_init(&seg_iter, tcb->unack_list);

			while(list_iterator_has_next(&seg_iter)) {
				Segment* segment = (Segment*)list_iterator_next(&seg_iter);
				if(segment->timeout < current) {
					// need to resend segment (packet_out())
					
				}
			}
		}
	}
	*/

	if(time_wait_list) {
		ListIterator iter;
		list_iterator_init(&iter, time_wait_list);
		while(list_iterator_has_next(&iter)) {
			TCB* tcb = list_iterator_next(&iter);
			printf("timeout : %d, cur : %d\n", tcb->timeout, current);
			if(tcb->timeout < current) {
				tcb->state = TCP_CLOSED;
				tcb_destroy(tcb);
			list_iterator_remove(&iter);
			}
		}
	}

	if(conn_try_list) {
		ListIterator iter;
		list_iterator_init(&iter, conn_try_list);
		while(list_iterator_has_next(&iter)) {
			Callback* callback = list_iterator_next(&iter);
			
			if(callback->timeout < current) {
				list_iterator_remove(&iter);
				callback->tcb->state = TCP_CLOSED;
				tcb_destroy(callback->tcb);
			} else {
				if(tcp_try_connect(callback->tcb)) 
					list_iterator_remove(&iter);	
			}
		}
	}

	return true;
}

bool tcp_port_alloc0(NetworkInterface* ni, uint32_t addr, uint16_t port) {
	IPv4Interface* interface = ni_ip_get(ni, addr);
	if(!interface->tcp_ports) {
		interface->tcp_ports = set_create(64, set_uint64_hash, set_uint64_equals, ni->pool);
		if(!interface->tcp_ports)
			return false;
	}

	if(set_contains(interface->tcp_ports, (void*)(uintptr_t)port))
		return false;

	return set_put(interface->tcp_ports, (void*)(uintptr_t)port);
}

uint16_t tcp_port_alloc(NetworkInterface* ni, uint32_t addr) {
	IPv4Interface* interface = ni_ip_get(ni, addr);
	if(!interface->tcp_ports) {
		interface->tcp_ports = set_create(64, set_uint64_hash, set_uint64_equals, ni->pool);
		if(!interface->tcp_ports)
			return 0;
	}

	uint16_t port = interface->tcp_next_port;
	if(port < 49152)
		port = 49152;
	
	while(set_contains(interface->tcp_ports, (void*)(uintptr_t)port)) {
		if(++port < 49152)
			port = 49152;
	}	

	if(!set_put(interface->tcp_ports, (void*)(uintptr_t)port))
		return 0;
	
	interface->tcp_next_port = port;
	
	return port;
}

void tcp_port_free(NetworkInterface* ni, uint32_t addr, uint16_t port) {
	IPv4Interface* interface = ni_ip_get(ni, addr);
	if(interface == NULL)
		return;
	
	set_remove(interface->tcp_ports, (void*)(uintptr_t)port);
}

void tcp_pack(Packet* packet, uint16_t tcp_body_len) {
	Ether* ether = (Ether*)(packet->buffer + packet->start);
	IP* ip = (IP*)ether->payload;
	TCP* tcp = (TCP*)ip->body;
	
	uint16_t tcp_len = TCP_LEN + tcp_body_len;
	
	TCP_Pseudo pseudo;
	pseudo.source = ip->source;
	pseudo.destination = ip->destination;
	pseudo.padding = 0;
	pseudo.protocol = ip->protocol;
	pseudo.length = endian16(tcp_len);
	
	tcp->checksum = 0;
	uint32_t sum = (uint16_t)~checksum(&pseudo, sizeof(pseudo)) + (uint16_t)~checksum(tcp, tcp_len);
	while(sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	tcp->checksum = endian16(~sum);
	
	ip_pack(packet, tcp_len);
}
