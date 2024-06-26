#include <string.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

char *blocked_url;

typedef struct result {
	int id;
	bool blocked;
} result;

void dump(unsigned char* buf, int size) {
	int i;
	printf("\n");
	for (i = 0; i < size; i++) {
		if (i != 0 && i % 16 == 0)
			printf("\n");
		printf("%02X ", buf[i]);
	}
	printf("\n");
}

bool match(int blocked_l, int host_l, unsigned char *host){
	// original logic
	/*
	if (blocked_l == host_l && strncmp(blocked_url, host, blocked_l) == 0){
		return true;
	}
	return false;
	*/
	
	// new logic
	
	for(int i = 0; i <= host_l; i++){
		if (i == host_l || host[host_l - i - 1] == '.'){
			unsigned char *host_chk = &host[host_l - i];
			// if host is abc.def.qwe.rty
			// it iterates [rty, qwe.rty, def.qwe.rty, abc.def.qwe.rty]
			// this way, we can block "something.test.gilgil.net" with block keyword "test.gilgil.net" which should be blocked,
			// and pass through something like "nottest.gilgil.net" which shouldn't be blocked
			
			if (i == blocked_l && strncmp(blocked_url, (const char*)host_chk, blocked_l) == 0)
				return true;
		}
	}
	
	return false;
}

/* returns true if packet is blocked */
static result print_pkt (struct nfq_data *tb)
{
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	u_int32_t mark,ifi; 
	int ret;
	int b;
	unsigned char *data;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
		printf("hw_protocol=0x%04x hook=%u id=%u ",
			ntohs(ph->hw_protocol), ph->hook, id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);

		printf("hw_src_addr=");
		for (i = 0; i < hlen-1; i++)
			printf("%02x:", hwph->hw_addr[i]);
		printf("%02x ", hwph->hw_addr[hlen-1]);
	}

	mark = nfq_get_nfmark(tb);
	if (mark)
		printf("mark=%u ", mark);

	ifi = nfq_get_indev(tb);
	if (ifi)
 		printf("indev=%u ", ifi);
 		
	ifi = nfq_get_outdev(tb);
	if (ifi)
		printf("outdev=%u ", ifi);
	ifi = nfq_get_physindev(tb);
	if (ifi)
		printf("physindev=%u ", ifi);

	ifi = nfq_get_physoutdev(tb);
	if (ifi)
		printf("physoutdev=%u ", ifi);

	ret = nfq_get_payload(tb, &data);
	
	result res = {id, false};
	
	if (ret >= 0){
		printf("payload_len=%d ", ret);
		
		// dump(data, ret);
		
		int blocked_l = strlen(blocked_url);
		unsigned char *host = NULL;
		
		for(int i = 0; i < ret - 6 - blocked_l; i++){
			if (strncmp("Host: ", (const char*)&data[i], 6) == 0){
				host = &data[i + 6];
				break;
			}
		}
		
		if (host != NULL){
			int host_l = 0;
			for(int j = 0; j < ret - 2; j++){
				if (strncmp("\r\n", (const char*)&host[j], 2) == 0){
					host_l = j;
					break;
				}
			}
			
			if (match(blocked_l, host_l, host)){
				printf("\nPacket id: %d BLOCKED. ", id);
				res.blocked = true;
			}
		}
	}

	return res;
}


static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *dat)
{
	result res = print_pkt(nfa);
	return nfq_set_verdict(qh, res.id, res.blocked ? NF_DROP : NF_ACCEPT, 0, NULL);
}

int main(int argc, char **argv)
{
	if(argc != 2){
		printf("syntax : sudo ./netfilter-test <host>\n");
		printf("sample : sudo ./netfilter-test test.gilgil.net\n");
		exit(1);
	}
	blocked_url = argv[1];
	
	system("sudo iptables -F");
	system("sudo iptables -A OUTPUT -j NFQUEUE --queue-num 0");
	system("sudo iptables -A INPUT -j NFQUEUE --queue-num 0");


	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	struct nfnl_handle *nh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));

	printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	printf("binding this socket to queue '0'\n");
	qh = nfq_create_queue(h,  0, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}
		/* if your application is too slow to digest the packets that
		 * are sent from kernel-space, the socket buffer that we use
		 * to enqueue packets may fill up returning ENOBUFS. Depending
		 * on your application, this error may be ignored. Please, see
		 * the doxygen documentation of this library on how to improve
		 * this situation.
		 */
		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	exit(0);
}

