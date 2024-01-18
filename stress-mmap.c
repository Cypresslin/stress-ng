/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2024 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-arch.h"
#include "core-builtin.h"
#include "core-madvise.h"
#include "core-mincore.h"
#include "core-mmap.h"
#include "core-out-of-memory.h"

#if defined(HAVE_SYS_PRCTL_H)
#include <sys/prctl.h>
#endif

#if defined(__NR_mmap2)
#define HAVE_MMAP2
#endif

#define MIN_MMAP_BYTES		(4 * KB)
#define MAX_MMAP_BYTES		(MAX_MEM_LIMIT)
#define DEFAULT_MMAP_BYTES	(256 * MB)

static const stress_help_t help[] = {
	{ NULL,	"mmap N",	 "start N workers stressing mmap and munmap" },
	{ NULL,	"mmap-async",	 "using asynchronous msyncs for file based mmap" },
	{ NULL,	"mmap-bytes N",	 "mmap and munmap N bytes for each stress iteration" },
	{ NULL,	"mmap-file",	 "mmap onto a file using synchronous msyncs" },
	{ NULL, "mmap-madvise",	 "enable random madvise on mmap'd region" },
	{ NULL,	"mmap-mergeable","where possible, flag mmap'd pages as mergeable" },
	{ NULL,	"mmap-mlock",	 "attempt to mlock mmap'd pages" },
	{ NULL,	"mmap-mmap2",	 "use mmap2 instead of mmap (when available)" },
	{ NULL,	"mmap-mprotect", "enable mmap mprotect stressing" },
	{ NULL, "mmap-odirect",	 "enable O_DIRECT on file" },
	{ NULL,	"mmap-ops N",	 "stop after N mmap bogo operations" },
	{ NULL, "mmap-osync",	 "enable O_SYNC on file" },
	{ NULL,	NULL,		 NULL }
};

typedef void * (*mmap_func_t)(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

typedef struct {
	int fd;
	int flags;
	size_t sz;
	size_t mmap_bytes;
	bool mmap_async;
	bool mmap_file;
	bool mmap_madvise;
	bool mmap_mergeable;
	bool mmap_mlock;
	bool mmap_mprotect;
	mmap_func_t mmap;
	size_t mmap_prot_count;
	int *mmap_prot_perms;
	size_t mmap_flag_count;
	int *mmap_flag_perms;
} stress_mmap_context_t;

#define NO_MEM_RETRIES_MAX	(65536)

static sigjmp_buf jmp_env;
static bool jmp_env_set;

static const int mmap_prot[] = {
#if defined(PROT_NONE)
	PROT_NONE,
#endif
#if defined(PROT_EXEC)
	PROT_EXEC,
#endif
#if defined(PROT_READ)
	PROT_READ,
#endif
#if defined(PROT_WRITE)
	PROT_WRITE,
#endif
};

static const int mmap_std_flags[] = {
#if defined(MAP_ANONYMOUS)
	MAP_ANONYMOUS,
#endif
#if defined(MAP_SHARED)
	MAP_SHARED,
#endif
#if defined(MAP_SHARED_VALIDATE)
	MAP_SHARED_VALIDATE,
#endif
#if defined(MAP_PRIVATE)
	MAP_PRIVATE,
#endif
};

/* Misc randomly chosen mmap flags */
static const int mmap_flags[] = {
#if defined(MAP_HUGE_2MB) &&	\
    defined(MAP_HUGETLB)
	MAP_HUGE_2MB | MAP_HUGETLB,
#endif
#if defined(MAP_HUGE_1GB) &&	\
    defined(MAP_HUGETLB)
	MAP_HUGE_1GB | MAP_HUGETLB,
#endif
#if defined(MAP_HUGETLB)
	MAP_HUGETLB,
#endif
#if defined(MAP_NONBLOCK)
	MAP_NONBLOCK,
#endif
#if defined(MAP_GROWSDOWN)
	MAP_GROWSDOWN,
#endif
#if defined(MAP_LOCKED)
	MAP_LOCKED,
#endif
#if defined(MAP_32BIT) &&	\
    defined(STRESS_ARCH_X86_64)
	MAP_32BIT,
#endif
#if defined(MAP_NOCACHE)	/* Mac OS X */
	MAP_NOCACHE,
#endif
#if defined(MAP_HASSEMAPHORE)	/* Mac OS X */
	MAP_HASSEMAPHORE,
#endif
#if defined(MAP_NORESERVE)
	MAP_NORESERVE,
#endif
#if defined(MAP_STACK)
	MAP_STACK,
#endif
#if defined(MAP_EXECUTABLE)
	MAP_EXECUTABLE,
#endif
#if defined(MAP_UNINITIALIZED)
	MAP_UNINITIALIZED,
#endif
#if defined(MAP_DENYWRITE)
	MAP_DENYWRITE,
#endif
/* OpenBSD */
#if defined(MAP_HASSEMAPHORE)
	 MAP_HASSEMAPHORE,
#endif
/* OpenBSD */
#if defined(MAP_INHERIT)
	MAP_INHERIT,
#endif
/* FreeBSD */
#if defined(MAP_NOCORE)
	MAP_NOCORE,
#endif
/* FreeBSD */
#if defined(MAP_NOSYNC)
	MAP_NOSYNC,
#endif
/* FreeBSD */
#if defined(MAP_PREFAULT_READ)
	MAP_PREFAULT_READ,
#endif
/* NetBSD */
#if defined(MAP_WIRED)
	MAP_WIRED,
#endif
/* NetBSD */
#if defined(MAP_ALIGNMENT_64KB)
	MAP_ALIGNMENT_64KB,
#endif
/* NetBSD */
#if defined(MAP_ALIGNMENT_16MB)
	MAP_ALIGNMENT_16MB,
#endif
/* OpenBSD */
#if defined(MAP_CONCEAL)
	MAP_CONCEAL,
#endif
/* OpenBSD */
#if defined(MAP_COPY)
	MAP_COPY,
#endif
/* Solaris */
#if defined(MAP_LOW32)
	MAP_LOW32,
#endif
	0,
};

static void MLOCKED_TEXT stress_mmap_sighandler(int signum)
{
	(void)signum;

	if (jmp_env_set)
		siglongjmp(jmp_env, 1);
}

/*
 *   mmap2_try()
 *	If mmap2 is requested then try to use it for 4K page aligned
 *	offsets. Fall back to mmap() if not possible.
 */
#if defined(HAVE_MMAP2) &&	\
    defined(HAVE_SYSCALL) &&	\
    defined(__NR_mmap2)
static void *mmap2_try(void *addr, size_t length, int prot, int flags,
	int fd, off_t offset)
{
	void *ptr;
	off_t pgoffset;

	/* Non 4K-page aligned offsets need to use mmap() */
	if (offset & 4095)
		return mmap(addr, length, prot, flags, fd, offset);
	pgoffset = offset >> 12;
	ptr = (void *)syscall(__NR_mmap2, addr, length, prot, flags, fd, pgoffset);
	if (ptr == MAP_FAILED) {
		/* For specific failure cases retry with mmap() */
		if ((errno == ENOSYS) || (errno == EINVAL))
			ptr = mmap(addr, length, prot, flags, fd, offset);
	}
	return ptr;
}
#endif

static int stress_set_mmap_bytes(const char *opt)
{
	size_t mmap_bytes;

	mmap_bytes = (size_t)stress_get_uint64_byte_memory(opt, 1);
	stress_check_range_bytes("mmap-bytes", mmap_bytes,
		MIN_MMAP_BYTES, MAX_MMAP_BYTES);
	return stress_set_setting("mmap-bytes", TYPE_ID_SIZE_T, &mmap_bytes);
}

static int stress_set_mmap_mergeable(const char *opt)
{
	return stress_set_setting_true("mmap-mergeable", opt);
}

static int stress_set_mmap_mprotect(const char *opt)
{
	return stress_set_setting_true("mmap-mprotect", opt);
}

static int stress_set_mmap_file(const char *opt)
{
	return stress_set_setting_true("mmap-file", opt);
}

static int stress_set_mmap_async(const char *opt)
{
	return stress_set_setting_true("mmap-async", opt);
}

static int stress_set_mmap_osync(const char *opt)
{
	return stress_set_setting_true("mmap-osync", opt);
}

static int stress_set_mmap_odirect(const char *opt)
{
	return stress_set_setting_true("mmap-odirect", opt);
}

static int stress_set_mmap_madvise(const char *opt)
{
	return stress_set_setting_true("mmap-madvise", opt);
}

static int stress_set_mmap_mlock(const char *opt)
{
	return stress_set_setting_true("mmap-mlock", opt);
}

static int stress_set_mmap_mmap2(const char *opt)
{
	return stress_set_setting_true("mmap-mmap2", opt);
}

/*
 *  stress_mmap_mprotect()
 *	cycle through page settings on a region of mmap'd memory
 */
static void stress_mmap_mprotect(
	const char *name,
	void *addr,
	const size_t len,
	const size_t page_size,
	const bool mmap_mprotect)
{
#if defined(HAVE_MPROTECT)
	if (mmap_mprotect) {
		void *last_page = (void *)(~(uintptr_t)0 & ~(page_size - 1));

		/* Invalid mix of PROT_GROWSDOWN | PROT_GROWSUP */
#if defined(PROT_GROWSDOWN) &&	\
    defined(PROT_GROWSUP)
		VOID_RET(int, mprotect(addr, len, PROT_READ | PROT_WRITE | PROT_GROWSDOWN | PROT_GROWSUP));
#endif

		/* Invalid non-page aligned start address */
		VOID_RET(int, mprotect((void *)(((uint8_t *)addr) + 7), len, PROT_READ | PROT_WRITE));

		/* Exercise zero len (should succeed) */
		VOID_RET(int, mprotect(addr, 0, PROT_READ | PROT_WRITE));

		/* Exercise flags all set */
		VOID_RET(int, mprotect(addr, len, ~0));

		/* Exercise invalid unmapped addressed, should return ENOMEM */
		VOID_RET(int, mprotect(last_page, page_size, PROT_READ | PROT_WRITE));

		/* Exercise invalid wrapped range, should return EINVAL */
		VOID_RET(int, mprotect(last_page, page_size << 1, PROT_READ | PROT_WRITE));

		/* Cycle through potection */
		if (mprotect(addr, len, PROT_NONE) < 0)
			pr_fail("%s: mprotect set to PROT_NONE failed\n", name);
		if (mprotect(addr, len, PROT_READ) < 0)
			pr_fail("%s: mprotect set to PROT_READ failed\n", name);
		if (mprotect(addr, len, PROT_WRITE) < 0)
			pr_fail("%s: mprotect set to PROT_WRITE failed\n", name);
		if (mprotect(addr, len, PROT_EXEC) < 0)
			pr_fail("%s: mprotect set to PROT_EXEC failed\n", name);
		if (mprotect(addr, len, PROT_READ | PROT_WRITE) < 0)
			pr_fail("%s: mprotect set to PROT_READ | PROT_WRITE failed\n", name);
	}
#else
	(void)name;
	(void)addr;
	(void)len;
	(void)page_size;
	(void)mmap_mprotect;
#endif
}

/*
 *  stress_mmap_invalid()
 *	exercise invalid mmap mapping, munmap allocation if
 *	it succeeds (which it is not expected to do).
 */
static void stress_mmap_invalid(
	void *addr,
	size_t length,
	int prot,
	int flags,
	int fd,
	off_t offset)
{
	void *ptr;

	ptr = mmap(addr, length, prot, flags, fd, offset);
	if (ptr != MAP_FAILED)
		(void)stress_munmap_retry_enomem(ptr, length);

#if defined(__NR_mmap) &&	\
    defined(HAVE_SYSCALL)
	/*
	 *  libc may detect offset is invalid and not do the syscall so
	 *  do direct syscall if possible
	 */
	ptr = (void *)(uintptr_t)syscall(__NR_mmap, addr, length, prot, flags, fd, offset + 1);
	if (ptr != MAP_FAILED)
		(void)stress_munmap_retry_enomem(ptr, length);
#endif
	/* Do the above via libc */
	ptr = mmap(addr, length, prot, flags, fd, offset + 1);
	if (ptr != MAP_FAILED)
		(void)stress_munmap_retry_enomem(ptr, length);
}

/*
 *  stress_mmap_index_shuffle()
 *	single pass shuffle to mix up page mapping orders
 */
static void OPTIMIZE3 stress_mmap_index_shuffle(size_t *index, const size_t n)
{
	size_t i;

	if (LIKELY(n <= 0xffffffff)) {
		/* small index < 4GB of items we can use 32bit mod */
		for (i = 0; i < n; i++) {
			register size_t tmp, j = (size_t)stress_mwc32modn_maybe_pwr2(n);

			tmp = index[i];
			index[i] = index[j];
			index[j] = tmp;
		}
	} else {
		for (i = 0; i < n; i++) {
			register size_t tmp, j = (size_t)stress_mwc64modn_maybe_pwr2(n);

			tmp = index[i];
			index[i] = index[j];
			index[j] = tmp;
		}
	}
}

/*
 *  stress_mmap_fast_unmap()
 *      individual page unmappings can be very slow, especially with
 *      cgroups since the page removal in the kernel release_pages
 *      path has a heavily contended spinlock on the lruvec on large
 *      systems. Since this stressor is exercising mmap and not mmunap
 *      we focus on optimizing this unmappings by trying to unmap as
 *      large a region as possible by checking for page adjacency and
 *	where possible unmapping large contiguous regions.
 */
static void stress_mmap_fast_unmap(
	uint8_t **mappings,
	uint8_t *mapped,
	const size_t pages,
	const size_t page_size)
{
	register size_t i, munmap_size = 0;
	register uint8_t *munmap_start = NULL;

	for (i = 0; i < pages; i++) {
		if (mapped[i] == PAGE_MAPPED) {
			munmap_size = page_size;
			munmap_start = mappings[i];
			break;
		}
	}

	for (; i < pages; i++) {
		if (mapped[i] == PAGE_MAPPED) {
			if (mappings[i] == munmap_start + munmap_size) {
				munmap_size += page_size;
			} else {
				(void)stress_munmap_retry_enomem((void *)munmap_start, munmap_size);
				munmap_start = mappings[i];
				munmap_size = page_size;
			}
		}
	}
	if (munmap_start && (munmap_size > 0))
		(void)stress_munmap_retry_enomem((void *)munmap_start, munmap_size);
	(void)memset(mapped, 0, pages);
}

static int stress_mmap_child(stress_args_t *args, void *ctxt)
{
	stress_mmap_context_t *context = (stress_mmap_context_t *)ctxt;
	const size_t page_size = args->page_size;
	const size_t sz = context->sz;
	const size_t pages = sz / page_size;
	const bool mmap_file = context->mmap_file;
	const int fd = context->fd;
	NOCLOBBER int no_mem_retries = 0;
	const int bad_fd = stress_get_bad_fd();
	const int ms_flags = context->mmap_async ? MS_ASYNC : MS_SYNC;
	uint8_t *mapped, **mappings;
	size_t *index;
	void *hint;
	int ret;
	NOCLOBBER int mask = ~0;
	static const char mmap_name[] = "stress-mmap";

	VOID_RET(int, stress_sighandler(args->name, SIGBUS, stress_mmap_sighandler, NULL));

	mapped = (uint8_t *)mmap(NULL, pages * sizeof(*mapped),
				PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mapped == MAP_FAILED) {
		pr_dbg("%s: cannot allocate mapped buffer: %d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	if (context->mmap_mlock)
		(void)shim_mlock(mapped, pages * sizeof(*mapped));
	mappings = (uint8_t **)mmap(NULL, pages * sizeof(*mappings),
				PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mappings == MAP_FAILED) {
		pr_dbg("%s: cannot allocate mappings buffer: %d (%s)\n",
			args->name, errno, strerror(errno));
		(void)munmap((void *)mapped, pages * sizeof(*mapped));
		return EXIT_NO_RESOURCE;
	}
	if (context->mmap_mlock)
		(void)shim_mlock(mappings, pages * sizeof(*mappings));
	index = (size_t *)mmap(NULL, pages * sizeof(*index),
				PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (index == MAP_FAILED) {
		pr_dbg("%s: cannot allocate index buffer: %d (%s)\n",
			args->name, errno, strerror(errno));
		(void)munmap((void *)mappings, pages * sizeof(*mappings));
		(void)munmap((void *)mapped, pages * sizeof(*mapped));
		return EXIT_NO_RESOURCE;
	}
	if (context->mmap_mlock)
		(void)shim_mlock(index, pages * sizeof(*index));

	do {
		size_t n;
		int rnd, rnd_flag;
		uint8_t *buf = NULL;
#if defined(HAVE_MPROTECT)
		uint64_t *buf64;
#endif
retry:
		if (no_mem_retries >= NO_MEM_RETRIES_MAX) {
			pr_inf("%s: gave up trying to mmap, no available memory\n",
				args->name);
			break;
		}

		if (!stress_continue_flag())
			break;

		if ((g_opt_flags & OPT_FLAGS_OOM_AVOID) && stress_low_memory(sz))
			goto retry;

		/*
		 *  ret is 1 if SIGBUS occurs, so re-try mmap. This
		 *  can occur when Hugepages are allocated but not
		 *  available to containers or pods that don't have
		 *  access to the hugepages mount points. It's a useful
		 *  corner case worth exercising to see if the kernel
		 *  generates a SIGBUS, so we need to handle it.
		 */
		ret = sigsetjmp(jmp_env, 1);
		if (ret == 1)
			continue;
		jmp_env_set = true;

		rnd = stress_mwc32modn(SIZEOF_ARRAY(mmap_flags));
		rnd_flag = mmap_flags[rnd];
		/*
		 *  ARM64, one can opt-int to getting VAs from 52 bit
		 *  space by hinting with an address that is > 48 bits.
		 *  Since this is a hint, we can try this for all
		 *  architectures.
		 */
		hint = stress_mwc1() ? NULL : (void *)~(uintptr_t)0;
		buf = (uint8_t *)context->mmap(hint, sz,
			PROT_READ | PROT_WRITE, (context->flags | rnd_flag) & mask, fd, 0);
		if (buf == MAP_FAILED) {
#if defined(MAP_POPULATE)
			/* Force MAP_POPULATE off, just in case */
			if (context->flags & MAP_POPULATE) {
				context->flags &= ~MAP_POPULATE;
				no_mem_retries++;
				continue;
			}
#endif
#if defined(MAP_HUGETLB)
			/* Force MAP_HUGETLB off, just in case */
			if (rnd_flag & MAP_HUGETLB) {
				mask &= ~MAP_HUGETLB;
				no_mem_retries++;
				goto retry;
			}
#endif
#if defined(MAP_UNINITIALIZED)
			/* Force MAP_UNINITIALIZED off, just in case */
			if (rnd_flag & MAP_UNINITIALIZED) {
				mask &= ~MAP_UNINITIALIZED;
				no_mem_retries++;
				goto retry;
			}
#endif
#if defined(MAP_DENYWRITE)
			/* Force MAP_DENYWRITE off, just in case */
			if (rnd_flag & MAP_DENYWRITE) {
				mask &= ~MAP_DENYWRITE;
				no_mem_retries++;
				goto retry;
			}
#endif
			no_mem_retries++;
			if (no_mem_retries > 1)
				(void)shim_usleep(100000);
			continue;	/* Try again */
		}

		if (context->mmap_mlock)
			(void)shim_mlock(buf, sz);
		no_mem_retries = 0;
		if (mmap_file) {
			(void)shim_memset(buf, 0xff, sz);
			(void)shim_msync((void *)buf, sz, ms_flags);
		}
		if (context->mmap_madvise)
			(void)stress_madvise_random(buf, sz);
		if (context->mmap_mergeable)
			(void)stress_madvise_mergeable(buf, sz);
		(void)stress_mincore_touch_pages(buf, sz);
		stress_mmap_mprotect(args->name, buf, sz, page_size, context->mmap_mprotect);
		for (n = 0; n < pages; n++) {
			mapped[n] = PAGE_MAPPED;
			mappings[n] = buf + (n * page_size);
		}

		/* Ensure we can write to the mapped pages */
		stress_mmap_set_light(buf, sz, page_size);
		if (g_opt_flags & OPT_FLAGS_VERIFY) {
			if (stress_mmap_check_light(buf, sz, page_size) < 0)
				pr_fail("%s: mmap'd region of %zu bytes does "
					"not contain expected data\n", args->name, sz);
		}

		/*
		 *  Step #0, write + read the mmap'd data from the file back into
		 *  the mappings.
		 */
		if ((fd >= 0) && (mmap_file)) {
			off_t offset = 0;

			for (n = 0; n < pages; n++, offset += page_size) {
				if (lseek(fd, offset, SEEK_SET) < 0)
					continue;

				VOID_RET(ssize_t, write(fd, mappings[n], page_size));
				VOID_RET(ssize_t, read(fd, mappings[n], page_size));
			}
		}

		(void)stress_mincore_touch_pages(buf, sz);

		/*
		 *  Step #1, set random ordered page advise and protection
		 */
		for (n = 0; n < pages; n++)
			index[n] = n;
		stress_mmap_index_shuffle(index, n);

		for (n = 0; n < pages; n++) {
			register size_t page = index[n];

			if (mapped[page] == PAGE_MAPPED) {
#if defined(HAVE_MQUERY) &&	\
    defined(MAP_FIXED)
				{
					/* Exercise OpenBSD mquery */
					VOID_RET(void *, mquery(mappings[page], page_size,
							PROT_READ, MAP_FIXED, -1, 0));
				}
#endif
				if (context->mmap_madvise)
					(void)stress_madvise_random(mappings[page], page_size);
				stress_mmap_mprotect(args->name, mappings[page],
					page_size, page_size, context->mmap_mprotect);
			}
			if (!stress_continue_flag())
				goto cleanup;
		}
		/*
		 *  ..and ummap pages
		 */
		stress_mmap_fast_unmap(mappings, mapped, pages, page_size);

		(void)stress_munmap_retry_enomem((void *)buf, sz);
#if defined(MAP_FIXED)

		/*
		 *  Step #2, map them back in random order
		 */
		stress_mmap_index_shuffle(index, n);

		for (n = 0; n < pages; n++) {
			register size_t page = index[n];

			if (!mapped[page]) {
				off_t offset = mmap_file ? (off_t)(page * page_size) : 0;
				int fixed_flags = MAP_FIXED;

				/*
				 * Attempt to map them back into the original address, this
				 * may fail (it's not the most portable operation), so keep
				 * track of failed mappings too
				 */
#if defined(MAP_FIXED_NOREPLACE)
				if (stress_mwc1())
					fixed_flags = MAP_FIXED_NOREPLACE;
#endif
				mappings[page] = (uint8_t *)context->mmap((void *)mappings[page],
					page_size, PROT_READ | PROT_WRITE, fixed_flags | context->flags, fd, offset);

				if (mappings[page] == MAP_FAILED) {
					mapped[page] = PAGE_MAPPED_FAIL;
					mappings[page] = NULL;
				} else {
					if (context->mmap_mlock)
						(void)shim_mlock(mappings[page], page_size);
					(void)stress_mincore_touch_pages(mappings[page], page_size);
					if (context->mmap_madvise)
						(void)stress_madvise_random(mappings[page], page_size);
					if (context->mmap_mergeable)
						(void)stress_madvise_mergeable(mappings[page], page_size);
					stress_mmap_mprotect(args->name, mappings[page],
						page_size, page_size, context->mmap_mprotect);
					mapped[page] = PAGE_MAPPED;
					/* Ensure we can write to the mapped page */
					stress_mmap_set_light(mappings[page], page_size, page_size);
					if (stress_mmap_check_light(mappings[page], page_size, page_size) < 0)
						pr_fail("%s: mmap'd region of %zu bytes does "
							"not contain expected data\n", args->name, page_size);
					if (mmap_file) {
						(void)shim_memset(mappings[page], (int)n, page_size);
						(void)shim_msync((void *)mappings[page], page_size, ms_flags);
#if defined(FALLOC_FL_KEEP_SIZE) &&	\
    defined(FALLOC_FL_PUNCH_HOLE)
						(void)shim_fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
							offset, (off_t)page_size);
#endif
					}
				}
				if (!stress_continue_flag())
					goto cleanup;
			}
		}
#endif
cleanup:
		/*
		 *  Step #3, unmap them all
		 */
		stress_mmap_fast_unmap(mappings, mapped, pages, page_size);

		/*
		 *  Step #4, invalid unmapping on the first found page that
		 *  was successfully mapped earlier. This page should be now
		 *  unmapped so unmap it again in various ways
		 */
		for (n = 0; n < pages; n++) {
			if (mapped[n] & PAGE_MAPPED) {
				(void)stress_munmap_retry_enomem((void *)mappings[n], 0);
				(void)stress_munmap_retry_enomem((void *)mappings[n], page_size);
				break;
			}
		}

		/*
		 *  Step #5, invalid mappings
		 */
		stress_mmap_invalid(NULL, 0, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		stress_mmap_invalid((void *)(~(uintptr_t)0), 0, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		stress_mmap_invalid(NULL, ~(size_t)0, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		stress_mmap_invalid((void *)(~(uintptr_t)0), ~(size_t)0, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		stress_mmap_invalid(NULL, args->page_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE, bad_fd, 0);
		stress_mmap_invalid(NULL, args->page_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE, bad_fd, 0);
		if (fd >= 0)
			stress_mmap_invalid(NULL, args->page_size << 2, PROT_READ | PROT_WRITE,
					MAP_PRIVATE, fd,
					(off_t)(((~(size_t)0) & ~(args->page_size - 1)) - args->page_size));

		/*
		 *  Step #6, invalid unmappings
		 */
		(void)munmap(stress_get_null(), 0);
		(void)munmap(stress_get_null(), ~(size_t)0);

		/*
		 *  Step #7, random choice from any of the valid/invalid
		 *  mmap flag permutations
		 */
		if ((context->mmap_prot_perms) && (context->mmap_prot_count > 0)) {
			const size_t rnd_sz = stress_mwc16modn(context->mmap_prot_count);
			const int rnd_prot = context->mmap_prot_perms[rnd_sz];

			buf = (uint8_t *)mmap(NULL, rnd_sz, rnd_prot, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			if (buf != MAP_FAILED) {
				if (context->mmap_mlock)
					(void)shim_mlock(buf, rnd_sz);
				(void)stress_munmap_retry_enomem((void *)buf, rnd_sz);
			}
		}

		/*
		 *  Step #8, work through all flag permutations
		 */
		if ((context->mmap_flag_perms) && (context->mmap_flag_count > 0)) {
			static size_t flag_perms_index;
			const int flag = context->mmap_flag_perms[flag_perms_index];
			int tmpfd;

			if (flag & MAP_ANONYMOUS)
				tmpfd = -1;
			else
				tmpfd = open("/dev/zero", O_RDONLY);

			buf = (uint8_t *)mmap(NULL, page_size, PROT_READ, flag, tmpfd, 0);
			if (buf != MAP_FAILED) {
				if (context->mmap_mlock)
					(void)shim_mlock(buf, page_size);
				stress_set_vma_anon_name((void *)buf, page_size, mmap_name);
				(void)stress_munmap_retry_enomem((void *)buf, page_size);
			}
			if (tmpfd >= 0)
				(void)close(tmpfd);
			flag_perms_index++;
			if (flag_perms_index >= context->mmap_flag_count)
				flag_perms_index = 0;
		}
#if defined(HAVE_MPROTECT)
		/*
		 *  Step #9, mmap write-only page, write data,
		 *  change to read-only page, read data.
		 */
		buf64 = (uint64_t *)mmap(NULL, page_size, PROT_WRITE,
					MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (buf64 != MAP_FAILED) {
			uint64_t val = stress_mwc64();

			if (context->mmap_mlock)
				(void)shim_mlock(buf64, page_size);
			stress_set_vma_anon_name((void *)buf64, page_size, mmap_name);

			*buf64 = val;
			ret = mprotect((void *)buf64, page_size, PROT_READ);
			if (ret < 0) {
				if ((errno != EACCES) &&
				    (errno != ENOMEM) &&
				    (errno != EPERM)) {
					pr_fail("%s: cannot set write-only page to read-only, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
				}
			} else {
				if (*buf64 != val) {
					pr_fail("%s: unexpected value in read-only page, "
						"got %" PRIx64 ", expected %" PRIx64 "\n",
						args->name, *buf64, val);
				}
			}
			(void)stress_munmap_retry_enomem((void *)buf64, page_size);
		}
#endif
#if defined(HAVE_MPROTECT)
		/*
		 *  Step #10, mmap read-only page, change to write-only page, write data.
		 */
		buf64 = (uint64_t *)mmap(NULL, page_size, PROT_READ,
					MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (buf64 != MAP_FAILED) {
			if (context->mmap_mlock)
				(void)shim_mlock(buf64, page_size);
			stress_set_vma_anon_name((void *)buf64, page_size, mmap_name);

			ret = mprotect((void *)buf64, page_size, PROT_WRITE);
			if (ret < 0) {
				if ((errno != EACCES) &&
				    (errno != ENOMEM) &&
				    (errno != EPERM)) {
					pr_fail("%s: cannot set read-only page to write-only, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
				}
			}
			(void)stress_munmap_retry_enomem((void *)buf64, page_size);
		}
#endif
		stress_bogo_inc(args);
	} while (stress_continue(args));

	jmp_env_set = false;

	(void)munmap((void *)index, pages * sizeof(*index));
	(void)munmap((void *)mappings, pages * sizeof(*mappings));
	(void)munmap((void *)mapped, pages * sizeof(*mapped));

	return EXIT_SUCCESS;
}

/*
 *  stress_mmap()
 *	stress mmap
 */
static int stress_mmap(stress_args_t *args)
{
	const size_t page_size = args->page_size;
	char filename[PATH_MAX];
	bool mmap_osync = false;
	bool mmap_odirect = false;
	bool mmap_mmap2 = false;
	int ret, all_flags;
	stress_mmap_context_t context;
	size_t i;

	jmp_env_set = false;

	context.fd = -1;
	context.mmap = (mmap_func_t)mmap;
	context.mmap_bytes = DEFAULT_MMAP_BYTES;
	context.mmap_async = false;
	context.mmap_file = false;
	context.mmap_madvise = false;
	context.mmap_mergeable = false;
	context.mmap_mlock = false;
	context.mmap_mprotect = false;
	context.flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_POPULATE)
	context.flags |= MAP_POPULATE;
#endif

	(void)stress_get_setting("mmap-async", &context.mmap_async);
	(void)stress_get_setting("mmap-file", &context.mmap_file);
	(void)stress_get_setting("mmap-osync", &mmap_osync);
	(void)stress_get_setting("mmap-odirect", &mmap_odirect);
	(void)stress_get_setting("mmap-madvise", &context.mmap_madvise);
	(void)stress_get_setting("mmap-mergeable", &context.mmap_mergeable);
	(void)stress_get_setting("mmap-mlock", &context.mmap_mlock);
	(void)stress_get_setting("mmap-mmap2", &mmap_mmap2);
	(void)stress_get_setting("mmap-mprotect", &context.mmap_mprotect);

	for (all_flags = 0, i = 0; i < SIZEOF_ARRAY(mmap_prot); i++)
		all_flags |= mmap_prot[i];
	context.mmap_prot_count = stress_flag_permutation(all_flags, &context.mmap_prot_perms);

	for (all_flags = 0, i = 0; i < SIZEOF_ARRAY(mmap_std_flags); i++)
		all_flags |= mmap_std_flags[i];
	for (i = 0; i < SIZEOF_ARRAY(mmap_flags); i++)
		all_flags |= mmap_flags[i];
	context.mmap_flag_count = stress_flag_permutation(all_flags, &context.mmap_flag_perms);

	if (mmap_osync || mmap_odirect)
		context.mmap_file = true;

	if (mmap_mmap2) {
#if defined(HAVE_MMAP2) && \
    defined(HAVE_SYSCALL) && \
    defined(__NR_mmap2)
		context.mmap = (mmap_func_t)mmap2_try;
#else
		if (args->instance == 0)
			pr_inf("%s: using mmap instead of mmap2 as it is not available\n",
				args->name);
#endif
	}

	if (!stress_get_setting("mmap-bytes", &context.mmap_bytes)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			context.mmap_bytes = MAX_32;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			context.mmap_bytes = MIN_MMAP_BYTES;
	}
	context.mmap_bytes /= args->num_instances;
	if (context.mmap_bytes < MIN_MMAP_BYTES)
		context.mmap_bytes = MIN_MMAP_BYTES;
	if (context.mmap_bytes < page_size)
		context.mmap_bytes = page_size;
	context.sz = context.mmap_bytes & ~(page_size - 1);

	if (context.mmap_file) {
		int file_flags = O_CREAT | O_RDWR;
		ssize_t wr_ret, rc;

		rc = stress_temp_dir_mk_args(args);
		if (rc < 0)
			return stress_exit_status((int)-rc);

		(void)stress_temp_filename_args(args,
			filename, sizeof(filename), stress_mwc32());

		if (mmap_odirect) {
#if defined(O_DIRECT)
			file_flags |= O_DIRECT;
#else
			pr_inf("%s: --mmap-odirect selected by not supported by this system\n",
				args->name);
			UNEXPECTED
#endif
		}
		if (mmap_osync) {
#if defined(O_SYNC)
			file_flags |= O_SYNC;
#else
			pr_inf("%s: --mmap-osync selected by not supported by this system\n",
				args->name);
			UNEXPECTED
#endif
		}

		context.fd = open(filename, file_flags, S_IRUSR | S_IWUSR);
		if (context.fd < 0) {
			rc = stress_exit_status(errno);
			pr_fail("%s: open %s failed, errno=%d (%s)\n",
				args->name, filename, errno, strerror(errno));
			(void)shim_unlink(filename);
			(void)stress_temp_dir_rm_args(args);

			return (int)rc;
		}
		(void)shim_unlink(filename);
		if (lseek(context.fd, (off_t)(context.sz - args->page_size), SEEK_SET) < 0) {
			pr_fail("%s: lseek failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			(void)close(context.fd);
			(void)stress_temp_dir_rm_args(args);

			return EXIT_FAILURE;
		}
redo:
		/*
		 *  Write a page aligned chunk of data, we can
		 *  use g_shared as this is mmap'd and hence
		 *  page algned and always available for reading
		 */
		wr_ret = write(context.fd, g_shared, args->page_size);
		if (wr_ret != (ssize_t)args->page_size) {
			if ((errno == EAGAIN) || (errno == EINTR))
				goto redo;
			rc = stress_exit_status(errno);
			pr_fail("%s: write failed, errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			(void)close(context.fd);
			(void)stress_temp_dir_rm_args(args);

			return (int)rc;
		}
		context.flags &= ~(MAP_ANONYMOUS | MAP_PRIVATE);
		context.flags |= MAP_SHARED;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	ret = stress_oomable_child(args, &context, stress_mmap_child, STRESS_OOMABLE_NORMAL);

	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	if (context.mmap_file) {
		(void)close(context.fd);
		(void)stress_temp_dir_rm_args(args);
	}
	if (context.mmap_prot_perms)
		free(context.mmap_prot_perms);
	if (context.mmap_flag_perms)
		free(context.mmap_flag_perms);

	return ret;
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_mmap_async,	stress_set_mmap_async },
	{ OPT_mmap_bytes,	stress_set_mmap_bytes },
	{ OPT_mmap_file,	stress_set_mmap_file },
	{ OPT_mmap_madvise,	stress_set_mmap_madvise },
	{ OPT_mmap_mergeable,	stress_set_mmap_mergeable },
	{ OPT_mmap_mlock,	stress_set_mmap_mlock },
	{ OPT_mmap_mmap2,	stress_set_mmap_mmap2 },
	{ OPT_mmap_mprotect,	stress_set_mmap_mprotect },
	{ OPT_mmap_odirect,	stress_set_mmap_odirect },
	{ OPT_mmap_osync,	stress_set_mmap_osync },
	{ 0,			NULL }
};

stressor_info_t stress_mmap_info = {
	.stressor = stress_mmap,
	.class = CLASS_VM | CLASS_OS,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
