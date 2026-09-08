/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <threads.h>

#include "sd-journal.h"

#include "fd-util.h"
#include "iovec-util.h"
#include "journal-file-segment.h"
#include "journal-file-util.h"
#include "journal-vacuum.h"
#include "path-util.h"
#include "rm-rf.h"
#include "set.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "time-util.h"

static thread_local unsigned n_fsync;
static atomic_bool fail_next_fsync;
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
        if (atomic_exchange(&fail_next_fsync, false)) {
                errno = EIO;
                return -1;
        }
        return real_fsync(fd);
}

static void append(JournalFile *f, uint64_t *seqnum) {
        struct iovec iov = IOVEC_MAKE_STRING("MESSAGE=segment handoff");
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

TEST(segment_handoff) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_set_free_ Set *deferred = NULL;
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        uint64_t seqnum = 0;
        unsigned n = 0;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        ASSERT_NOT_NULL(deferred = set_new(&journal_file_hash_ops_deferred_close));
        f = open_file(mmap, path);
        for (unsigned i = 0; i < 5; i++) {
                _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
                JournalFile *next = NULL;

                append(f, &seqnum);
                ASSERT_OK(journal_file_segment_create(f, /* flags= */ 0, &segment));
                append(f, &seqnum); /* Adoption must not use creation's stale sequence boundary. */
                unsigned before = n_fsync;
                ASSERT_OK(journal_file_segment_adopt(segment, f, /* flags= */ 0, mmap, &next));
                ASSERT_EQ(next->header->tail_entry_seqnum, f->header->tail_entry_seqnum);
                ASSERT_OK(journal_file_rotate_segment(&f, next, deferred));
                ASSERT_EQ(n_fsync, before);
                ASSERT_TRUE(f->deferred_archive);
                ASSERT_NOT_NULL(strchr(f->path, '@'));
                set_clear(deferred);
        }
        append(f, &seqnum);
        f = journal_file_offline_close(f);
        ASSERT_OK(sd_journal_open_directory(&j, dir, /* flags= */ 0));
        SD_JOURNAL_FOREACH(j)
                n++;
        ASSERT_EQ(n, 11U);
        ASSERT_EQ(seqnum, 11U);
}

TEST(segment_vacuum) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        _cleanup_set_free_ Set *deferred = NULL;
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        JournalFile *next = NULL;
        uint64_t seqnum = 0;
        unsigned n = 0;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        ASSERT_NOT_NULL(deferred = set_new(&journal_file_hash_ops_deferred_close));
        f = open_file(mmap, path);
        append(f, &seqnum);
        ASSERT_OK(journal_file_segment_create(f, /* flags= */ 0, &segment));
        ASSERT_OK(journal_file_segment_adopt(segment, f, /* flags= */ 0, mmap, &next));
        ASSERT_OK(journal_file_rotate_segment(&f, next, deferred));
        set_clear(deferred);
        append(f, &seqnum);
        ASSERT_OK(journal_file_set_offline(f, /* wait= */ true));
        ASSERT_OK(journal_directory_vacuum(dir, 1, 1, 0, /* oldest_usec= */ NULL, false));
        ASSERT_OK_ERRNO(access(f->path, F_OK));
        f = journal_file_offline_close(f);
        ASSERT_OK(sd_journal_open_directory(&j, dir, /* flags= */ 0));
        SD_JOURNAL_FOREACH(j)
                n++;
        ASSERT_EQ(n, 1U);
}

TEST(segment_failure) {
        _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
        _cleanup_free_ char *path = NULL, *unused_path = NULL;
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(journal_file_offline_closep) JournalFile *f = NULL;
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        JournalFile *next = NULL;
        uint64_t seqnum = 0;

        ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
        ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        f = open_file(mmap, path);
        ASSERT_ERROR(journal_file_segment_create(f, JOURNAL_SEAL, &segment), EOPNOTSUPP);
        ASSERT_NULL(segment);
        atomic_store(&fail_next_fsync, true);
        ASSERT_ERROR(journal_file_segment_create(f, /* flags= */ 0, &segment), EIO);
        ASSERT_NULL(segment);
        append(f, &seqnum);

        ASSERT_OK(journal_file_segment_create(f, /* flags= */ 0, &segment));
        ASSERT_NOT_NULL(unused_path = strdup(segment->path));
        ASSERT_OK_ERRNO(ftruncate(segment->fd, 0));
        ASSERT_ERROR(journal_file_segment_adopt(segment, f, /* flags= */ 0, mmap, &next), EBADMSG);
        ASSERT_NULL(next);
        segment = journal_file_segment_free(segment);
        ASSERT_ERROR_ERRNO(access(unused_path, F_OK), ENOENT);
        ASSERT_OK(journal_file_rotate(&f, mmap, /* file_flags= */ 0, UINT64_MAX, /* deferred_closes= */ NULL));
        append(f, &seqnum);
        ASSERT_EQ(seqnum, 2U);
}

TEST(segment_process_crash_recovery) {
        for (unsigned boundary = 0; boundary < 3; boundary++) {
                _cleanup_(rm_rf_physical_and_freep) char *dir = NULL;
                _cleanup_free_ char *path = NULL;
                _cleanup_(sd_journal_closep) sd_journal *j = NULL;
                unsigned n = 0;
                int status;
                pid_t pid;

                ASSERT_OK(mkdtemp_malloc(/* pattern= */ NULL, &dir));
                ASSERT_NOT_NULL(path = path_join(dir, "system.journal"));
                ASSERT_OK_ERRNO(pid = fork());
                if (pid == 0) {
                        MMapCache *mmap = mmap_cache_new();
                        JournalFile *f = open_file(mmap, path), *next = NULL;
                        JournalFileSegment *segment = NULL;
                        Set *deferred = set_new(&journal_file_hash_ops_deferred_close);
                        uint64_t seqnum = 0;

                        append(f, &seqnum);
                        ASSERT_OK(journal_file_segment_create(f, /* flags= */ 0, &segment));
                        if (boundary > 0) {
                                ASSERT_OK(journal_file_segment_adopt(segment, f, /* flags= */ 0, mmap, &next));
                                ASSERT_OK(journal_file_rotate_segment(&f, next, deferred));
                                append(f, &seqnum);
                        }
                        if (boundary > 1) {
                                set_clear(deferred);
                                ASSERT_OK(journal_file_set_offline(f, /* wait= */ true));
                        }
                        /* Process death before adoption, after handoff, or after explicit sync. This is
                         * not a power-loss test: the kernel's page cache survives SIGKILL. */
                        assert_se(kill(getpid(), SIGKILL) == 0);
                        _exit(EXIT_FAILURE);
                }
                ASSERT_EQ(waitpid(pid, &status, 0), pid);
                ASSERT_TRUE(WIFSIGNALED(status));
                ASSERT_EQ(WTERMSIG(status), SIGKILL);
                ASSERT_OK(journal_file_recover_segments(dir));
                ASSERT_OK(journal_file_recover_segments(dir));
                ASSERT_OK(sd_journal_open_directory(&j, dir, /* flags= */ 0));
                SD_JOURNAL_FOREACH(j)
                        n++;
                ASSERT_EQ(n, boundary == 0 ? 1U : 2U);
        }
}

DEFINE_TEST_MAIN(LOG_DEBUG);
