---
title: Journal Segment Lifecycle
category: Interfaces
layout: default
SPDX-License-Identifier: LGPL-2.1-or-later
---

# Journal Segment Lifecycle

An unsealed journal can rotate to a replacement with a unique name rather than reuse the active
pathname at each rotation. The binary journal format, file IDs, sequence numbers and cursors are unchanged.
When sealing is requested, journald retains conventional rotation, including when sealing keys are not
currently available. Segment creation does not advance or transfer shared FSS state.

## Creation and handoff

Initial journal opens use `system.journal` or `user-<uid>.journal` as before. A replacement segment uses
`system@<random-id>.journal` or `user-<uid>@<random-id>.journal`, where the random ID is 32 hexadecimal
characters. This is not the conventional archive pattern containing a sequence-number ID, first entry
sequence and timestamp. Existing vacuum therefore counts the file's space but does not delete it as an
archive while it is active or awaiting finalization.

Creation initializes an empty ONLINE header, preallocates an initial bounded extent, and synchronizes
the file and its initial directory entry. It uses descriptor I/O rather than an additional mmap cache.
Only the explicitly transferred new descriptor can bypass the ordinary rejection of an unclean ONLINE
file. Adoption validates that it is still empty and unsealed, attaches it to the writer's existing mmap
cache, and initializes hash tables through the ordinary constructors.

The writer transfers the old file's final sequence boundary when adopting the replacement. It then
switches its in-memory active-file pointer without changing either pathname. Creating the replacement
synchronously is sufficient for this protocol; creating it ahead of time is a separate scheduling choice.
If segment creation fails, conventional rotation remains available as a fallback.

## Finalization

The previous file is immutable after handoff. The existing bounded deferred-close machinery completes
its finalization, preserving the sequence:

1. Finish archive slack deallocation and synchronize file contents.
2. Write STATE_ARCHIVED and synchronize the file again.
3. Rename the unique name to the conventional archive name without replacing an existing file, then
   synchronize the parent directory.

Only after archive publication does the file become an ordinary vacuum candidate. A failed rename
leaves its previous recoverable name intact. A synchronous client request drains outstanding deferred
finalization before synchronizing active persistent journals. Active unique names were already made
durable before adoption. A full deferred backlog still applies backpressure rather than spawning
unbounded workers. Orderly lifecycle close finalizes a unique active segment as an archive.

## Recovery and compatibility

Before opening new journals in a storage directory, recovery scans the exact unique segment patterns.
It removes never-adopted empty files and moves other stranded segments to the existing `.journal~`
convention without rewriting their contents. The scan preserves all potentially populated segments,
not just a selected newest file, and synchronizes its directory changes. Journald then opens fresh
conventional active files. Repeating recovery is safe.

Readers can continue using directory enumeration, the existing journal format and inotify notifications.
Existing file descriptors and mappings survive the final archive rename. Reader compatibility alone
is not writer downgrade compatibility: older journald versions do not perform this recovery scan.
Orderly finalization or recovery with a version that understands unique segments is needed to avoid
stranding these protected names when reverting to an older writer.
