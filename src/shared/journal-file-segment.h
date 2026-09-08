/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "journal-file.h"

typedef struct JournalFileSegment {
        int fd;
        char *path;
        JournalFileSegmentState *state;
} JournalFileSegment;

int journal_file_segment_create(JournalFile *template, JournalFileFlags flags, JournalFileSegment **ret);
JournalFileSegment* journal_file_segment_free(JournalFileSegment *segment);
DEFINE_TRIVIAL_CLEANUP_FUNC(JournalFileSegment*, journal_file_segment_free);
int journal_file_segment_adopt(JournalFileSegment *segment, JournalFile *template, JournalFileFlags flags, MMapCache *mmap, JournalFile **ret);
int journal_file_rotate_segment(JournalFile **f, JournalFile *next, Set *deferred_closes);
int journal_file_recover_segments(const char *directory);
