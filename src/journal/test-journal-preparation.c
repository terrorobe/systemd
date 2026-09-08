/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <threads.h>

#include "sd-journal.h"

#include "fd-util.h"
#include "iovec-util.h"
#include "journal-file-prepare.h"
#include "journal-file-segment.h"
#include "journal-file-util.h"
#include "path-util.h"
#include "rm-rf.h"
#include "set.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "time-util.h"

static thread_local unsigned n_fsync;
static atomic_bool fail_next_fsync;
static atomic_bool fail_next_create;
static int (*real_pthread_create)(pthread_t*, const pthread_attr_t*, void *(*)(void*), void*);
static pthread_once_t create_once = PTHREAD_ONCE_INIT;

static void resolve_pthread_create(void) {
        void *symbol;

        assert_se(symbol = dlsym(RTLD_NEXT, "pthread_create"));
        memcpy(&real_pthread_create, &symbol, sizeof(symbol));
}

__attribute__((visibility("default")))
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg) {
        if (atomic_exchange(&fail_next_create, false))
                return EAGAIN;
        assert_se(pthread_once(&create_once, resolve_pthread_create) == 0);
        return real_pthread_create(thread, attr, start_routine, arg);
}

static atomic_bool block_next_fsync;
static int prepare_ready_fd, prepare_release_fd;
static int (*real_fsync)(int);
static pthread_once_t fsync_once = PTHREAD_ONCE_INIT;

static void resolve_fsync(void) {
        void *symbol;

        assert_se(symbol = dlsym(RTLD_NEXT, "fsync"));
        memcpy(&real_fsync, &symbol, sizeof(symbol));
}

__attribute__((visibility("default")))
int fsync(int fd) {
        assert_se(pthread_once(&fsync_once, resolve_fsync) == 0);
        n_fsync++;
        if (atomic_exchange(&block_next_fsync, false)) {
                eventfd_t value;

                assert_se(eventfd_write(prepare_ready_fd, 1) == 0);
                assert_se(eventfd_read(prepare_release_fd, &value) == 0);
        }
        if (atomic_exchange(&fail_next_fsync, false)) {
                errno = EIO;
                return -1;
        }
        return real_fsync(fd);
}

static void append(JournalFile *f, uint64_t *seqnum) {
        struct iovec iov = IOVEC_MAKE_STRING("MESSAGE=prepared rotation");
        dual_timestamp ts;

        dual_timestamp_now(&ts);
        ASSERT_OK(journal_file_append_entry(f, &ts, /* boot_id= */ NULL, &iov, 1, seqnum,
                                            /* seqnum_id= */ NULL, /* ret_object= */ NULL, /* ret_offset= */ NULL));
}

static JournalFile* open_file(MMapCache *mmap, const char *path) {
        JournalFile *f = NULL;

        ASSERT_OK(journal_file_open(-EBADF, path, O_RDWR|O_CREAT, /* file_flags= */ 0, 0600, UINT64_MAX,
                                    /* metrics= */ NULL, mmap, /* template= */ NULL, &f));
        return f;
}

static int rotate_synchronously(JournalFile **f, MMapCache *mmap) {
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        JournalFile *next = NULL;

        ASSERT_OK(journal_file_segment_create(*f, /* flags= */ 0, &segment));
        ASSERT_OK(journal_file_segment_adopt(segment, *f, /* flags= */ 0, mmap, &next));
        return journal_file_rotate_segment(f, next, /* deferred_closes= */ NULL);
}

TEST(prepare_rotate) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_preparation_freep) JournalFilePreparation *p = NULL;
        _cleanup_set_free_ Set *deferred = NULL;
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        uint64_t seqnum = 0;
        unsigned n = 0;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        ASSERT_NOT_NULL(deferred = set_new(&journal_file_hash_ops_deferred_close));
        f = open_file(mmap, path);

        for (unsigned i = 0; i < 3; i++) {
                JournalFile *prepared = NULL;
                sd_id128_t id = f->header->file_id;

                append(f, &seqnum);
                ASSERT_OK(journal_file_preparation_start(f, /* flags= */ 0, &p));
                ASSERT_TRUE(journal_file_preparation_matches(p, f));
                /* Continue appending after the worker has snapshotted the sequence boundary. */
                append(f, &seqnum);
                ASSERT_OK(journal_file_preparation_wait(p));
                ASSERT_TRUE(journal_file_preparation_done(p));
                unsigned before = n_fsync;
                ASSERT_OK(journal_file_preparation_take(p, f, /* flags= */ 0, mmap, &prepared));
                ASSERT_EQ(n_fsync, before);
                ASSERT_EQ(prepared->header->tail_entry_seqnum, f->header->tail_entry_seqnum);
                p = journal_file_preparation_free(p);
                ASSERT_OK(journal_file_rotate_segment(&f, prepared, deferred));
                ASSERT_TRUE(f->deferred_archive);
                ASSERT_FALSE(sd_id128_equal(id, f->header->file_id));
                ASSERT_EQ(f->header->state, STATE_ONLINE);
                set_clear(deferred);
        }
        append(f, &seqnum);
        f = journal_file_offline_close(f);
        ASSERT_OK(sd_journal_open_directory(&j, dir, /* flags= */ 0));
        SD_JOURNAL_FOREACH(j)
                n++;
        ASSERT_EQ(n, 7U);
        ASSERT_EQ(seqnum, 7U);
}

TEST(prepare_reject_and_discard) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_preparation_freep) JournalFilePreparation *p = NULL;
        JournalFile *prepared = NULL;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        f = open_file(mmap, path);
        ASSERT_ERROR(journal_file_preparation_start(f, JOURNAL_SEAL, &p), EOPNOTSUPP);
        ASSERT_NULL(p);
        atomic_store(&fail_next_create, true);
        ASSERT_ERROR(journal_file_preparation_start(f, /* flags= */ 0, &p), EAGAIN);
        ASSERT_NULL(p);
        ASSERT_OK(journal_file_preparation_start(f, /* flags= */ 0, &p));
        ASSERT_OK(journal_file_preparation_wait(p));
        ASSERT_ERROR(journal_file_preparation_take(p, f, JOURNAL_COMPRESS, mmap, &prepared), ESTALE);
        ASSERT_NULL(prepared);
        p = journal_file_preparation_free(p);

        /* Disposal also joins a still-running worker and leaves the active file untouched. */
        ASSERT_OK(journal_file_preparation_start(f, /* flags= */ 0, &p));
        p = journal_file_preparation_free(p);
        ASSERT_OK_ERRNO(access(path, F_OK));
}

TEST(prepare_not_ready_fallback) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_preparation_freep) JournalFilePreparation *p = NULL;
        _cleanup_close_ int ready = -EBADF, release = -EBADF;
        JournalFile *prepared = NULL;
        uint64_t seqnum = 0;
        eventfd_t value;
        JournalFileSegmentState state = {};

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        f = open_file(mmap, path);
        append(f, &seqnum);
        ASSERT_OK_ERRNO(ready = eventfd(0, EFD_CLOEXEC));
        ASSERT_OK_ERRNO(release = eventfd(0, EFD_CLOEXEC));
        prepare_ready_fd = ready;
        prepare_release_fd = release;
        f->segment_state = &state; /* The owner outlives both the replaced file and the gated preparer. */
        atomic_store(&block_next_fsync, true);
        ASSERT_OK(journal_file_preparation_start(f, /* flags= */ 0, &p));
        ASSERT_OK_ERRNO(eventfd_read(ready, &value));
        ASSERT_FALSE(journal_file_preparation_done(p));
        ASSERT_ERROR(journal_file_preparation_take(p, f, /* flags= */ 0, mmap, &prepared), EAGAIN);
        ASSERT_NULL(prepared);
        /* A preparation holds no references to the active JournalFile. It remains safe to rotate and
         * destroy that file while its snapshot's fsync is held at the gate. */
        ASSERT_OK(rotate_synchronously(&f, mmap));
        append(f, &seqnum);
        ASSERT_OK_ERRNO(eventfd_write(release, 1));
        ASSERT_OK(journal_file_preparation_wait(p));
        ASSERT_ERROR(journal_file_preparation_take(p, f, /* flags= */ 0, mmap, &prepared), ESTALE);
        ASSERT_NULL(prepared);
}

TEST(prepare_worker_sync_failure) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_preparation_freep) JournalFilePreparation *p = NULL;
        JournalFile *prepared = NULL;
        uint64_t seqnum = 0;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        f = open_file(mmap, path);
        atomic_store(&fail_next_fsync, true);
        ASSERT_OK(journal_file_preparation_start(f, /* flags= */ 0, &p));
        ASSERT_ERROR(journal_file_preparation_wait(p), EIO);
        ASSERT_ERROR(journal_file_preparation_take(p, f, /* flags= */ 0, mmap, &prepared), EIO);
        ASSERT_NULL(prepared);
        p = journal_file_preparation_free(p);
        append(f, &seqnum);
        ASSERT_OK(rotate_synchronously(&f, mmap));
        append(f, &seqnum);
        ASSERT_EQ(seqnum, 2U);
}

DEFINE_TEST_MAIN(LOG_DEBUG);
