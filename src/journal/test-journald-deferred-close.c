/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "fd-util.h"
#include "journal-file-util.h"
#include "journald-manager.h"
#include "mkdir.h"
#include "path-util.h"
#include "rm-rf.h"
#include "set.h"
#include "tests.h"
#include "tmpfile-util.h"
#include "time-util.h"

#define N_DEFERRED_CLOSES 2

static atomic_bool fail_next_pthread_create;

__attribute__((visibility("default")))
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg) {
        static int (*real_pthread_create)(pthread_t*, const pthread_attr_t*, void *(*)(void*), void*);

        if (atomic_exchange(&fail_next_pthread_create, false))
                return EAGAIN;

        if (!real_pthread_create) {
                void *symbol;

                assert_se(symbol = dlsym(RTLD_NEXT, "pthread_create"));
                memcpy(&real_pthread_create, &symbol, sizeof(symbol));
        }

        return real_pthread_create(thread, attr, start_routine, arg);
}

typedef struct DeferredCloseFixture {
        JournalFile *file;
        int ready_fd;
        int release_fd;
        int done_fd;
        atomic_bool restart_requested;
} DeferredCloseFixture;

typedef struct AsyncCall {
        Manager *manager;
        atomic_bool done;
} AsyncCall;

static void *fake_offline_thread(void *userdata) {
        DeferredCloseFixture *fixture = ASSERT_PTR(userdata);
        eventfd_t value;

        assert_se(eventfd_write(fixture->ready_fd, 1) >= 0);
        assert_se(eventfd_read(fixture->release_fd, &value) >= 0);

        atomic_store(&fixture->restart_requested,
                     __atomic_load_n(&fixture->file->offline_state, __ATOMIC_SEQ_CST) != OFFLINE_SYNCING);
        fixture->file->header->state = STATE_ARCHIVED;
        __atomic_store_n(&fixture->file->offline_state, OFFLINE_DONE, __ATOMIC_SEQ_CST);

        assert_se(eventfd_write(fixture->done_fd, 1) >= 0);
        return NULL;
}

static void deferred_close_fixture_start(
                DeferredCloseFixture *fixture,
                MMapCache *mmap,
                const char *path) {

        eventfd_t value;

        assert_se(fixture);
        assert_se(mmap);
        assert_se(path);

        *fixture = (DeferredCloseFixture) {
                .ready_fd = -EBADF,
                .release_fd = -EBADF,
                .done_fd = -EBADF,
        };

        ASSERT_OK_ZERO(journal_file_open(
                                -EBADF,
                                path,
                                O_RDWR|O_CREAT,
                                /* file_flags= */ 0,
                                0600,
                                UINT64_MAX,
                                /* metrics= */ NULL,
                                mmap,
                                /* template= */ NULL,
                                &fixture->file));

        fixture->file->archive = true;
        __atomic_store_n(&fixture->file->offline_state, OFFLINE_SYNCING, __ATOMIC_SEQ_CST);
        ASSERT_OK_POSITIVE(fixture->ready_fd = eventfd(0, EFD_CLOEXEC));
        ASSERT_OK_POSITIVE(fixture->release_fd = eventfd(0, EFD_CLOEXEC));
        ASSERT_OK_POSITIVE(fixture->done_fd = eventfd(0, EFD_CLOEXEC));
        ASSERT_OK_ZERO(pthread_create(&fixture->file->offline_thread, NULL, fake_offline_thread, fixture));
        ASSERT_OK_ERRNO(eventfd_read(fixture->ready_fd, &value));
}

static void deferred_close_fixture_release(DeferredCloseFixture *fixture) {
        assert_se(fixture);

        ASSERT_OK_ERRNO(eventfd_write(fixture->release_fd, 1));
}

static void deferred_close_fixture_wait(DeferredCloseFixture *fixture) {
        eventfd_t value;

        assert_se(fixture);

        ASSERT_OK_ERRNO(eventfd_read(fixture->done_fd, &value));
}

static void deferred_close_fixture_done(DeferredCloseFixture *fixture) {
        assert_se(fixture);

        fixture->ready_fd = safe_close(fixture->ready_fd);
        fixture->release_fd = safe_close(fixture->release_fd);
        fixture->done_fd = safe_close(fixture->done_fd);
}

static void *vacuum_deferred_closes_thread(void *userdata) {
        AsyncCall *call = ASSERT_PTR(userdata);

        manager_vacuum_deferred_closes(call->manager, N_DEFERRED_CLOSES);
        atomic_store(&call->done, true);
        return NULL;
}

static void *clear_deferred_closes_thread(void *userdata) {
        AsyncCall *call = ASSERT_PTR(userdata);

        set_clear(call->manager->deferred_closes);
        atomic_store(&call->done, true);
        return NULL;
}

static void setup_deferred_closes(
                Manager *manager,
                MMapCache *mmap,
                const char *directory,
                DeferredCloseFixture fixtures[N_DEFERRED_CLOSES]) {

        assert_se(manager);
        assert_se(mmap);
        assert_se(directory);
        assert_se(fixtures);

        ASSERT_NOT_NULL(manager->deferred_closes = set_new(&journal_file_hash_ops_deferred_close));

        for (size_t i = 0; i < N_DEFERRED_CLOSES; i++) {
                _cleanup_free_ char *path = NULL;

                ASSERT_OK(asprintf(&path, "%s/%zu.journal", directory, i));
                deferred_close_fixture_start(fixtures + i, mmap, path);
                ASSERT_EQ(set_put(manager->deferred_closes, fixtures[i].file), 1);
        }
}

static void teardown_deferred_closes(
                Manager *manager,
                DeferredCloseFixture fixtures[N_DEFERRED_CLOSES]) {

        assert_se(manager);
        assert_se(fixtures);

        set_clear(manager->deferred_closes);
        manager->deferred_closes = set_free(manager->deferred_closes);

        for (size_t i = 0; i < N_DEFERRED_CLOSES; i++) {
                ASSERT_FALSE(atomic_load(&fixtures[i].restart_requested));
                deferred_close_fixture_done(fixtures + i);
        }
}

TEST(asynchronous_start_failure_falls_back) {
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *directory = NULL;
        _cleanup_free_ char *path = NULL;
        _cleanup_close_ int fd = -EBADF;
        JournalFile *file = NULL;
        uint8_t state;

        ASSERT_OK(mkdtemp_malloc(NULL, &directory));
        ASSERT_OK(asprintf(&path, "%s/fallback.journal", directory));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        ASSERT_OK_ZERO(journal_file_open(
                                -EBADF,
                                path,
                                O_RDWR|O_CREAT,
                                /* file_flags= */ 0,
                                0600,
                                UINT64_MAX,
                                /* metrics= */ NULL,
                                mmap,
                                /* template= */ NULL,
                                &file));

        ASSERT_NULL(file->post_change_timer);
        file->archive = true;
        atomic_store(&fail_next_pthread_create, true);
        ASSERT_EQ(journal_file_set_offline(file, /* wait= */ false), -EAGAIN);
        ASSERT_TRUE(__atomic_load_n(&file->offline_state, __ATOMIC_SEQ_CST) == OFFLINE_JOINED);

        ASSERT_NULL(journal_file_deferred_close(file));

        ASSERT_OK_ERRNO(fd = open(path, O_RDONLY|O_CLOEXEC));
        assert_se(pread(fd, &state, sizeof(state), offsetof(Header, state)) == (ssize_t) sizeof(state));
        ASSERT_EQ(state, STATE_ARCHIVED);
}

TEST(completed_deferred_close_is_reaped_first) {
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *directory = NULL;
        DeferredCloseFixture fixtures[N_DEFERRED_CLOSES];
        Manager manager = {};

        ASSERT_OK(mkdtemp_malloc(NULL, &directory));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        setup_deferred_closes(&manager, mmap, directory, fixtures);

        deferred_close_fixture_release(fixtures + 0);
        deferred_close_fixture_wait(fixtures + 0);

        manager_vacuum_deferred_closes(&manager, N_DEFERRED_CLOSES);
        ASSERT_EQ(set_size(manager.deferred_closes), 1u);
        ASSERT_TRUE(journal_file_is_offlining(fixtures[1].file));

        deferred_close_fixture_release(fixtures + 1);
        teardown_deferred_closes(&manager, fixtures);
}

TEST(deferred_close_limit_waits_without_restart) {
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *directory = NULL;
        DeferredCloseFixture fixtures[N_DEFERRED_CLOSES];
        Manager manager = {};
        AsyncCall call = { .manager = &manager };
        pthread_t thread;

        ASSERT_OK(mkdtemp_malloc(NULL, &directory));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        setup_deferred_closes(&manager, mmap, directory, fixtures);

        ASSERT_OK_ZERO(pthread_create(&thread, NULL, vacuum_deferred_closes_thread, &call));
        ASSERT_OK(usleep_safe(20 * USEC_PER_MSEC));
        ASSERT_FALSE(atomic_load(&call.done));

        for (size_t i = 0; i < N_DEFERRED_CLOSES; i++)
                deferred_close_fixture_release(fixtures + i);

        ASSERT_OK_ZERO(pthread_join(thread, NULL));
        ASSERT_TRUE(atomic_load(&call.done));
        ASSERT_EQ(set_size(manager.deferred_closes), 1u);

        teardown_deferred_closes(&manager, fixtures);
}

TEST(deferred_closes_are_drained_without_restart) {
        _cleanup_(mmap_cache_unrefp) MMapCache *mmap = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *directory = NULL;
        DeferredCloseFixture fixtures[N_DEFERRED_CLOSES];
        Manager manager = {};
        AsyncCall call = { .manager = &manager };
        pthread_t thread;

        ASSERT_OK(mkdtemp_malloc(NULL, &directory));
        ASSERT_NOT_NULL(mmap = mmap_cache_new());
        setup_deferred_closes(&manager, mmap, directory, fixtures);

        ASSERT_OK_ZERO(pthread_create(&thread, NULL, clear_deferred_closes_thread, &call));
        ASSERT_OK(usleep_safe(20 * USEC_PER_MSEC));
        ASSERT_FALSE(atomic_load(&call.done));

        for (size_t i = 0; i < N_DEFERRED_CLOSES; i++)
                deferred_close_fixture_release(fixtures + i);

        ASSERT_OK_ZERO(pthread_join(thread, NULL));
        ASSERT_TRUE(atomic_load(&call.done));
        ASSERT_TRUE(set_isempty(manager.deferred_closes));

        teardown_deferred_closes(&manager, fixtures);
}

DEFINE_TEST_MAIN(LOG_INFO);
