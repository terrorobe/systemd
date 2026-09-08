/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "dirent-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "journal-authenticate.h"
#include "journal-file-segment.h"
#include "journal-file-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "user-util.h"
#include "xattr-util.h"

static bool journal_file_segment_name_valid(const char *name) {
        _cleanup_free_ char *prefix = NULL;
        const char *at;
        sd_id128_t id;
        uid_t uid;

        at = strchr(name, '@');
        if (!at || strlen(at) != 1 + 32 + STRLEN(".journal") || !endswith(at, ".journal"))
                return false;

        char hex[SD_ID128_STRING_MAX];
        memcpy(hex, at + 1, 32);
        hex[32] = 0;
        if (sd_id128_from_string(hex, &id) < 0)
                return false;
        prefix = strndup(name, at - name);
        if (!prefix)
                return false;
        if (streq(prefix, "system"))
                return true;
        return startswith(prefix, "user-") && parse_uid(prefix + STRLEN("user-"), &uid) >= 0;
}

int journal_file_segment_create(JournalFile *template, JournalFileFlags flags, JournalFileSegment **ret) {
        _cleanup_(journal_file_segment_freep) JournalFileSegment *segment = NULL;
        const char *suffix, *name;
        sd_id128_t id;
        int r;

        assert(template);
        assert(ret);

        /* Keep the existing path when sealing is requested, including when keys might appear between
         * rotations. Segment creation does not yet transfer or advance shared FSS state. */
        if (FLAGS_SET(flags, JOURNAL_SEAL) || JOURNAL_HEADER_SEALED(template->header))
                return -EOPNOTSUPP;
        suffix = endswith(template->path, ".journal");
        if (!suffix)
                return -EINVAL;
        name = strrchr(template->path, '/');
        name = name ? name + 1 : template->path;
        if (journal_file_segment_name_valid(name))
                suffix = strchr(name, '@');

        segment = new(JournalFileSegment, 1);
        if (!segment)
                return -ENOMEM;
        *segment = (JournalFileSegment) { .fd = -EBADF };

        r = sd_id128_randomize(&id);
        if (r < 0)
                return r;
        if (asprintf(&segment->path, "%.*s@" SD_ID128_FORMAT_STR ".journal",
                     (int) (suffix - template->path), template->path, SD_ID128_FORMAT_VAL(id)) < 0)
                return -ENOMEM;

        segment->fd = open(segment->path, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW, template->mode);
        if (segment->fd < 0)
                return -errno;
        (void) fd_setcrtime(segment->fd, 0);
        r = journal_file_initialize_segment(segment->fd, flags, template);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(segment);
        return 0;
}

JournalFileSegment* journal_file_segment_free(JournalFileSegment *segment) {
        if (!segment)
                return NULL;

        /* Only remove a name actually created by us, never an O_EXCL collision. Adoption transfers both
         * the descriptor and namespace responsibility to the JournalFile. */
        if (segment->fd >= 0 && segment->path)
                (void) unlink(segment->path);
        safe_close(segment->fd);
        free(segment->path);
        return mfree(segment);
}

int journal_file_segment_adopt(JournalFileSegment *segment, JournalFile *template, JournalFileFlags flags, MMapCache *mmap, JournalFile **ret) {
        JournalFile *f;
        int r;

        assert(segment);
        assert(segment->fd >= 0);
        assert(segment->path);
        assert(template);
        assert(ret);

        r = journal_file_open(segment->fd, segment->path, O_RDWR|O_CREAT, flags|JOURNAL_NEW_SEGMENT,
                              template->mode, template->compress_threshold_bytes, /* metrics= */ NULL,
                              mmap, template, &f);
        if (r < 0)
                return r;

        /* The old file may have advanced since creation. Transfer the sequence boundary only when the
         * caller is ready to switch files. The new segment still has no entries. */
        f->header->seqnum_id = template->header->seqnum_id;
        f->header->tail_entry_seqnum = template->header->tail_entry_seqnum;
        segment->fd = -EBADF;
        segment->path = mfree(segment->path);
        *ret = f;
        return 0;
}

int journal_file_rotate_segment(JournalFile **f, JournalFile *next, Set *deferred_closes) {
        int r;

        assert(f);
        assert(*f);
        assert(next);
        assert(next->header->n_entries == 0);

        (void) journal_file_auth_append_tag(*f);

        /* Complete any periodic sync of this active file before changing archive intent. A normal ONLINE
         * file has no worker, so this usually returns immediately. Its predecessor is independently
         * managed by the caller's bounded deferred-close set. */
        r = journal_file_set_offline_thread_join(*f);
        if (r < 0) {
                (void) unlink(next->path);
                journal_file_close(next);
                return r;
        }

        /* The next segment's unique name is already durable. Leave it unchanged across the pointer swap;
         * old-file namespace publication happens only after its two file barriers in the finalizer. */
        (*f)->deferred_archive = true;
        (*f)->archive = true;
        next->deferred_archive = true;
        journal_file_initiate_close(*f, deferred_closes);
        *f = next;
        return 0;
}

int journal_file_recover_segments(const char *directory) {
        _cleanup_closedir_ DIR *d = NULL;
        bool changed = false;
        int r;

        d = opendir(directory);
        if (!d)
                return errno == ENOENT ? 0 : -errno;
        FOREACH_DIRENT(de, d, return -errno) {
                _cleanup_close_ int fd = -EBADF;
                struct stat st;
                Header h;
                ssize_t n;

                if (!journal_file_segment_name_valid(de->d_name))
                        continue;
                fd = openat(dirfd(d), de->d_name, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
                if (fd < 0)
                        return -errno;
                if (fstat(fd, &st) < 0)
                        return -errno;
                if (!S_ISREG(st.st_mode))
                        continue;
                n = pread(fd, &h, sizeof(h), 0);
                if (n < 0)
                        return -errno;

                /* Only a never-adopted empty segment can be discarded. Preserve all other files, including
                 * damaged headers, using the existing recoverable .journal~ convention. Do not choose only
                 * a newest-file winner, append to a crashed segment, or rewrite its contents. */
                if (n == 0 || (n == sizeof(h) && memcmp(h.signature, HEADER_SIGNATURE, 8) == 0 &&
                               h.n_objects == 0 && h.n_entries == 0)) {
                        if (unlinkat(dirfd(d), de->d_name, 0) < 0)
                                return -errno;
                } else {
                        r = journal_file_dispose(dirfd(d), de->d_name);
                        if (r < 0)
                                return r;
                }
                changed = true;
        }
        return changed ? RET_NERRNO(fsync(dirfd(d))) : 0;
}
