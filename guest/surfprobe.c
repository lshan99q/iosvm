
/*
 * Twelfth stage, P1''''': what a surface that no texture bind names holds.  The kernel's own log says
 * SpringBoard makes a 312x202 'LA08' overlay (the text) and a 184x124 BGRA surface (a widget
 * snapshot), and the render server's texture binds name neither of them.  Whether those surfaces
 * hold pixels at all is what separates "the text was never drawn" from "it was drawn and never
 * composited", and this process is the one that made them, so it can look them up itself:
 * boot-arg surfsample=4 (and surfsample_b=3) names them, and a thread of ours reads each one every
 * five seconds -- five row hashes (the same FNV the glwire probes use), how much of the sampling is
 * not zero, and the size/stride/format the client sees.
 */
static unsigned surface_sample_ids[2];
static int surface_sample_count;

static uint32_t
sample_row_hash(const unsigned char *row, size_t used)
{
	uint32_t hash = 2166136261u;

	if (row == NULL || used < 4) {
		return hash;
	}
	for (int i = 0; i < 5; i++) {
		size_t pixel = ((used / 4) - 1) * (size_t)i / 4 * 4;

		for (int b = 0; b < 4; b++) {
			hash = (hash ^ row[pixel + (size_t)b]) * 16777619u;
		}
	}
	return hash;
}

static void
sample_one_surface(unsigned sid)
{
	IOSurfaceRef surface = IOSurfaceLookup((IOSurfaceID)sid);
	const unsigned char *base;
	size_t row, used;
	uint32_t hash[5];
	unsigned w, h, nonzero = 0, samples = 0;

	if (surface == NULL) {
		say("surfprobe: id=%u not there (pid %d)\n", sid, (int)getpid());
		return;
	}
	base = IOSurfaceGetBaseAddress(surface);
	row = IOSurfaceGetBytesPerRow(surface);
	w = (unsigned)IOSurfaceGetWidth(surface);
	h = (unsigned)IOSurfaceGetHeight(surface);
	used = (size_t)w * (size_t)IOSurfaceGetBytesPerElement(surface);
	if (used > row) {
		used = row;
	}
	if (base == NULL || row == 0 || w == 0 || h == 0 || used < 4) {
		say("surfprobe: id=%u %ux%u bpr %zu: no pixels to read (pid %d)\n", sid, w, h, row, (int)getpid());
		IOSurfaceRelease(surface);
		return;
	}
	for (int i = 0; i < 5; i++) {
		h > 0 ? (void)0 : (void)0;
		hash[i] = sample_row_hash(base + row * (size_t)((h - 1) * (unsigned)i / 4), used);
	}
	for (unsigned y = 0; y < h; y += 16) {
		const unsigned char *line = base + row * (size_t)y;

		for (size_t x = 0; x < used; x += 64) {
			nonzero += line[x] != 0;
			samples++;
		}
	}
	say("surfprobe: id=%u %ux%u bpr %zu bpe %zu rows %08x %08x %08x %08x %08x, %u of %u sampled bytes "
	    "not zero (pid %d)\n", sid, w, h, row, IOSurfaceGetBytesPerElement(surface), hash[0], hash[1], hash[2],
	    hash[3], hash[4], nonzero, samples, (int)getpid());
	IOSurfaceRelease(surface);
}

static void *
surface_sampler(void *unused)
{
	(void)unused;
	for (;;) {
		sleep(5);
		for (int i = 0; i < surface_sample_count; i++) {
			sample_one_surface(surface_sample_ids[i]);
		}
	}
	return NULL;
}

static void
start_surface_sampler(void)
{
	char arguments[512];
	size_t size = sizeof(arguments) - 1;
	const char *a, *b;
	pthread_t thread;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	a = boot_argument_value(arguments, "surfsample");
	b = boot_argument_value(arguments, "surfsample_b");
	if (a != NULL && *a != 0) {
		surface_sample_ids[surface_sample_count++] = (unsigned)strtoul(a, NULL, 10);
	}
	if (b != NULL && *b != 0 && surface_sample_count < 2) {
		surface_sample_ids[surface_sample_count++] = (unsigned)strtoul(b, NULL, 10);
	}
	if (surface_sample_count == 0) {
		return;
	}
	if (pthread_create(&thread, NULL, surface_sampler, NULL) == 0) {
		pthread_detach(thread);
		say("surfprobe: sampling surface %u (and %u) every five seconds, pid %d\n", surface_sample_ids[0],
		    surface_sample_count > 1 ? surface_sample_ids[1] : 0, (int)getpid());
	}
}
