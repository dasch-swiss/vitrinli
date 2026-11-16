---
title: "A cap on input bytes does not bound memory when parse state is copied per element"
date: 2026-11-16
category: "architecture"
component: "metadata"
module: "src/metadata/cpp/xmp.cpp (Xmp::merge_extended, the expat Scan)"
problem_type: "untrusted-input-amplification"
severity: "high"
symptoms: "ExtendedXMP read back from a JPEG was capped at 16 MiB and a scan was capped at 64 namespace bindings and 256 rdf:Description elements, yet a crafted file could still make one read allocate gigabytes."
root_cause: "Each Description copied the namespace bindings in scope, and the merge wrote them again onto every description. The caps counted bindings and descriptions but not the bytes of a binding, so 64 bindings of about 250 KB each, times 256 descriptions, multiplied the 16 MiB input about 256 times."
tags: [xmp, extended-xmp, jpeg, expat, untrusted-input, memory, amplification, caps, security, review]
issue: "DEV-7606"
---

# A cap on input bytes does not bound memory when parse state is copied per element

## What happened

DEV-7606 made the JPEG reader join an ExtendedXMP back into its StandardXMP
(`Xmp::merge_extended`). The parser it shares with the writer (`Scan` in
`xmp.cpp`) already capped bindings (64) and descriptions (256), with the
comment "every description copies the bindings in scope, so both are
capped". On the write path the input was at most a 64 KB packet Vitrinli
built itself, so the counts were enough.

On the read path the input is an untrusted file. The total input is capped
at 16 MiB, but one binding URI could be most of it. 64 long URIs copied into
256 descriptions, and then written as attributes onto each description in
the merged output, came to several GiB. Every IIIF request for that JPEG paid
it. The decode memory budget does not see metadata allocations, and the
decode deadline only abandons the thread.

The per-unit phase review did not catch it. The final whole-stack security
review did, because it asked how the new read path changes the trust level
of an existing parser.

## Fix

A namespace URI over 512 bytes stops the scan (`kMaxNamespaceUriBytes`), so
the copied state is at most 64 x 512 bytes per description. The merge also
checks that the parts add up to the declared length before it reserves that
length.

## Rule

When a parser moves from trusted to untrusted input, check the product of
the caps, not each cap alone: anything copied per element multiplies by the
element count. Cap the size of what is copied, or store an index instead of
a copy.
