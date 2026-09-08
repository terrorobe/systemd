/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>

#include "dirent-util.h"
#include "fd-util.h"
#include "iovec-util.h"
#include "journal-file-segment.h"
#include "journal-file-util.h"
#include "journal-vacuum.h"
#include "journald-manager.h"
#include "path-util.h"
#include "rm-rf.h"
#include "set.h"
#include "string-util.h"
#include "tests.h"
#include "time-util.h"
#include "tmpfile-util.h"

typedef enum IOFault {
        IO_FAULT_NONE,
        IO_FAULT_SHORT,
        IO_FAULT_ZERO,
        IO_FAULT_EIO,
        IO_FAULT_EINTR,
        _IO_FAULT_MAX,
        _IO_FAULT_INVALID = -EINVAL,
} IOFault;

/* Fault injection is confined to this test executable. Returning test failures keeps fixture cleanup
 * active and allows the same regressions to run under sanitizers. */
static atomic_bool fail_directory_sync, fail_regular_sync, fail_rename, fail_unlink;
static atomic_int fail_fd = -1, allocation_error, mask_failure;
static atomic_uint failed_syncs, failed_renames, regular_syncs, regular_syncs_before_failure;
static atomic_int read_fault, write_fault, read_skip, write_skip;
static atomic_uint injected_io;
static ssize_t (*real_pread)(int, void*, size_t, off_t);
static ssize_t (*real_pwrite)(int, const void*, size_t, off_t);
static int (*real_fsync)(int);
static int (*real_unlink)(const char*);
static int (*real_renameat2)(int, const char*, int, const char*, unsigned);
static int (*real_posix_fallocate)(int, off_t, off_t);
static int (*real_pthread_sigmask)(int, const sigset_t*, sigset_t*);
static pthread_once_t symbols_once = PTHREAD_ONCE_INIT;

static void resolve_symbols(void) {
        void *p;

        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "pread64"));
        memcpy(&real_pread, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "pwrite64"));
        memcpy(&real_pwrite, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "unlink"));
        memcpy(&real_unlink, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "fsync"));
        memcpy(&real_fsync, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "renameat2"));
        memcpy(&real_renameat2, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "posix_fallocate64"));
        memcpy(&real_posix_fallocate, &p, sizeof(p));
        p = ASSERT_NOT_NULL(dlsym(RTLD_NEXT, "pthread_sigmask"));
        memcpy(&real_pthread_sigmask, &p, sizeof(p));
}

/* Arm only around the synchronous constructor/recovery call. A fault is consumed once; short I/O
 * actually transfers one byte. This models descriptor I/O, not mmap writeback or heap exhaustion. */
static IOFault take_io_fault(atomic_int *mode, atomic_int *skip) {
        IOFault fault;

        if (atomic_load(mode) == IO_FAULT_NONE || atomic_fetch_sub(skip, 1) > 0)
                return IO_FAULT_NONE;
        fault = atomic_exchange(mode, IO_FAULT_NONE);
        if (fault != IO_FAULT_NONE)
                atomic_fetch_add(&injected_io, 1);
        return fault;
}

__attribute__((visibility("default")))
ssize_t pread(int fd, void *buf, size_t size, off_t offset) {
        IOFault fault;

        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        fault = take_io_fault(&read_fault, &read_skip);
        if (IN_SET(fault, IO_FAULT_EIO, IO_FAULT_EINTR)) {
                errno = fault == IO_FAULT_EIO ? EIO : EINTR;
                return -1;
        }
        if (fault == IO_FAULT_ZERO)
                return 0;
        if (fault == IO_FAULT_SHORT) {
                ASSERT_GT(size, 1U);
                size = 1;
        }
        return real_pread(fd, buf, size, offset);
}

__attribute__((visibility("default")))
ssize_t pwrite(int fd, const void *buf, size_t size, off_t offset) {
        IOFault fault;

        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        fault = take_io_fault(&write_fault, &write_skip);
        if (IN_SET(fault, IO_FAULT_EIO, IO_FAULT_EINTR)) {
                errno = fault == IO_FAULT_EIO ? EIO : EINTR;
                return -1;
        }
        if (fault == IO_FAULT_ZERO)
                return 0;
        if (fault == IO_FAULT_SHORT) {
                ASSERT_GT(size, 1U);
                size = 1;
        }
        return real_pwrite(fd, buf, size, offset);
}

__attribute__((visibility("default")))
int fsync(int fd) {
        struct stat st;

        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        if (fstat(fd, &st) >= 0 && S_ISREG(st.st_mode)) {
                atomic_fetch_add(&regular_syncs, 1);
                if (atomic_load(&fail_regular_sync) && atomic_load(&regular_syncs_before_failure) > 0) {
                        atomic_fetch_sub(&regular_syncs_before_failure, 1);
                        return real_fsync(fd);
                }
        }
        if (fstat(fd, &st) >= 0 &&
            ((S_ISDIR(st.st_mode) && atomic_load(&fail_directory_sync)) ||
             (S_ISREG(st.st_mode) && atomic_load(&fail_regular_sync) &&
              (atomic_load(&fail_fd) < 0 || atomic_load(&fail_fd) == fd)))) {
                atomic_fetch_add(&failed_syncs, 1);
                errno = EIO;
                return -1;
        }
        return real_fsync(fd);
}

__attribute__((visibility("default")))
int unlink(const char *path) {
        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        if (atomic_load(&fail_unlink)) {
                errno = EIO;
                return -1;
        }
        return real_unlink(path);
}

__attribute__((visibility("default")))
int renameat2(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned flags) {
        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        if (atomic_load(&fail_rename)) {
                atomic_fetch_add(&failed_renames, 1);
                errno = EIO;
                return -1;
        }
        return real_renameat2(oldfd, oldpath, newfd, newpath, flags);
}

__attribute__((visibility("default")))
int posix_fallocate(int fd, off_t offset, off_t len) {
        int e;

        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        e = atomic_load(&allocation_error);
        return e > 0 ? e : real_posix_fallocate(fd, offset, len);
}

__attribute__((visibility("default")))
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
        assert_se(pthread_once(&symbols_once, resolve_symbols) == 0);
        if ((atomic_load(&mask_failure) == 1 && how == SIG_BLOCK) ||
            (atomic_load(&mask_failure) == 2 && how == SIG_SETMASK)) {
                atomic_store(&mask_failure, 0);
                return EINVAL;
        }
        return real_pthread_sigmask(how, set, oldset);
}

typedef struct Fixture {
        char *directory;
        MMapCache *mmap;
        JournalFile *file;
        Set *deferred;
        uint64_t seqnum;
        Manager manager;
} Fixture;

static void fixture_done(Fixture *t) {
        atomic_store(&read_fault, IO_FAULT_NONE);
        atomic_store(&write_fault, IO_FAULT_NONE);
        atomic_store(&read_skip, 0);
        atomic_store(&write_skip, 0);
        atomic_store(&fail_regular_sync, false);
        atomic_store(&fail_directory_sync, false);
        atomic_store(&fail_rename, false);
        atomic_store(&fail_unlink, false);
        atomic_store(&allocation_error, 0);
        atomic_store(&mask_failure, 0);
        set_free(t->deferred);
        journal_file_offline_close(t->file);
        mmap_cache_unref(t->mmap);
        rm_rf_physical_and_free(t->directory);
}

static void fixture_init(Fixture *t) {
        _cleanup_free_ char *path = NULL;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &t->directory));
        path = ASSERT_NOT_NULL(path_join(t->directory, "system.journal"));
        t->mmap = ASSERT_NOT_NULL(mmap_cache_new());
        t->deferred = ASSERT_NOT_NULL(set_new(&journal_file_hash_ops_deferred_close));
        ASSERT_OK(journal_file_open(-EBADF, path, O_RDWR|O_CREAT, /* file_flags= */ 0, 0600, UINT64_MAX,
                                    /* metrics= */ NULL, t->mmap, /* template= */ NULL, &t->file));
        t->manager = (Manager) {
                .mmap = t->mmap,
                .deferred_closes = t->deferred,
        };
        t->file->segment_state = &t->manager.segment_state;
        atomic_store(&fail_fd, -1);
        atomic_store(&failed_syncs, 0);
        atomic_store(&failed_renames, 0);
        atomic_store(&regular_syncs, 0);
        atomic_store(&regular_syncs_before_failure, 0);
}

static void append(Fixture *t) {
        struct iovec iov = IOVEC_MAKE_STRING("MESSAGE=segment failure validation");
        dual_timestamp ts;

        dual_timestamp_now(&ts);
        ASSERT_OK(journal_file_append_entry(t->file, &ts, /* boot_id= */ NULL, &iov, 1, &t->seqnum,
                                            /* seqnum_id= */ NULL, /* ret_object= */ NULL, /* ret_offset= */ NULL));
}

static void rotate(Fixture *t) {
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        JournalFile *next = NULL;
        Manager m = { .deferred_closes = t->deferred };

        manager_vacuum_deferred_closes(&m, /* max= */ 2);
        ASSERT_OK(journal_file_segment_create(t->file, /* flags= */ 0, &segment));
        ASSERT_OK(journal_file_segment_adopt(segment, t->file, /* flags= */ 0, t->mmap, &next));
        ASSERT_OK(journal_file_rotate_segment(&t->file, next, t->deferred));
        ASSERT_LE(set_size(t->deferred), 2U);
        set_clear(t->deferred);
}

static unsigned count_unique(const char *directory) {
        _cleanup_closedir_ DIR *d = NULL;
        unsigned n = 0;

        d = ASSERT_NOT_NULL(opendir(directory));
        FOREACH_DIRENT_ALL(de, d, assert_not_reached()) {
                const char *at = strchr(de->d_name, '@');

                if (at && strlen(at) == 1 + 32 + STRLEN(".journal") && endswith(at, ".journal"))
                        n++;
        }
        return n;
}

TEST(segment_initial_sync_failures) {
        for (unsigned directory = 0; directory < 2; directory++) {
                _cleanup_(fixture_done) Fixture t = {};
                _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;

                fixture_init(&t);
                atomic_store(directory ? &fail_directory_sync : &fail_regular_sync, true);
                ASSERT_ERROR(journal_file_segment_create(t.file, /* flags= */ 0, &segment), EIO);
                ASSERT_NULL(segment);
                ASSERT_EQ(atomic_load(&failed_syncs), 1U);
                ASSERT_EQ(count_unique(t.directory), 0U);
                atomic_store(&fail_directory_sync, false);
                atomic_store(&fail_regular_sync, false);
                append(&t);
        }
}

TEST(segment_allocation_failures) {
        int e;

        FOREACH_ARGUMENT(e, ENOSPC, EDQUOT, EROFS, EIO, ENOMEM) {
                _cleanup_(fixture_done) Fixture t = {};
                _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;

                fixture_init(&t);
                atomic_store(&allocation_error, e);
                ASSERT_ERROR(journal_file_segment_create(t.file, /* flags= */ 0, &segment), e);
                ASSERT_NULL(segment);
                ASSERT_EQ(count_unique(t.directory), 0U);
                atomic_store(&allocation_error, 0);
                append(&t);
        }
}

TEST(segment_unlinked_replacement) {
        _cleanup_(fixture_done) Fixture t = {};
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        JournalFile *next = NULL;

        fixture_init(&t);
        ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
        ASSERT_OK_ERRNO(unlink(segment->path));
        ASSERT_ERROR(journal_file_segment_adopt(segment, t.file, /* flags= */ 0, t.mmap, &next), EIDRM);
        ASSERT_NULL(next);
        append(&t);
}

TEST(segment_archive_collision) {
        _cleanup_(fixture_done) Fixture t = {};
        _cleanup_free_ char *archive = NULL;
        _cleanup_close_ int fd = -EBADF;
        char buf[8];

        fixture_init(&t);
        append(&t);
        rotate(&t);
        append(&t);
        ASSERT_OK(asprintf(&archive, "%s/system@" SD_ID128_FORMAT_STR "-%016" PRIx64 "-%016" PRIx64 ".journal",
                           t.directory, SD_ID128_FORMAT_VAL(t.file->header->seqnum_id),
                           le64toh(t.file->header->head_entry_seqnum), le64toh(t.file->header->head_entry_realtime)));
        fd = ASSERT_OK_ERRNO(open(archive, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0600));
        ASSERT_OK_EQ_ERRNO(write(fd, "sentinel", 8), 8);
        ASSERT_ERROR(journal_file_archive(t.file, /* ret_previous_path= */ NULL), EEXIST);
        ASSERT_OK_EQ_ERRNO(pread(fd, buf, sizeof(buf), 0), 8);
        ASSERT_EQ(memcmp(buf, "sentinel", 8), 0);
        ASSERT_OK_ERRNO(access(t.file->path, F_OK));
        ASSERT_OK_ERRNO(unlink(archive));
}

TEST(segment_signal_mask_failures) {
        for (int phase = 1; phase <= 2; phase++) {
                _cleanup_(fixture_done) Fixture t = {};
                sigset_t saved;

                fixture_init(&t);
                append(&t);
                ASSERT_OK_ZERO(pthread_sigmask(SIG_SETMASK, NULL, &saved));
                t.file->archive = true;
                atomic_store(&mask_failure, phase);
                ASSERT_ERROR(journal_file_set_offline(t.file, /* wait= */ false), EINVAL);
                ASSERT_OK_ZERO(pthread_sigmask(SIG_SETMASK, &saved, NULL));
                if (phase == 1)
                        ASSERT_EQ(__atomic_load_n(&t.file->offline_state, __ATOMIC_SEQ_CST), (OfflineState) OFFLINE_JOINED);
                t.file = journal_file_deferred_close(t.file);
        }
}

TEST_RET(segment_publication_failure_bound) {
        _cleanup_(fixture_done) Fixture t = {};
        unsigned before, after;

        fixture_init(&t);
        append(&t);
        rotate(&t); /* Begin on a unique name, with conventional first-generation publication complete. */
        atomic_store(&fail_rename, true);
        for (unsigned i = 0; i < 32; i++) {
                append(&t);
                t.manager.runtime_journal = t.file;
                manager_rotate(&t.manager);
                t.file = t.manager.runtime_journal;
                ASSERT_NOT_NULL(t.file);
                ASSERT_LE(set_size(t.deferred), 2U);
                set_clear(t.deferred); /* Explicit sync/lifecycle drains must not reset the error latch. */
        }
        before = count_unique(t.directory);
        ASSERT_GT(atomic_load(&failed_renames), 0U);
        ASSERT_ERROR(journal_file_segment_state_error(&t.manager.segment_state), EIO);
        ASSERT_EQ(set_size(t.deferred), 0U);
        ASSERT_OK(journal_directory_vacuum(t.directory, 1, 1, 0, /* oldest_usec= */ NULL, false));
        after = count_unique(t.directory);
        log_info("rotation requests=32 deferred=0 unique_before_vacuum=%u unique_after_vacuum=%u (includes active=1)", before, after);
        ASSERT_LE(after, 3U);
        /* Once storage recovers, conventional rotation works, but unique creation stays disabled. */
        atomic_store(&fail_rename, false);
        for (unsigned i = 0; i < 4; i++) {
                append(&t);
                t.manager.runtime_journal = t.file;
                manager_rotate(&t.manager);
                t.file = t.manager.runtime_journal;
                ASSERT_NOT_NULL(t.file);
                set_clear(t.deferred);
        }
        ASSERT_EQ(count_unique(t.directory), after);
        return EXIT_SUCCESS;
}

TEST(segment_failure_blocks_other_streams) {
        _cleanup_(fixture_done) Fixture a = {}, b = {};
        _cleanup_(journal_file_segment_freep) JournalFileSegment *replacement = NULL, *unused = NULL;
        JournalFile *next = NULL;

        fixture_init(&a);
        fixture_init(&b);
        b.file->segment_state = &a.manager.segment_state;
        ASSERT_OK(journal_file_segment_create(b.file, /* flags= */ 0, &replacement));
        append(&a);
        rotate(&a);
        append(&a);
        atomic_store(&fail_rename, true);
        /* Lifecycle close is outside the deferred set, but must trip the same shared error latch. */
        a.file = journal_file_offline_close(a.file);
        ASSERT_ERROR(journal_file_segment_state_error(&a.manager.segment_state), EIO);
        ASSERT_ERROR(journal_file_segment_create(b.file, /* flags= */ 0, &unused), EIO);
        ASSERT_NULL(unused);
        ASSERT_ERROR(journal_file_segment_adopt(replacement, b.file, /* flags= */ 0, b.mmap, &next), EIO);
        ASSERT_NULL(next);
        replacement = journal_file_segment_free(replacement);
        atomic_store(&fail_rename, false);
        ASSERT_ERROR(journal_file_segment_create(b.file, /* flags= */ 0, &unused), EIO);
}

TEST(segment_failure_between_adoption_and_handoff) {
        _cleanup_(fixture_done) Fixture t = {};
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        _cleanup_free_ char *path = NULL;
        JournalFile *next = NULL, *original;
        int fd;

        fixture_init(&t);
        original = t.file;
        ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
        ASSERT_OK(journal_file_segment_adopt(segment, t.file, 0, t.mmap, &next));
        path = ASSERT_NOT_NULL(strdup(next->path));
        fd = next->fd;
        journal_file_segment_state_fail(&t.manager.segment_state, -EIO);
        ASSERT_ERROR(journal_file_rotate_segment(&t.file, next, t.deferred), EIO);
        ASSERT_PTR_EQ(t.file, original);
        ASSERT_ERROR_ERRNO(access(path, F_OK), ENOENT);
        ASSERT_ERROR_ERRNO(fcntl(fd, F_GETFD), EBADF);
        ASSERT_EQ(set_size(t.deferred), 0U);
        append(&t);
}

TEST(segment_unused_cleanup_failure) {
        _cleanup_(fixture_done) Fixture t = {};
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;

        fixture_init(&t);
        ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
        atomic_store(&fail_unlink, true);
        segment = journal_file_segment_free(segment);
        ASSERT_ERROR(journal_file_segment_state_error(&t.manager.segment_state), EIO);
        for (unsigned i = 0; i < 32; i++)
                ASSERT_ERROR(journal_file_segment_create(t.file, /* flags= */ 0, &segment), EIO);
        ASSERT_NULL(segment);
        ASSERT_EQ(count_unique(t.directory), 1U);
}

TEST(segment_failed_barriers_prevent_publication) {
        for (unsigned barrier = 0; barrier < 2; barrier++) {
                _cleanup_(fixture_done) Fixture t = {};
                _cleanup_free_ char *path = NULL;

                fixture_init(&t);
                append(&t);
                rotate(&t);
                append(&t);
                path = ASSERT_NOT_NULL(strdup(t.file->path));
                t.file->archive = true;
                atomic_store(&regular_syncs_before_failure, barrier);
                atomic_store(&fail_regular_sync, true);
                ASSERT_ERROR(journal_file_set_offline(t.file, /* wait= */ true), EIO);
                ASSERT_EQ(atomic_load(&failed_syncs), 1U);
                ASSERT_TRUE(t.file->deferred_archive);
                ASSERT_FALSE(t.file->archive_data_synced);
                ASSERT_STREQ(t.file->path, path);
                ASSERT_EQ(t.file->header->state, barrier == 0 ? STATE_ONLINE : STATE_ARCHIVED);
                atomic_store(&fail_regular_sync, false);
                ASSERT_OK(journal_file_set_offline(t.file, /* wait= */ true));
                ASSERT_TRUE(t.file->archive_data_synced);
                ASSERT_FALSE(t.file->deferred_archive);
                ASSERT_ERROR_ERRNO(access(path, F_OK), ENOENT);
                ASSERT_ERROR(journal_file_segment_state_error(&t.manager.segment_state), EIO);
        }
}

TEST_RET(segment_failed_directory_sync_retry) {
        _cleanup_(fixture_done) Fixture t = {};
        unsigned first, file_syncs;
        int r, again;

        fixture_init(&t);
        append(&t);
        rotate(&t);
        append(&t);
        t.file->archive = true;
        atomic_store(&fail_directory_sync, true);
        r = journal_file_set_offline(t.file, /* wait= */ true);
        first = atomic_load(&failed_syncs);
        file_syncs = atomic_load(&regular_syncs);
        ASSERT_EQ(first, 1U);
        ASSERT_TRUE(t.file->archive_directory_sync_pending);
        ASSERT_TRUE(t.file->deferred_archive);
        again = journal_file_set_offline(t.file, /* wait= */ true);
        ASSERT_EQ(atomic_load(&regular_syncs), file_syncs);
        ASSERT_EQ(atomic_load(&failed_syncs), 2U);
        log_info("failed_directory_syncs_first=%u after_retry=%u result=%i retry_result=%i publication_pending=%s",
                 first, atomic_load(&failed_syncs), r, again, yes_no(t.file->deferred_archive));
        ASSERT_ERROR(r, EIO);
        ASSERT_ERROR(again, EIO);
        atomic_store(&fail_directory_sync, false);
        ASSERT_OK(journal_file_set_offline(t.file, /* wait= */ true));
        ASSERT_EQ(atomic_load(&regular_syncs), file_syncs);
        ASSERT_FALSE(t.file->archive_directory_sync_pending);
        ASSERT_FALSE(t.file->deferred_archive);
        return EXIT_SUCCESS;
}

TEST_RET(segment_damaged_header_recovery) {
        _cleanup_(fixture_done) Fixture t = {};
        _cleanup_closedir_ DIR *d = NULL;
        struct stat before;
        bool preserved = false;

        fixture_init(&t);
        append(&t);
        rotate(&t);
        append(&t);
        ASSERT_GT(le64toh(t.file->header->tail_object_offset), 0U);
        ASSERT_OK_ERRNO(fstat(t.file->fd, &before));
        /* Contradictory damaged metadata is not proof of a never-adopted empty replacement. */
        t.file->header->n_entries = 0;
        t.file->header->n_objects = 0;
        ASSERT_OK(journal_file_set_offline(t.file, /* wait= */ true));
        t.file = journal_file_close(t.file);
        ASSERT_OK(journal_file_recover_segments(t.directory));
        ASSERT_OK(journal_file_recover_segments(t.directory));
        /* Ordinary startup vacuum must not undo conservative recovery via the same damaged counter. */
        ASSERT_OK(journal_directory_vacuum(t.directory, UINT64_MAX, 1000, 0, NULL, false));
        d = ASSERT_NOT_NULL(opendir(t.directory));
        FOREACH_DIRENT_ALL(de, d, assert_not_reached()) {
                struct stat st;

                ASSERT_OK_ERRNO(fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW));
                if (st.st_ino == before.st_ino && st.st_dev == before.st_dev)
                        preserved = true;
        }
        log_info("populated_segment_with_damaged_counts_preserved=%s", yes_no(preserved));
        return preserved ? EXIT_SUCCESS : EXIT_FAILURE;
}

TEST(segment_empty_recovery) {
        for (unsigned variant = 0; variant < 8; variant++) {
                _cleanup_(fixture_done) Fixture t = {};
                _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
                _cleanup_closedir_ DIR *d = NULL;
                struct stat before;
                Header h;
                bool preserved = false;

                fixture_init(&t);
                ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
                ASSERT_OK_EQ_ERRNO(pread(segment->fd, &h, sizeof(h), 0), (ssize_t) sizeof(h));
                switch (variant) {
                case 0: /* Fully initialized, zero-filled unused replacement. */
                        break;
                case 1: /* Zero-length interrupted creation. */
                        ASSERT_OK_ERRNO(ftruncate(segment->fd, 0));
                        break;
                case 2: /* Partial header, not provably empty. */
                        ASSERT_OK_ERRNO(ftruncate(segment->fd, sizeof(h) / 2));
                        break;
                case 3: /* Counts can be zero while actual object data remains. */
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, "x", 1, sizeof(h) + 8192), 1);
                        break;
                case 4: /* Check the end of the arena too. */
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, "x", 1, le64toh(h.header_size) + le64toh(h.arena_size) - 1), 1);
                        break;
                case 5: /* Future format, even if otherwise empty. */
                        h.compatible_flags |= htole32(1U << 31);
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, &h, sizeof(h), 0), (ssize_t) sizeof(h));
                        break;
                case 6:
                        h.header_size = htole64(sizeof(h) + 8);
                        h.arena_size = htole64(le64toh(h.arena_size) - 8);
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, &h, sizeof(h), 0), (ssize_t) sizeof(h));
                        break;
                case 7: /* Recovery work is bounded; large empty-looking images are preserved. */
                        ASSERT_OK_ERRNO(ftruncate(segment->fd, 9 * U64_MB));
                        h.arena_size = htole64(9 * U64_MB - sizeof(h));
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, &h, sizeof(h), 0), (ssize_t) sizeof(h));
                        break;
                default:
                        assert_not_reached();
                }
                ASSERT_OK_ERRNO(fstat(segment->fd, &before));
                segment->fd = safe_close(segment->fd);
                ASSERT_OK(journal_file_recover_segments(t.directory));
                ASSERT_OK(journal_file_recover_segments(t.directory));
                ASSERT_OK(journal_directory_vacuum(t.directory, UINT64_MAX, 1000, 0, NULL, false));
                d = ASSERT_NOT_NULL(opendir(t.directory));
                FOREACH_DIRENT_ALL(de, d, assert_not_reached()) {
                        struct stat st;

                        ASSERT_OK_ERRNO(fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW));
                        if (st.st_ino == before.st_ino && st.st_dev == before.st_dev)
                                preserved = true;
                }
                ASSERT_EQ(preserved, variant >= 2);
                /* Preserving ambiguous images is not exemption from explicitly configured retention. */
                ASSERT_OK(journal_directory_vacuum(t.directory, 1, 1, 0, NULL, false));
                rewinddir(d);
                FOREACH_DIRENT_ALL(de, d, assert_not_reached()) {
                        struct stat st;

                        ASSERT_OK_ERRNO(fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW));
                        ASSERT_FALSE(st.st_ino == before.st_ino && st.st_dev == before.st_dev);
                }
        }
}

TEST(segment_constructor_short_io) {
        static const struct {
                const char *name;
                bool read;
                unsigned writes_to_skip;
        } steps[] = {
                { .name = "initial header write" },
                { .name = "header readback", .read = true },
                { .name = "final header write", .writes_to_skip = 1 },
        };

        FOREACH_ELEMENT(step, steps) {
                IOFault fault;

                FOREACH_ARGUMENT(fault, IO_FAULT_SHORT, IO_FAULT_ZERO, IO_FAULT_EIO) {
                        _cleanup_(fixture_done) Fixture t = {};
                        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;

                        log_info("Constructor %s: I/O fault %i", step->name, fault);
                        fixture_init(&t);
                        atomic_store(&injected_io, 0);
                        atomic_store(step->read ? &read_fault : &write_fault, fault);
                        atomic_store(&write_skip, step->writes_to_skip);
                        /* Failure must not hand out an incomplete segment, regardless of errno mapping. */
                        ASSERT_LT(journal_file_segment_create(t.file, /* flags= */ 0, &segment), 0);
                        ASSERT_EQ(atomic_load(&injected_io), 1U);
                        ASSERT_NULL(segment);
                        ASSERT_EQ(count_unique(t.directory), 0U);
                        append(&t);
                        ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
                }
        }
}

TEST(segment_recovery_short_io) {
        static const struct {
                const char *name;
                IOFault fault;
                bool arena, preserve, nonzero_data;
        } cases[] = {
                { .name = "short header", .fault = IO_FAULT_SHORT, .preserve = true },
                { .name = "header EOF", .fault = IO_FAULT_ZERO, .preserve = true },
                { .name = "header error", .fault = IO_FAULT_EIO, .preserve = true },
                { .name = "short arena read", .fault = IO_FAULT_SHORT, .arena = true },
                { .name = "arena EOF", .fault = IO_FAULT_ZERO, .arena = true, .preserve = true },
                { .name = "arena error", .fault = IO_FAULT_EIO, .arena = true, .preserve = true },
                { .name = "interrupted arena read", .fault = IO_FAULT_EINTR, .arena = true },
                { .name = "short arena read before nonzero data", .fault = IO_FAULT_SHORT,
                  .arena = true, .preserve = true, .nonzero_data = true },
        };

        FOREACH_ELEMENT(c, cases) {
                _cleanup_(fixture_done) Fixture t = {};
                _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
                _cleanup_closedir_ DIR *d = NULL;
                struct stat original;
                bool preserved = false;
                int r;

                log_info("Recovery: %s", c->name);
                fixture_init(&t);
                ASSERT_OK(journal_file_segment_create(t.file, /* flags= */ 0, &segment));
                /* The short arena read returns only the first zero byte. The following nonzero byte
                 * must still be scanned, not skipped by advancing past the whole requested buffer. */
                if (c->nonzero_data)
                        ASSERT_OK_EQ_ERRNO(pwrite(segment->fd, "x", 1, sizeof(Header) + 1), 1);
                ASSERT_OK_ERRNO(fstat(segment->fd, &original));
                segment->fd = safe_close(segment->fd);
                atomic_store(&injected_io, 0);
                atomic_store(&read_fault, c->fault);
                atomic_store(&read_skip, c->arena); /* The header read precedes the arena scan. */
                r = journal_file_recover_segments(t.directory);
                ASSERT_EQ(atomic_load(&injected_io), 1U);
                if (c->fault == IO_FAULT_EIO)
                        ASSERT_LT(r, 0);
                else
                        ASSERT_OK(r);
                d = ASSERT_NOT_NULL(opendir(t.directory));
                FOREACH_DIRENT_ALL(de, d, assert_not_reached()) {
                        struct stat st;

                        ASSERT_OK_ERRNO(fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW));
                        if (st.st_ino == original.st_ino && st.st_dev == original.st_dev)
                                preserved = true;
                }
                ASSERT_EQ(preserved, c->preserve);
                /* Retry after the one-shot fault, including after ambiguous images have been renamed. */
                ASSERT_OK(journal_file_recover_segments(t.directory));
                ASSERT_OK(journal_file_recover_segments(t.directory));
                append(&t);
        }
}

/* Manager-level conventional sync acknowledgment errors are outside this protocol regression suite. */

DEFINE_TEST_MAIN(LOG_INFO);
