/*
 * hostinfo_probe: call host_info() from a plain guest executable, not from an
 * extension.  It separates "the extension is special" from "host port / MIG is
 * broken for everyone".
 *
 *   hostinfo: mach_host_self port=%u valid=%d
 *   hostinfo: empty send kr=%d
 *   hostinfo: OK ncpu=%d mem=%llu
 *   hostinfo: BLOCKED after 5s
 *   hostinfo: KR=%d
 *
 * The worker is deliberately not joined: if host_info blocks, the main thread
 * prints BLOCKED after the five-second semaphore and exits.
 *
 * Build (from a userland tree that has the iPhoneOS16.5 SDK):
 *   $DYN -o obj/hostinfo_probe src/hostinfo_probe.c
 *   cp obj/hostinfo_probe root/bin/
 * Run from rc:
 *   [ -x /bin/hostinfo_probe ] && /bin/hostinfo_probe >>/dev/console 2>&1
 */
#include <dispatch/dispatch.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct probe_result {
	kern_return_t kr;
	host_basic_info_data_t info;
	mach_msg_type_number_t count;
	dispatch_semaphore_t done;
};

static void *
host_info_worker(void *arg)
{
	struct probe_result *r = arg;

	r->count = HOST_BASIC_INFO_COUNT;
	memset(&r->info, 0, sizeof(r->info));
	r->kr = host_info(mach_host_self(), HOST_BASIC_INFO,
	    (host_info_t)&r->info, &r->count);
	dispatch_semaphore_signal(r->done);
	return NULL;
}

int
main(void)
{
	mach_port_t host = mach_host_self();
	dispatch_semaphore_t done;
	struct probe_result r;
	pthread_t thread;
	long waited;
	mach_msg_return_t mr;
	mach_msg_header_t msg;
	int valid = MACH_PORT_VALID(host);

	printf("hostinfo: mach_host_self port=%u valid=%d\n", host, valid);
	fflush(stdout);
	if (!valid) {
		return 1;
	}

	/* Control 1: send a header-only message to the host port and wait for
	 * the send itself to return.  No reply right is supplied. */
	memset(&msg, 0, sizeof(msg));
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_remote_port = host;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = 0;
	msg.msgh_size = sizeof(msg);
	mr = mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    sizeof(msg), 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
	printf("hostinfo: empty send kr=%d\n", mr);
	fflush(stdout);

	/* Control 2: host_info(HOST_BASIC_INFO) on a worker, max 5 s. */
	done = dispatch_semaphore_create(0);
	memset(&r, 0, sizeof(r));
	r.done = done;
	if (pthread_create(&thread, NULL, host_info_worker, &r) != 0) {
		printf("hostinfo: KR=%d\n", KERN_FAILURE);
		return 2;
	}
	waited = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
	    5 * NSEC_PER_SEC));
	if (waited != 0) {
		printf("hostinfo: BLOCKED after 5s\n");
		fflush(stdout);
		return 3;
	}
	if (r.kr != KERN_SUCCESS) {
		printf("hostinfo: KR=%d\n", r.kr);
	} else {
		printf("hostinfo: OK ncpu=%d mem=%llu\n",
		    (int)r.info.avail_cpus, (unsigned long long)r.info.max_mem);
	}
	fflush(stdout);
	return 0;
}
