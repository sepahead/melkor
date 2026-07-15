# Resource limits

Melkor treats every file as untrusted, and "untrusted" includes *well-formed but enormous*. A
40 GiB PLY that declares two billion splats is not malformed; it is simply larger than the machine
can survive. Refusing to bound that case lets a single file exhaust memory, fill a disk, or wedge a
machine, so resource exhaustion is **in scope** (this reverses the pre-v2 policy, release blocker
P0-12).

Every limit is enforced through a shared `Budget` (`include/melkor/budget.hpp`), charged before
the allocation it accounts for, so a new parser cannot opt out by forgetting to check.

## Profiles

Choose a named profile with `--limits-profile web|desktop|server`. The numbers below are safety
defaults, not scientific truths; they will be revisited against benchmark data before the final
release, and any change is a changelog entry because it affects which inputs are accepted.

| Limit | web | desktop | server |
|---|---:|---:|---:|
| Input bytes | 2 GiB | 4 GiB | 32 GiB |
| Decoded bytes | 2 GiB | 8 GiB | 64 GiB |
| Working memory | 1 GiB | 4 GiB | 16 GiB |
| Splats | 8 M | 25 M | 150 M |
| Mesh triangles | 16 M | 50 M | 300 M |
| Decompression ratio | 100 | 1000 | 1000 |
| Image dimension | 8192 | 16384 | 32768 |
| Threads | 4 | logical CPUs (≤32) | logical CPUs (≤32) |

The `web` profile is the tightest because a browser tab cannot swap and exceeding memory kills the
page. `server` is still bounded — "server" means the operator chose these numbers knowingly, not
"unlimited".

## No "disable all limits" switch

There is deliberately none. A custom profile may raise a limit, but checked arithmetic, structural
format limits, path containment, and output-integrity safeguards stay on regardless, and a limit
raised past the point where Melkor's own arithmetic can represent the result is rejected. A zeroed
limit is not read as "unlimited": it fails validation, because an all-zeros profile is the most
likely way someone accidentally disables resource accounting.

## Decompression bombs

An absolute decoded-byte cap alone is not enough: it lets a 1 KiB file expand all the way to the
cap. Before inflating anything, Melkor also checks the declared expansion against a ratio guard
(`declared_decoded > max_ratio · compressed`), so the *shape* of a bomb is refused, not only its
magnitude. A legitimately highly-compressible asset can trip it and needs a deliberate override.

## Diagnostics

A limit failure exits with code **6** and its diagnostic names the limit, the observed value, and
the flag that raises it — so the message tells you how to proceed, not merely that you may not.
