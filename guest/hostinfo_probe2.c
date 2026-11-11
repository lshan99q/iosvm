/*
 * hostinfo_probe2: locate the exact step that kills the guest, not just "blocked".
 *
 * Order: mach_host_self -> host_info(HOST_BASIC_INFO) -> host_info(HOST_LOAD_INFO)
 * -> host_statistics64 -> header-only mach_msg to the host port (no reply).
 * Every step prints "before"/"after" and fflush() so the last line names the
 * failing call.  The empty send is deliberately last: the first probe died after
 * the mach_host_self line, so the send was the likely crash point.
 */
#include <mach/host_info.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
line(const char *s)
{
	printf("probe2: %s\n", s);
	fflush(stdout);
}

int
main(void)
{
	mach_port_t hp;
	kern_return_t kr;
	mach_msg_return_t mr;

	line("start");
	line("before mach_host_self");
	hp = mach_host_self();
	printf("probe2: after mach_host_self port=%u valid=%d\n",
	    (unsigned)hp, MACH_PORT_VALID(hp));
	fflush(stdout);
	if (!MACH_PORT_VALID(hp)) {
		line("invalid host port, stop");
		return 1;
	}

	{
		host_basic_info_data_t bi;
		mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;

		memset(&bi, 0, sizeof(bi));
		line("before host_info(HOST_BASIC_INFO)");
		kr = host_info(hp, HOST_BASIC_INFO, (host_info_t)&bi, &count);
		printf("probe2: after host_info basic kr=%d count=%u ncpu=%d memory=%llu max_mem=%llu\n",
		    (int)kr, (unsigned)count, (int)bi.avail_cpus,
		    (unsigned long long)bi.memory_size,
		    (unsigned long long)bi.max_mem);
		fflush(stdout);
	}

	{
		host_load_info_data_t li;
		mach_msg_type_number_t count = HOST_LOAD_INFO_COUNT;

		memset(&li, 0, sizeof(li));
		line("before host_info(HOST_LOAD_INFO)");
		kr = host_info(hp, HOST_LOAD_INFO, (host_info_t)&li, &count);
		printf("probe2: after host_info load kr=%d count=%u avenrun0=%d\n",
		    (int)kr, (unsigned)count, (int)li.avenrun[0]);
		fflush(stdout);
	}

	{
		vm_statistics64_data_t si;
		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

		memset(&si, 0, sizeof(si));
		line("before host_statistics64");
		kr = host_statistics64(hp, HOST_VM_INFO64, (host_info64_t)&si, &count);
		printf("probe2: after host_statistics64 kr=%d count=%u free=%llu\n",
		    (int)kr, (unsigned)count, (unsigned long long)si.free_count);
		fflush(stdout);
	}

	{
		mach_msg_header_t msg;

		memset(&msg, 0, sizeof(msg));
		msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		msg.msgh_remote_port = hp;
		msg.msgh_local_port = MACH_PORT_NULL;
		msg.msgh_id = 0;
		msg.msgh_size = sizeof(msg);
		line("before empty send (header only, no reply)");
		mr = mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
		    sizeof(msg), 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
		printf("probe2: after empty send kr=%d\n", (int)mr);
		fflush(stdout);
	}

	line("done");
	return 0;
}
