/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <pthread.h>
#include <stdatomic.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "journal-file-prepare.h"
#include "journal-file-segment.h"
#include "string-util.h"

struct JournalFilePreparation {
        pthread_t thread;
        bool started;
        atomic_bool done;
        int result;
        Header header;
        JournalFile template;
        JournalFileFlags flags;
        JournalFileSegment *segment;
};

static void* journal_file_prepare_thread(void *userdata) {
        JournalFilePreparation *p = ASSERT_PTR(userdata);

        (void) pthread_setname_np(pthread_self(), "journal-prepare");
        p->result = journal_file_segment_create(&p->template, p->flags, &p->segment);
        atomic_store_explicit(&p->done, true, memory_order_release);
        return NULL;
}

int journal_file_preparation_start(JournalFile *f, JournalFileFlags flags, JournalFilePreparation **ret) {
        _cleanup_(journal_file_preparation_freep) JournalFilePreparation *p = NULL;
        int r;

        assert(f);
        assert(ret);

        if (FLAGS_SET(flags, JOURNAL_SEAL) || JOURNAL_HEADER_SEALED(f->header))
                return -EOPNOTSUPP;
        p = new(JournalFilePreparation, 1);
        if (!p)
                return -ENOMEM;
        *p = (JournalFilePreparation) {
                .header = {
                        .file_id = f->header->file_id,
                        .seqnum_id = f->header->seqnum_id,
                        .tail_entry_seqnum = f->header->tail_entry_seqnum,
                        .compatible_flags = f->header->compatible_flags,
                },
                .flags = flags,
        };
        p->template = (JournalFile) {
                .header = &p->header,
                .metrics = f->metrics,
                .mode = f->mode,
                .compress_threshold_bytes = f->compress_threshold_bytes,
                .path = strdup(f->path),
        };
        if (!p->template.path)
                return -ENOMEM;

        /* The worker receives only copied inputs. It never accesses the active JournalFile, mmap cache,
         * event source, or FSS state. The active file may even be closed before preparation completes. */
        r = pthread_create(&p->thread, NULL, journal_file_prepare_thread, p);
        if (r != 0)
                return -r;
        p->started = true;
        *ret = TAKE_PTR(p);
        return 0;
}

int journal_file_preparation_wait(JournalFilePreparation *p) {
        assert(p);

        if (p->started) {
                assert_se(pthread_join(p->thread, NULL) == 0);
                p->started = false;
        }
        return p->result;
}

JournalFilePreparation* journal_file_preparation_free(JournalFilePreparation *p) {
        if (!p)
                return NULL;

        (void) journal_file_preparation_wait(p);
        journal_file_segment_free(p->segment);
        free(p->template.path);
        return mfree(p);
}

bool journal_file_preparation_matches(JournalFilePreparation *p, JournalFile *f) {
        return p && f && sd_id128_equal(p->header.file_id, f->header->file_id);
}

bool journal_file_preparation_done(JournalFilePreparation *p) {
        assert(p);

        return atomic_load_explicit(&p->done, memory_order_acquire);
}

int journal_file_preparation_take(JournalFilePreparation *p, JournalFile *f, JournalFileFlags flags, MMapCache *mmap, JournalFile **ret) {
        assert(p);
        assert(f);
        assert(ret);

        if (!journal_file_preparation_done(p))
                return -EAGAIN;
        if (!journal_file_preparation_matches(p, f) || p->flags != flags ||
            memcmp(&p->template.metrics, &f->metrics, sizeof(JournalMetrics)) != 0 ||
            p->template.compress_threshold_bytes != f->compress_threshold_bytes)
                return -ESTALE;
        if (p->result < 0)
                return p->result;

        (void) journal_file_preparation_wait(p);
        return journal_file_segment_adopt(p->segment, f, flags, mmap, ret);
}
