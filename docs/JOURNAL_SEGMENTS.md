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

The writer transfers the old file's final sequence boundary when adopting the replacement, then switches
its active-file pointer without changing either pathname. Segment creation occurs synchronously during
rotation. If it fails, journald uses conventional rotation.

## Finalization

The previous file is immutable after handoff. The existing bounded deferred-close machinery completes
its finalization, preserving the sequence:

1. Finish archive slack deallocation and synchronize file contents.
2. Write STATE_ARCHIVED and synchronize the file again.
3. Rename the unique name to the conventional archive name without replacing an existing file, then
   synchronize the parent directory.

After the archive rename, the file is an ordinary vacuum candidate. Both file barriers must succeed before
that rename; the finalizer reports a failure without publishing if either fails. A failed rename leaves the
old recoverable name intact. A failed directory sync leaves an explicit pending obligation and the actual
renamed path in memory, so retries synchronize that directory without renaming onto the file itself or
repeating successful file barriers.

A finalization error, or failure to unlink an unused replacement, latches an error shared by all of the
manager's journals. Allocation of fresh unique names and adoption of replacement segments
stop; rotation uses the conventional path until daemon restart. Conventional rotation may reuse the current
active pathname, including a unique-format name. It archives its predecessor before creating the replacement.
The latch survives sync drains, journal eviction, configuration reopen and storage changes. Existing
finalizers may retry, but success does not clear the latch: previously closed files may still need startup
recovery.

After the error is latched, journald creates no new unique names. Files that were already active or in flight
remain recoverable and continue to count toward normal storage limits.

A synchronous request drains deferred finalization before syncing active persistent journals. Active unique
names are durable before adoption. A full deferred backlog applies backpressure rather than spawning more
workers. Orderly lifecycle close attempts to finalize a unique active segment as an archive.

## Recovery and compatibility

Before opening new journals in a storage directory, recovery scans the exact unique segment patterns.
Zero-length candidates are removed. Other candidates are removed only if a complete empty header and
zero-filled arena identify an unused replacement. The scan is bounded to 8 MiB per candidate; larger or
unfamiliar images, partial headers and contradictory metadata are preserved through the existing
`.journal~` convention without rewriting their contents. Vacuum applies normal retention to these suspect
files rather than unconditionally deleting them based on an empty entry counter. The scan preserves all
potentially populated segments, not just a selected newest file, and synchronizes its directory changes.
Journald then opens fresh conventional active files. Repeating recovery is safe.

Readers can continue using directory enumeration, the existing journal format and inotify notifications.
Existing file descriptors and mappings survive the final archive rename. Reader compatibility alone
is not writer downgrade compatibility: older journald versions do not perform this recovery scan.
Orderly finalization or recovery with a version that understands unique segments is needed to avoid
stranding these protected names when reverting to an older writer.

## Example lifecycle

The columns below show the journal header state and the writer's role for each file. A system journal starts
with one active file:

```text
system.journal                                                                      ONLINE    active
```

Synchronous rotation creates and synchronizes an empty replacement before handoff:

```text
system.journal                                                                      ONLINE    active
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    prepared
```

Handoff changes roles but not names or header states:

```text
system.journal                                                                      ONLINE    finalizing
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
```

The first finalization barrier synchronizes the old file's data without changing its header state. Journald
then writes `STATE_ARCHIVED` and synchronizes the file again:

```text
system.journal                                                                      ARCHIVED  pending
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
```

Publication gives the old file its conventional archive name and synchronizes the directory:

```text
system@7b36f68b69434866a7e889acef36a43e-0000000000000001-00064f2ab4c00000.journal   ARCHIVED  archive
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
```

The next synchronous rotation creates another replacement:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    prepared
```

After handoff, the first random name is finalizing and the second is active. Once finalization completes, the
first random name is replaced by a conventional archive name. Random identifiers are not retained in archive
names.

On startup, a proven unused empty replacement is deleted. Other unique candidates are preserved without
changing their header state. For example, an `ONLINE` file interrupted while active may become:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2@00064f2abc000000-74f8e48a7ae47d31.journal~  ONLINE    recovered
system.journal                                                                      ONLINE    new active
```

A file interrupted after the second finalization barrier may instead be recovered with an `ARCHIVED` header.
User journals follow the same lifecycle with a `user-<uid>` prefix.
