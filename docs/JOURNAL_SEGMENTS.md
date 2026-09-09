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

A system journal initially uses the conventional active name:

```text
system.journal
```

After the first successful append, journald starts creating and synchronizing a uniquely named empty
replacement if a preparation slot is available:

```text
system.journal
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal
```

If no completed matching replacement is available at rotation, journald creates one synchronously instead
of waiting for an unfinished preparation.

After handoff, the names are unchanged: `system.journal` is immutable and finalizing, while the uniquely
named file is active. Once finalization completes, the old file is published under its conventional archive
name:

```text
system@7b36f68b69434866a7e889acef36a43e-0000000000000001-00064f2ab4c00000.journal
system@9f1f3eafec2f46e8b79aaac0bb58c6d2.journal
```

The next rotation creates another unique replacement. The old unique name is replaced by a conventional
archive name after its finalization; the random identifier is not retained in the archive name.

If recovery cannot prove that a unique candidate is an unused empty replacement, it preserves the file using
the existing unclean-file naming convention. For example:

```text
system@9f1f3eafec2f46e8b79aaac0bb58c6d2@00064f2abc000000-74f8e48a7ae47d31.journal~
```

User journals follow the same lifecycle with a `user-<uid>` prefix.
