/* netcfg: give en0 an address and a default route, without a DHCP client.
 *
 * The machine's own way of getting an address is configd's IPConfiguration plugin, which answers
 * net_test's DHCP request -- but in this image configd starts and never registers that service
 * ("net_test: dhcp: configd's IPConfiguration is not there"), so the interface stays unconfigured.
 * This does by hand what the plugin would: 10.0.2.15/24 on en0, a default route through 10.0.2.2.
 * Those are QEMU's user-mode network defaults (slirp: guest 10.0.2.15, gateway 10.0.2.2).
 *
 *   netcfg [ifname] [address] [gateway]
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/types.h>

/* The routing socket's own header (net/route.h) is a private one on iOS and is not in the SDK, so
 * the few pieces this program needs are here. The layout is Darwin's rt_msghdr, metrics included:
 * rtm_inits says which metrics are valid, and this program sets none of them. */
#define RTM_VERSION	5
#define RTM_ADD		0x1
#define RTA_DST		0x1
#define RTA_GATEWAY	0x2
#define RTA_NETMASK	0x4
#define RTF_UP		0x1
#define RTF_GATEWAY	0x2
#define RTF_STATIC	0x800

struct rt_metrics {
	u_int32_t rmx_locks, rmx_mtu, rmx_hopcount, rmx_expire, rmx_recvpipe, rmx_sendpipe;
	u_int32_t rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent, rmx_state, rmx_filler[3];
};

struct rt_msghdr {
	u_short	rtm_msglen;
	u_char	rtm_version;
	u_char	rtm_type;
	u_short	rtm_index;
	int	rtm_flags;
	int	rtm_addrs;
	pid_t	rtm_pid;
	int	rtm_seq;
	int	rtm_errno;
	int	rtm_use;
	u_int32_t rtm_inits;
	struct rt_metrics rtm_rmx;
};
#include <unistd.h>
#include <stdlib.h>

static int
set_address(int s, const char *ifname, const char *address, const char *netmask)
{
	struct ifaliasreq ifra;

	memset(&ifra, 0, sizeof(ifra));
	strncpy(ifra.ifra_name, ifname, sizeof(ifra.ifra_name) - 1);

	struct sockaddr_in *sin = (struct sockaddr_in *)&ifra.ifra_addr;

	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, address, &sin->sin_addr) != 1)
		return 1;

	sin = (struct sockaddr_in *)&ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	if (inet_pton(AF_INET, netmask, &sin->sin_addr) != 1)
		return 1;

	if (ioctl(s, SIOCAIFADDR, &ifra) < 0) {
		perror("netcfg: SIOCAIFADDR");
		return 1;
	}
	return 0;
}

static int
set_up(int s, const char *ifname)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		perror("netcfg: SIOCGIFFLAGS");
		return 1;
	}
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		perror("netcfg: SIOCSIFFLAGS");
		return 1;
	}
	return 0;
}

static int
add_default_route(const char *gateway)
{
	struct {
		struct rt_msghdr hdr;
		struct sockaddr_in dst;
		struct sockaddr_in gw;
		struct sockaddr_in mask;
	} msg;
	int s, len;

	memset(&msg, 0, sizeof(msg));
	msg.hdr.rtm_msglen = sizeof(msg);
	msg.hdr.rtm_version = RTM_VERSION;
	msg.hdr.rtm_type = RTM_ADD;
	msg.hdr.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	msg.hdr.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	msg.hdr.rtm_seq = 1;

	msg.dst.sin_len = sizeof(msg.dst);
	msg.dst.sin_family = AF_INET;		/* 0.0.0.0: the default route */
	msg.mask.sin_len = sizeof(msg.mask);
	msg.mask.sin_family = AF_INET;		/* netmask 0.0.0.0 */
	msg.gw.sin_len = sizeof(msg.gw);
	msg.gw.sin_family = AF_INET;
	if (inet_pton(AF_INET, gateway, &msg.gw.sin_addr) != 1)
		return 1;

	s = socket(AF_ROUTE, SOCK_RAW, 0);
	if (s < 0) {
		perror("netcfg: AF_ROUTE socket");
		return 1;
	}
	len = (int)sizeof(msg);
	if (write(s, &msg, len) < 0) {
		perror("netcfg: RTM_ADD");
		close(s);
		return 1;
	}
	close(s);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *ifname = argc > 1 ? argv[1] : "en0";
	const char *address = argc > 2 ? argv[2] : "10.0.2.15";
	const char *gateway = argc > 3 ? argv[3] : "10.0.2.2";
	int s;
	int failed = 0;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("netcfg: AF_INET socket");
		return 1;
	}
	failed |= set_address(s, ifname, address, "255.255.255.0");
	failed |= set_up(s, ifname);
	close(s);
	failed |= add_default_route(gateway);

	printf("netcfg: %s %s, default route through %s%s\n", ifname, address, gateway,
	    failed ? " -- FAILED" : "");
	return failed;
}
