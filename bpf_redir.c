#include <stdint.h>
#include <stdio.h>

#include <linux/bpf.h>
#include <sys/socket.h>

#include "bpf_sockops.h"

__section("sk_msg")
int bpf_redir(struct sk_msg_md *msg)
{
	__u64 flags = BPF_F_INGRESS;
	struct sock_key key = {};

	sk_msg_extract4_key(msg, &key);
	// See whether the source or destination IP is local host
	if (key.dip4 == loopback_ip || key.sip4 == loopback_ip ) {
		// See whether the source or destination port is 10000
		if (key.dport == bpf_htons(SERVER_PORT) || key.sport == bpf_htons(SERVER_PORT)) {
			//int len1 = (__u64)msg->data_end - (__u64)msg->data;
	                //printk("<<< redir_proxy port %d --> %d (%d)\n", key.sport, key.dport, len1);
			msg_redirect_hash(msg, &sock_ops_map, &key, flags);
		}
	}

	return SK_PASS;
}

BPF_LICENSE("GPL");
int _version __section("version") = 1;
