/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "journal-file.h"

typedef struct JournalFilePreparation JournalFilePreparation;

int journal_file_preparation_start(JournalFile *f, JournalFileFlags flags, JournalFilePreparation **ret);
JournalFilePreparation* journal_file_preparation_free(JournalFilePreparation *p);
DEFINE_TRIVIAL_CLEANUP_FUNC(JournalFilePreparation*, journal_file_preparation_free);
bool journal_file_preparation_matches(JournalFilePreparation *p, JournalFile *f);
bool journal_file_preparation_done(JournalFilePreparation *p);
int journal_file_preparation_wait(JournalFilePreparation *p);
int journal_file_preparation_take(JournalFilePreparation *p, JournalFile *f, JournalFileFlags flags, MMapCache *mmap, JournalFile **ret);
