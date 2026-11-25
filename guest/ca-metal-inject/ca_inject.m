/*
 * libcainject.dylib — instrumentation for the CoreAnimation-on-Metal experiment.
 *
 * Injected into backboardd only (etc/inject.conf). It answers one question and works around one
 * wall:
 *
 *   question: when CoreAnimation's Metal renderer stops at
 *             `Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm,
 *             line 3308`, which resource lookup gave it nil? Every bundle/CFBundle resource lookup
 *             for a Metal-named resource is logged with the bundle path and the answer.
 *   wall:     with a `default.metallib` request that comes back nil from QuartzCore's own bundle,
 *             the lookup returns a placeholder URL under /redwood/share instead — a file of ours,
 *             not Apple's. If CoreAnimation only hands that URL to the MTLDevice, the stub device
 *             decides what happens next; if it opens the file itself, the log says so.
 *
 * The replacement is a URL and nothing else: no Apple code is patched, no Apple file is copied,
 * and every substitution is printed. It exists for the Metal-path experiment only and is off
 * unless a process is started with this library inserted (see install.sh).
 *
 * SPDX-License-Identifier: MIT
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CAI_STUB_PATH "/redwood/share/ca-default.metallib"
#define CAI_MAX_LOGGED 400

static NSURL *gStubURL;
static int gLogCount;

static void
cai_log(const char *format, ...)
{
	char line[1500];
	va_list arguments;
	int used;

	va_start(arguments, format);
	used = vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (used < 0) {
		return;
	}
	if ((size_t)used >= sizeof(line)) {
		used = (int)sizeof(line) - 1;
	}
	if (used > 0 && line[used - 1] != '\n') {
		line[used++] = '\n';
	}
	write(2, line, (size_t)used);
}

static const char *
cai_cstring(NSString *string)
{
	return string != nil ? [string UTF8String] : "(nil)";
}

/* A resource name/type pair that is about Metal shaders: the only lookups this library both logs
 * in full and substitutes for. */
static BOOL
cai_metal_named(NSString *name, NSString *type)
{
	if (name == nil && type == nil) {
		return NO;
	}
	NSString *combined = [[NSString stringWithFormat:@"%@.%@", name != nil ? name : @"",
	                                              type != nil ? type : @""] lowercaseString];

	return [combined containsString:@"metallib"] || [combined containsString:@"pipelinelib"] ||
	       [combined containsString:@"metallib"] || [combined containsString:@".metallib"];
}

static BOOL
cai_quartzcore(NSString *bundlePath)
{
	return bundlePath != nil && [bundlePath containsString:@"QuartzCore"];
}

static void
cai_log_lookup(const char *api, NSString *bundlePath, NSString *name, NSString *type, id result)
{
	if (gLogCount >= CAI_MAX_LOGGED) {
		return;
	}
	gLogCount++;
	cai_log("ca-inject: %s bundle=%s name=%s type=%s -> %s", api, cai_cstring(bundlePath),
	    cai_cstring(name), cai_cstring(type), result != nil ? cai_cstring([result description]) : "nil");
}

/* ------------------------------------------------------------------------------------------------
 * NSBundle lookups.
 * ---------------------------------------------------------------------------------------------- */

typedef id (*cai_url2_fn)(id, SEL, id, id);
typedef id (*cai_url3_fn)(id, SEL, id, id, id);
typedef id (*cai_url4_fn)(id, SEL, id, id, id, id);
typedef id (*cai_path2_fn)(id, SEL, id, id);
typedef id (*cai_path3_fn)(id, SEL, id, id, id);

static cai_url2_fn gURForResource2;
static cai_url3_fn gURForResource3;
static cai_url4_fn gURForResource4;
static cai_path2_fn gPathForResource2;
static cai_path3_fn gPathForResource3;

static id
cai_url_for_resource2(id self, SEL _cmd, id name, id type)
{
	id result = gURForResource2(self, _cmd, name, type);
	NSString *bundlePath = [self bundlePath];

	if (cai_metal_named(name, type) || (result == nil && cai_quartzcore(bundlePath))) {
		cai_log_lookup("NSBundle-URLForResource:withExtension:", bundlePath, name, type, result);
	}
	if (result == nil && cai_metal_named(name, type)) {
		cai_log("ca-inject: substituting %s for the nil URL", CAI_STUB_PATH);
		return gStubURL;
	}
	return result;
}

static id
cai_url_for_resource3(id self, SEL _cmd, id name, id type, id subdirectory)
{
	id result = gURForResource3(self, _cmd, name, type, subdirectory);
	NSString *bundlePath = [self bundlePath];

	if (cai_metal_named(name, type) || (result == nil && cai_quartzcore(bundlePath))) {
		cai_log_lookup("NSBundle-URLForResource:withExtension:subdirectory:", bundlePath, name, type,
		    result);
		if (subdirectory != nil) {
			cai_log("ca-inject:   subdirectory=%s", cai_cstring(subdirectory));
		}
	}
	if (result == nil && cai_metal_named(name, type)) {
		return gStubURL;
	}
	return result;
}

static id
cai_url_for_resource4(id self, SEL _cmd, id name, id type, id subdirectory, id localization)
{
	id result = gURForResource4(self, _cmd, name, type, subdirectory, localization);
	NSString *bundlePath = [self bundlePath];

	if (cai_metal_named(name, type) || (result == nil && cai_quartzcore(bundlePath))) {
		cai_log_lookup("NSBundle-URLForResource:withExtension:subdirectory:localization:", bundlePath,
		    name, type, result);
	}
	if (result == nil && cai_metal_named(name, type)) {
		return gStubURL;
	}
	return result;
}

static id
cai_path_for_resource2(id self, SEL _cmd, id name, id type)
{
	id result = gPathForResource2(self, _cmd, name, type);
	NSString *bundlePath = [self bundlePath];

	if (cai_metal_named(name, type) || (result == nil && cai_quartzcore(bundlePath))) {
		cai_log_lookup("NSBundle-pathForResource:ofType:", bundlePath, name, type, result);
	}
	if (result == nil && cai_metal_named(name, type)) {
		return @CAI_STUB_PATH;
	}
	return result;
}

static id
cai_path_for_resource3(id self, SEL _cmd, id name, id type, id directory)
{
	id result = gPathForResource3(self, _cmd, name, type, directory);
	NSString *bundlePath = [self bundlePath];

	if (cai_metal_named(name, type) || (result == nil && cai_quartzcore(bundlePath))) {
		cai_log_lookup("NSBundle-pathForResource:ofType:inDirectory:", bundlePath, name, type, result);
	}
	if (result == nil && cai_metal_named(name, type)) {
		return @CAI_STUB_PATH;
	}
	return result;
}

static IMP
cai_swizzle(Class cls, const char *selectorName, IMP replacement)
{
	SEL selector = sel_registerName(selectorName);
	Method method = class_getInstanceMethod(cls, selector);

	if (method == NULL) {
		cai_log("ca-inject: NSBundle does not implement -%s on this system", selectorName);
		return NULL;
	}
	return method_setImplementation(method, replacement);
}

static void
cai_install_bundle_hooks(void)
{
	Class bundle = objc_getClass("NSBundle");

	if (bundle == Nil) {
		cai_log("ca-inject: no NSBundle; nothing can be hooked");
		return;
	}
	gURForResource2 = (cai_url2_fn)cai_swizzle(bundle, "URLForResource:withExtension:",
	    (IMP)cai_url_for_resource2);
	gURForResource3 = (cai_url3_fn)cai_swizzle(bundle, "URLForResource:withExtension:subdirectory:",
	    (IMP)cai_url_for_resource3);
	gURForResource4 = (cai_url4_fn)cai_swizzle(
	    bundle, "URLForResource:withExtension:subdirectory:localization:",
	    (IMP)cai_url_for_resource4);
	gPathForResource2 =
	    (cai_path2_fn)cai_swizzle(bundle, "pathForResource:ofType:", (IMP)cai_path_for_resource2);
	gPathForResource3 = (cai_path3_fn)cai_swizzle(bundle, "pathForResource:ofType:inDirectory:",
	    (IMP)cai_path_for_resource3);
	cai_log("ca-inject: NSBundle resource lookups hooked (URL and path, four and three selectors)");
}

/* ------------------------------------------------------------------------------------------------
 * CFBundle lookups: NSBundle is implemented on them, and a C++ renderer may call them directly.
 * DYLD_INTERPOSE makes the replacement win for every image in the process.
 * ---------------------------------------------------------------------------------------------- */

typedef CFURLRef (*cai_cf_url_fn)(CFBundleRef, CFStringRef, CFStringRef, CFStringRef);
typedef CFArrayRef (*cai_cf_urls_of_type_fn)(CFBundleRef, CFStringRef, CFStringRef);
typedef CFURLRef (*cai_cf_url_loc_fn_t)(CFBundleRef, CFStringRef, CFStringRef, CFStringRef, CFStringRef);

/* Resolved with RTLD_NEXT so that this image's own calls reach CoreFoundation rather than the
 * interposed entry (dyld would otherwise send them back here). */
static cai_cf_url_fn gCFBundleCopyResourceURL;
static cai_cf_urls_of_type_fn gCFBundleCopyResourceURLsOfType;
static cai_cf_url_loc_fn_t gCFBundleCopyResourceURLForLocalization;

static void
cai_resolve_cf(void)
{
	gCFBundleCopyResourceURL =
	    (cai_cf_url_fn)dlsym(RTLD_NEXT, "CFBundleCopyResourceURL");
	gCFBundleCopyResourceURLsOfType =
	    (cai_cf_urls_of_type_fn)dlsym(RTLD_NEXT, "CFBundleCopyResourceURLsOfType");
	gCFBundleCopyResourceURLForLocalization =
	    (cai_cf_url_loc_fn_t)dlsym(RTLD_NEXT, "CFBundleCopyResourceURLForLocalization");
	if (gCFBundleCopyResourceURL == NULL) {
		cai_log("ca-inject: CFBundleCopyResourceURL is not resolvable with RTLD_NEXT");
	}
}
typedef CFURLRef (*cai_cf_url_loc_fn)(
    CFBundleRef, CFStringRef, CFStringRef, CFStringRef, CFStringRef);
typedef CFURLRef (*cai_cf_url_dir_fn)(CFURLRef, CFStringRef, CFStringRef, CFStringRef);
typedef CFArrayRef (*cai_cf_urls_fn)(CFBundleRef, CFStringRef, CFStringRef);

static CFURLRef
cai_cf_url(CFBundleRef bundle, CFStringRef name, CFStringRef type, CFStringRef subdirectory)
{
	CFURLRef result = gCFBundleCopyResourceURL != NULL
	    ? gCFBundleCopyResourceURL(bundle, name, type, subdirectory)
	    : NULL;
	CFURLRef bundleURL = bundle != NULL ? CFBundleCopyBundleURL(bundle) : NULL;
	CFStringRef bundlePath = bundleURL != NULL ? CFURLCopyFileSystemPath(bundleURL, kCFURLPOSIXPathStyle) : NULL;
	char bundleBuffer[1024] = "(nil)", nameBuffer[256] = "(nil)", typeBuffer[256] = "(nil)";

	if (bundlePath != NULL) {
		CFStringGetCString(bundlePath, bundleBuffer, sizeof(bundleBuffer), kCFStringEncodingUTF8);
	}
	if (name != NULL) {
		CFStringGetCString(name, nameBuffer, sizeof(nameBuffer), kCFStringEncodingUTF8);
	}
	if (type != NULL) {
		CFStringGetCString(type, typeBuffer, sizeof(typeBuffer), kCFStringEncodingUTF8);
	}
	if (gLogCount < CAI_MAX_LOGGED &&
	    (strstr(typeBuffer, "metallib") != NULL || strstr(nameBuffer, "metallib") != NULL ||
	        (result == NULL && strstr(bundleBuffer, "QuartzCore") != NULL))) {
		gLogCount++;
		cai_log("ca-inject: CFBundleCopyResourceURL bundle=%s name=%s type=%s -> %s", bundleBuffer,
		    nameBuffer, typeBuffer, result != NULL ? "a URL" : "nil");
	}
	if (result == NULL && (strstr(typeBuffer, "metallib") != NULL || strstr(nameBuffer, "metallib") != NULL)) {
		if (gStubURL != NULL) {
			cai_log("ca-inject: substituting %s for the nil CFBundle URL", CAI_STUB_PATH);
			result = CFURLCreateWithFileSystemPath(
			    kCFAllocatorDefault, CFSTR(CAI_STUB_PATH), kCFURLPOSIXPathStyle, false);
		}
	}
	if (bundlePath != NULL) {
		CFRelease(bundlePath);
	}
	if (bundleURL != NULL) {
		CFRelease(bundleURL);
	}
	return result;
}

static CFArrayRef
cai_cf_urls(CFBundleRef bundle, CFStringRef type, CFStringRef subdirectory)
{
	CFArrayRef result = gCFBundleCopyResourceURLsOfType != NULL
	    ? gCFBundleCopyResourceURLsOfType(bundle, type, subdirectory)
	    : NULL;
	char typeBuffer[256] = "(nil)";

	if (type != NULL) {
		CFStringGetCString(type, typeBuffer, sizeof(typeBuffer), kCFStringEncodingUTF8);
	}
	if (gLogCount < CAI_MAX_LOGGED && strstr(typeBuffer, "metallib") != NULL) {
		gLogCount++;
		cai_log("ca-inject: CFBundleCopyResourceURLsOfType type=%s -> %ld URL(s)", typeBuffer,
		    result != NULL ? (long)CFArrayGetCount(result) : -1L);
	}
	return result;
}

static CFURLRef
cai_cf_url_localization(
    CFBundleRef bundle, CFStringRef name, CFStringRef type, CFStringRef subdirectory, CFStringRef localization)
{
	CFURLRef result = gCFBundleCopyResourceURLForLocalization != NULL
	    ? gCFBundleCopyResourceURLForLocalization(bundle, name, type, subdirectory, localization)
	    : NULL;
	char typeBuffer[256] = "(nil)";

	if (type != NULL) {
		CFStringGetCString(type, typeBuffer, sizeof(typeBuffer), kCFStringEncodingUTF8);
	}
	if (gLogCount < CAI_MAX_LOGGED && strstr(typeBuffer, "metallib") != NULL) {
		gLogCount++;
		cai_log("ca-inject: CFBundleCopyResourceURLForLocalization type=%s -> %s", typeBuffer,
		    result != NULL ? "a URL" : "nil");
	}
	if (result == NULL && strstr(typeBuffer, "metallib") != NULL) {
		result = CFURLCreateWithFileSystemPath(
		    kCFAllocatorDefault, CFSTR(CAI_STUB_PATH), kCFURLPOSIXPathStyle, false);
	}
	return result;
}

#define CAI_INTERPOSE(replacement, replacee)                                                       \
	__attribute__((used)) static struct {                                                      \
		const void *replacement;                                                           \
		const void *replacee;                                                              \
	} _cai_interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {              \
		(const void *)(unsigned long)&replacement, (const void *)(unsigned long)&replacee};

CAI_INTERPOSE(cai_cf_url, CFBundleCopyResourceURL)
CAI_INTERPOSE(cai_cf_urls, CFBundleCopyResourceURLsOfType)
CAI_INTERPOSE(cai_cf_url_localization, CFBundleCopyResourceURLForLocalization)

/* A path test on a Metal-named file: logged so that a renderer that checks the file itself rather
 * than asking its bundle is visible too. The answer is not changed (a file under /System really is
 * not there); the log is what decides whether it needs to be. */
typedef BOOL (*cai_file_exists_fn)(id, SEL, id);
static cai_file_exists_fn gFileExists;

static BOOL
cai_file_exists(id self, SEL _cmd, NSString *path)
{
	BOOL result = gFileExists(self, _cmd, path);

	if (path != nil && ([path containsString:@"metallib"] || [path containsString:@"pipelinelib"])) {
		if (gLogCount < CAI_MAX_LOGGED) {
			gLogCount++;
			cai_log("ca-inject: fileExistsAtPath=%s -> %s", cai_cstring(path),
			    result ? "YES" : "NO");
		}
	}
	return result;
}

static void
cai_install_file_hooks(void)
{
	Class manager = objc_getClass("NSFileManager");

	if (manager == Nil) {
		return;
	}
	gFileExists = (cai_file_exists_fn)cai_swizzle(manager, "fileExistsAtPath:", (IMP)cai_file_exists);
}

static void
cai_make_stub_file(void)
{
	int fd;
	static const unsigned char header[64] = { 'M', 'T', 'L', 'B', 1, 2, 0, 0 };

	if (access(CAI_STUB_PATH, F_OK) == 0) {
		cai_log("ca-inject: placeholder already in the image at %s", CAI_STUB_PATH);
		return;
	}
	fd = open(CAI_STUB_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		cai_log("ca-inject: cannot create %s: %s", CAI_STUB_PATH, strerror(errno));
		return;
	}
	(void)write(fd, header, sizeof(header));
	close(fd);
	cai_log("ca-inject: placeholder written at %s (64 bytes)", CAI_STUB_PATH);
}


/* ------------------------------------------------------------------------------------------------
 * Symbol resolution for the crash PCs the guest prints as un-slid addresses.
 *
 * hakios' fault line reports unslid addresses. Objective-C methods in the dyld shared cache keep
 * direct IMPs, so the method that a crash address belongs to can be named from inside the process:
 * take the Metal image's runtime base, subtract it from 0xa7f7c (and other offsets of interest),
 * then walk every class of the process looking for that IMP. No Apple symbols file is needed.
 * Printed once, before the process starts driving CoreAnimation.
 * ---------------------------------------------------------------------------------------------- */

static intptr_t gMetalBase;
static intptr_t gCacheSlide;

static void
cai_scan_imp(intptr_t target, const char *what)
{
	unsigned classCount = 0;
	Class *classes = objc_copyClassList(&classCount);
	int found = 0;

	for (unsigned i = 0; i < classCount && classes != NULL; i++) {
		unsigned methodCount = 0;
		Method *methods = class_copyMethodList(classes[i], &methodCount);

		for (unsigned j = 0; j < methodCount; j++) {
			IMP imp = method_getImplementation(methods[j]);

			if ((intptr_t)imp == target) {
				cai_log("ca-inject: %s is -[%s %s]", what, class_getName(classes[i]),
				    sel_getName(method_getName(methods[j])));
				found = 1;
			}
		}
		free(methods);
	}
	free(classes);
	if (!found) {
		cai_log("ca-inject: %s is not an Objective-C method entry of a loaded class", what);
	}
}

static void
cai_resolve_crash_pcs(void)
{
	void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_NOLOAD);
	void *symbol;

	if (metal == NULL) {
		cai_log("ca-inject: Metal is not loaded; crash-PC resolution skipped");
		return;
	}
	symbol = dlsym(metal, "MTLCreateSystemDefaultDevice");
	if (symbol == NULL) {
		cai_log("ca-inject: MTLCreateSystemDefaultDevice is not exported; crash-PC resolution skipped");
		return;
	}
	{
		Dl_info info;
		if (dladdr(symbol, &info) == 0 || info.dli_fbase == NULL) {
			return;
		}
		gMetalBase = (intptr_t)info.dli_fbase;
		gCacheSlide = gMetalBase - 0x181a12000; /* the unslid base of Metal.framework 16.5 */
	}
	cai_log("ca-inject: Metal image base %p; resolving crash offsets", (void *)gMetalBase);
	cai_scan_imp(gMetalBase + 0xa7f7c, "Metal+0xa7f7c");
	cai_scan_imp(gMetalBase + 0x2aa3c, "Metal+0x2aa3c");
}


/* ------------------------------------------------------------------------------------------------
 * Fault census: log the registers at a crash, in particular x1 — at the entry of an Objective-C
 * method that is where objc_msgSend leaves the selector. It is how the experiment names a
 * selector that runs inside Apple's code and dies before any of our logs. The previous handler
 * (hakios' own fault line, installed first) is chained, so its output is not lost.
 * ---------------------------------------------------------------------------------------------- */

static struct sigaction gPreviousFault;
static int gFaultInstalled;

static void
cai_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uint64_t pc = 0, lr = 0, x0 = 0, x1 = 0, x2 = 0, x3 = 0, x8 = 0;
	char line[512];
	int used;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = context->uc_mcontext->__ss.__pc;
		lr = context->uc_mcontext->__ss.__lr;
		x0 = context->uc_mcontext->__ss.__x[0];
		x1 = context->uc_mcontext->__ss.__x[1];
		x2 = context->uc_mcontext->__ss.__x[2];
		x3 = context->uc_mcontext->__ss.__x[3];
		x8 = context->uc_mcontext->__ss.__x[8];
	}
	used = snprintf(line, sizeof(line),
	    "ca-inject: fault %d at %p: pc=%#llx lr=%#llx (unslid pc=%#llx lr=%#llx) "
	    "x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx\n",
	    signal_number, info != NULL ? info->si_addr : NULL, (unsigned long long)pc,
	    (unsigned long long)lr, (unsigned long long)(pc - gCacheSlide),
	    (unsigned long long)(lr - gCacheSlide), (unsigned long long)x0, (unsigned long long)x1,
	    (unsigned long long)x2, (unsigned long long)x3, (unsigned long long)x8);
	if (used > 0) {
		write(2, line, (size_t)(used < (int)sizeof(line) ? used : (int)sizeof(line) - 1));
	}
	/* a short frame-pointer walk; the chain in hakios' line is already useful but ours can show
	 * the heap/block frames too. Only pointers that look like a stack are read. */
	{
		uintptr_t fp = context != NULL && context->uc_mcontext != NULL
		    ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;
		char frames[12 * 20 + 1];
		size_t used = 0;

		frames[0] = 0;
		for (unsigned i = 0; i < 12 && fp >= 0x100000000ULL && fp < 0x00007fffffffffffULL &&
		     (fp & 7) == 0; i++) {
			uintptr_t next = ((uintptr_t *)fp)[0];
			uintptr_t ret = ((uintptr_t *)fp)[1];
			used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#llx",
			    (unsigned long long)(ret - gCacheSlide));
			if (next <= fp) {
				break;
			}
			fp = next;
		}
		if (frames[0] != 0) {
			used = snprintf(line, sizeof(line), "ca-inject:   frames (unslid)%s\n", frames);
			if (used > 0) {
				write(2, line, (size_t)(used < (int)sizeof(line) ? used : (int)sizeof(line) - 1));
			}
		}
	}
	if (x1 != 0 && sel_isMapped((SEL)(uintptr_t)x1)) {
		const char *name = sel_getName((SEL)(uintptr_t)x1);

		if (name != NULL) {
			used = snprintf(line, sizeof(line), "ca-inject:   the selector in x1 is -%s\n", name);
			if (used > 0) {
				write(2, line, (size_t)(used < (int)sizeof(line) ? used : (int)sizeof(line) - 1));
			}
		}
	}
	if (gFaultInstalled && gPreviousFault.sa_flags != 0) {
		if (gPreviousFault.sa_flags & SA_SIGINFO) {
			if (gPreviousFault.sa_sigaction != NULL &&
			    gPreviousFault.sa_sigaction != (void *)SIG_DFL &&
			    gPreviousFault.sa_sigaction != (void *)SIG_IGN) {
				gPreviousFault.sa_sigaction(signal_number, info, context_in);
				return;
			}
		} else if (gPreviousFault.sa_handler != SIG_DFL &&
		           gPreviousFault.sa_handler != SIG_IGN) {
			gPreviousFault.sa_handler(signal_number);
			return;
		}
	}
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
cai_install_fault_census(void)
{
	static const int signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP };
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = cai_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	/* one handler is enough to report the first fault of a process; chain whatever was there */
	if (sigaction(SIGSEGV, NULL, &gPreviousFault) == 0) {
		gFaultInstalled = 1;
	}
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	cai_log("ca-inject: fault census installed (chained over the previous SIGSEGV handler)");
}


/* ------------------------------------------------------------------------------------------------
 * Diagnostic shim for one Apple implementation: CA calls
 * -[MTLPipelineLibrary setDisableRunTimeCompilation:] on the placeholder pipeline library our
 * plug-in returns. The plug-in subclasses _MTLPipelineLibrary and inherits Metal's real
 * implementation, which dereferences the real object and faults at 0x10. Until the plug-in
 * implements the selector (it will, in the same series), this replaces the base implementation in
 * this process with a no-op that only records the call. It is a shim, not the fix: the plug-in
 * is where the real stub belongs.
 * ---------------------------------------------------------------------------------------------- */

static void
cai_noop_set_disable_runtime(id self, SEL _cmd, BOOL value)
{
	static int said;

	(void)self;
	(void)_cmd;
	if (!said) {
		said = 1;
		cai_log("ca-inject: shim: -setDisableRunTimeCompilation:%d on the placeholder pipeline "
		        "library (base implementation replaced)", value ? 1 : 0);
	}
}

static void
cai_patch_pipeline_library(void)
{
	Class cls = objc_getClass("_MTLPipelineLibrary");
	SEL selector = sel_registerName("setDisableRunTimeCompilation:");
	Method method;

	if (cls == Nil) {
		cai_log("ca-inject: shim: _MTLPipelineLibrary is not loaded; nothing patched");
		return;
	}
	method = class_getInstanceMethod(cls, selector);
	if (method == NULL) {
		cai_log("ca-inject: shim: _MTLPipelineLibrary has no -setDisableRunTimeCompilation:");
		return;
	}
	method_setImplementation(method, (IMP)cai_noop_set_disable_runtime);
	cai_log("ca-inject: shim: _MTLPipelineLibrary -setDisableRunTimeCompilation: replaced for this process");
}


/* ------------------------------------------------------------------------------------------------
 * Diagnostic: skip the dealloc of Metal's private base classes for our stub objects.
 *
 * The renderer's first frame is encoded (six empty passes, blit copies) and then the process faults
 * in libdispatch from inside Metal (+0x25930), with a null dispatch object. That is the shape of a
 * real `_MTL* -dealloc` running on an object whose Metal-side ivars our stub never set. To test the
 * hypothesis without touching the plug-in, the base dealloc implementations are replaced with a
 * logger here: the plug-in's own dealloc still frees its state and calls the base's dealloc, which
 * now only logs and leaks the (tiny, per-process) Metal-side allocation.
 * ---------------------------------------------------------------------------------------------- */

static void
cai_base_dealloc(id self, SEL _cmd)
{
	static int said;
	Class cls = object_getClass(self);

	if (!said) {
		said = 1;
		cai_log("ca-inject: dealloc shim: first base dealloc skipped: -[%s dealloc]",
		    cls != Nil ? class_getName(cls) : "?");
	}
}

static void
cai_patch_base_deallocs(void)
{
	static const char *const classes[] = {
		"_MTLCommandBuffer", "_MTLCommandEncoder", "_MTLBlitEncoder", "_MTLCommandQueue",
		"_MTLLibrary", "_MTLFunction", "_MTLRenderPipelineState", "_MTLDepthStencilState",
		"_MTLSamplerState", "_MTLPipelineLibrary", "_MTLResource"
	};
	SEL deallocSelector = sel_registerName("dealloc");
	int patched = 0;

	for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		Class cls = objc_getClass(classes[i]);
		Method method;

		if (cls == Nil) {
			continue;
		}
		method = class_getInstanceMethod(cls, deallocSelector);
		if (method == NULL || method_getImplementation(method) == (IMP)cai_base_dealloc) {
			continue;
		}
		method_setImplementation(method, (IMP)cai_base_dealloc);
		patched++;
	}
	cai_log("ca-inject: dealloc shim: %d Metal base classes' -dealloc replaced", patched);
}

/* The diagnostic shims (setDisableRunTimeCompilation: and the Metal base-class dealloc replacement)
 * are enabled only while the image carries /redwood/etc/ca-inject-shims. Once the plug-in implements
 * both itself, remove the file and the same library still logs the resource lookups, the crash
 * registers and the fault frames. */
static int
cai_shims_enabled(void)
{
	return access("/redwood/etc/ca-inject-shims", F_OK) == 0;
}


/* ------------------------------------------------------------------------------------------------
 * The pipeline descriptor's function properties.
 *
 * Once CoreAnimation has a placeholder specialized MTLFunction from the plug-in, it puts it into a
 * real MTLRenderPipelineDescriptor and Apple's implementation of -setVertexFunction: /
 * -setFragmentFunction: may inspect the object it is given. That is the next fault candidate
 * (Metal.framework +0x…, called from QuartzCore's MetalContext). Store the function as an
 * associated object instead, and answer the matching getter from the same slot: the plug-in's
 * -newRenderPipelineStateWithDescriptor:error: reads the functions back with the getters, so the
 * log still names them, and no Apple code has to accept our placeholder object.
 * ---------------------------------------------------------------------------------------------- */

static void
cai_descriptor_set_function(id self, SEL _cmd, id function)
{
	static int said;
	static char last[128];
	const char *name = sel_getName(_cmd);

	objc_setAssociatedObject(self, (const void *)_cmd, function, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	if (!said) {
		said = 1;
		cai_log("ca-inject: pipeline descriptor %s hooked (function stored, not Apple-handled)",
		    name);
	}
	(void)last;
}

static id
cai_descriptor_get_function(id self, SEL _cmd)
{
	const char *getter = sel_getName(_cmd);
	char setter[160];

	snprintf(setter, sizeof(setter), "set%c%s:", getter[0] >= 'a' && getter[0] <= 'z'
	    ? getter[0] - 'a' + 'A' : getter[0], getter + 1);
	return objc_getAssociatedObject(self, (const void *)sel_registerName(setter));
}

static void
cai_patch_pipeline_descriptor(void)
{
	static const struct {
		const char *setter;
		const char *getter;
	} properties[] = {
		{ "setVertexFunction:", "vertexFunction" },
		{ "setFragmentFunction:", "fragmentFunction" },
	};
	int patched = 0;
	unsigned class_count = 0;
	Class *classes = objc_copyClassList(&class_count);

	/* the public MTLRenderPipelineDescriptor may not carry the implementations: hook every class
	 * whose name mentions RenderPipelineDescriptor and that actually implements both accessors. */
	for (unsigned c = 0; classes != NULL && c < class_count; c++) {
		const char *name = class_getName(classes[c]);

		if (strstr(name, "RenderPipelineDescriptor") == NULL) {
			continue;
		}
		for (unsigned i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
			Method setter = class_getInstanceMethod(classes[c], sel_registerName(properties[i].setter));
			Method getter = class_getInstanceMethod(classes[c], sel_registerName(properties[i].getter));

			if (setter == NULL || getter == NULL) {
				continue;
			}
			method_setImplementation(setter, (IMP)cai_descriptor_set_function);
			method_setImplementation(getter, (IMP)cai_descriptor_get_function);
			patched++;
			cai_log("ca-inject: %s %s/%s hooked", name, properties[i].setter, properties[i].getter);
		}
	}
	free(classes);
	cai_log("ca-inject: render pipeline descriptor function accessors hooked: %d", patched);
}

__attribute__((constructor)) static void
cai_load(void)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

	gStubURL = [[NSURL alloc] initFileURLWithPath:@CAI_STUB_PATH];
	cai_log("ca-inject: loaded as pid %d (%s)", getpid(), getprogname() != NULL ? getprogname() : "?");
	cai_make_stub_file();
	cai_resolve_cf();
	cai_install_bundle_hooks();
	cai_resolve_crash_pcs();
	cai_patch_pipeline_descriptor();
	if (cai_shims_enabled()) {
		cai_patch_pipeline_library();
		cai_patch_base_deallocs();
	} else {
		cai_log("ca-inject: diagnostic shims off (no /redwood/etc/ca-inject-shims)");
	}
	cai_install_fault_census();
	cai_install_file_hooks();
	[pool drain];
}
