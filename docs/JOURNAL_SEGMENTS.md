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
its active-file pointer without changing either pathname. If no prepared segment is available, creation
occurs synchronously during rotation. If it fails, journald uses conventional rotation.

## Ahead-of-time preparation

Journald can start segment creation after an entry has been appended, while the current file continues
receiving entries. At most two preparations are retained globally across journal streams. Each worker
receives copied identity, pathname and policy inputs and performs only the descriptor-based creation
step. It does not access the active file, mmap cache, event sources or shared FSS state. It shares only an
owner-scoped atomic failure latch with other files and preparations. That owner outlives all workers.

A completed preparation is adopted at rotation only if it still belongs to the current file and policy.
Adoption uses the final sequence boundary, not the earlier snapshot. If no usable segment is ready,
rotation creates one synchronously using the same segment protocol rather than joining an unfinished
preparer. Worker failures also fall back to this path after cleanup. Lifecycle and storage changes,
configuration-driven reopen, memory pressure and shutdown discard unused preparations.

Workers, descriptors and reserved files are bounded independently of the number of UIDs. Reserved files
count toward filesystem usage.

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

A finalization error, or failure to unlink an unused preparation, latches an error shared by all of the
manager's journals and preparations. Allocation of fresh unique names and adoption of prepared segments
stop; rotation uses the conventional path until daemon restart. Conventional rotation may reuse the current
active pathname, including a unique-format name. It archives its predecessor before creating the replacement.
The latch survives sync drains, journal eviction, configuration reopen and storage changes. Existing
finalizers may retry, but success does not clear the latch: previously closed files may still need startup
recovery. Completed unused preparations are discarded without waiting for unfinished preparers on the ingress
path.

After the error is latched, journald creates no new unique names. Files that were already active or in flight
remain recoverable and continue to count toward normal storage limits.

A synchronous request drains deferred finalization before syncing active persistent journals. Active unique
names are durable before adoption. A full deferred backlog applies backpressure rather than spawning more
workers. Orderly lifecycle close attempts to finalize a unique active segment as an archive.

## Recovery and compatibility

Before opening new journals in a storage directory, recovery scans the exact unique segment patterns.
Zero-length candidates are removed. Other candidates are removed only if a complete empty header and
zero-filled arena identify an unused preparation. The scan is bounded to 8 MiB per candidate; larger or
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

After the first successful append, journald starts creating and synchronizing an empty replacement if a
preparation slot is available:

```text
system.journal                                                                      ONLINE    active
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    prepared
```

At rotation, a completed matching replacement becomes active. Handoff changes roles but not names or header
states:

```text
system.journal                                                                      ONLINE    finalizing
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
```

If no completed matching replacement is available, rotation creates one synchronously instead of waiting for
an unfinished preparation.

After the first successful append to the new active file, preparation of its successor may overlap
finalization of the old file:

```text
system.journal                                                                      ONLINE    finalizing
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    prepared
```

The first finalization barrier synchronizes the old file's data without changing its header state. Journald
then writes `STATE_ARCHIVED` and synchronizes the file again:

```text
system.journal                                                                      ARCHIVED  pending
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    prepared
```

Publication gives the old file its conventional archive name and synchronizes the directory:

```text
system@7b36f68b69434866a7e889acef36a43e-0000000000000001-00064f2ab4c00000.journal   ARCHIVED  archive
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    active
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    prepared
```

At the next rotation, the prepared successor becomes active:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    finalizing
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    active
```

The first successful append to that active file starts preparation of the following segment:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal                                     ONLINE    finalizing
system@4b3ef6704cff4c61af21ebfbd74a8c63.journal                                     ONLINE    active
system@81c58abc46d847e5a8cf478fa757cb62.journal                                     ONLINE    prepared
```

Finalization replaces each old random name with a conventional archive name. Random identifiers are not
retained in archive names.

On startup, a proven unused empty preparation is deleted. Other unique candidates are preserved without
changing their header state. For example, an `ONLINE` file interrupted while active may become:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2@00064f2abc000000-74f8e48a7ae47d31.journal~  ONLINE    recovered
system.journal                                                                      ONLINE    new active
```

A file interrupted after the second finalization barrier may instead be recovered with an `ARCHIVED` header.
User journals follow the same lifecycle with a `user-<uid>` prefix.
