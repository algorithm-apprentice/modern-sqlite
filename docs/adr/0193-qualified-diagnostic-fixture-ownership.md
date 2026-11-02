# ADR-0193: Qualified Diagnostic Fixture Ownership and Draft Disposition

- Status: Accepted
- Date: 2026-11-02
- Module graph node: `implement-advanced-sql`
- Base: qualified PR #283 main `b766f6a7`, tree `b2615241`
- Related contracts: ADR-0106, ADR-0153 and ADR-0178

## Context and completed inventory

PR #269 is an unmerged diagnostic follow-up based on the older PR #268 main.
Its final Windows gate failed; its local results and four source commits are
not final delivery acceptance. Later qualified main does not implicitly
qualify that branch. The user requested an explicit disposition rather than
an indefinitely retained draft.

The current diagnostic `TemporaryDatabase` still constructs a timestamp and
process-local sequence filename, then opens it with truncation. That does not
reserve ownership across processes. Current fuzz/platform tests already share
one `PrivateDatabase` owner with exclusive file/directory creation, exact
bounded children and lifetime cleanup. Reuse it instead of creating another
temporary-file algorithm.

The old fixture correction in `7dc5664` remains directly applicable.
Current `tests/fuzz/private_database.hpp` retains the same POSIX mechanism:
`mkdtemp`, an owned directory descriptor, exclusive/no-follow `openat`,
complete EINTR-aware writes, and named-child cleanup before directory removal.
Current `tests/support/windows_fuzz_database.hpp` retains bounded random-name
exclusive directory creation, a pinned non-reparse directory handle,
exclusive/no-follow NativeVfs creation, owner-backed UTF-8 paths, and cleanup
of exactly its four owned children.

Existing consumers are read database fuzz, read smoke fixtures, write SQL
fuzz, write database fuzz and the native Windows owner test. The verification
layering test restricts their public-session/native-fixture boundaries.
No new platform algorithm, error policy, resource bound or consumer is needed.

## Decision

Promote the canonical header from fuzz to shared test support and its
namespace from `fuzz::detail` to `test_support`. Update every dependent
include/type, the formatter input and the exact layering whitelist paths.
Do not retain a forwarding compatibility header or duplicate owner.
Preserve both platform method bodies, limits, file flags and cleanup order;
namespace qualification changes are mechanical, not resource redesign.

The diagnostic fixture owns that same `PrivateDatabase`. Initialization
failure throws the existing test-fixture error type. It keeps an owner-backed
UTF-8 view for NativeVfs calls and derives its filesystem path from UTF-8,
never through a locale-dependent narrow path conversion. Read back the
created image and compare it byte-for-byte before an inspection can run.
Failure during initialization, conversion or readback unwinds the same owner.

Retain every existing corruption assertion. Verify the assumed original
schema type before mutating it and attach the actual inspection JSON to
failed-assertion context. This improves the fixture's ownership and failure
evidence; it does not establish a causal explanation for the old macOS
assertion or a hosted timeout.

This is one coherent, bounded test-ownership repair in a new pull request,
not a fifth commit appended to the historical four-slice batch. It changes no
production library, storage/transaction algorithm, SQL behavior, workload,
corpus, worker, scheduler, assertion ceiling or deadline.

## Disposition of all four old commits

1. `c34b7c9`: retire its unmerged pager/B-tree progress-only additions. They
   contain no correctness or workload fix. Do not introduce the old
   `PhaseTiming` alongside current shared `TestProgress`.
2. `7dc5664`: re-deliver the fixture ownership, readback, UTF-8 lifetime and
   corruption context through this qualified successor. Do not copy its
   obsolete truth-delivery status or its `PhaseTiming` formatter entry.
3. `67115a6`: retire its unmerged per-input write-smoke tracing as investigative
   instrumentation. Keep the current corpus, failure policies and original
   smoke deadline; do not call the historical timeout diagnosed or waived.
4. `8b714ea`: retain main's existing replacement from PR #274 / `26a0002`,
   `TestProgress` and complete original deterministic write-model action
   diagnostics, rather than adding a second progress implementation.

Retiring unmerged optional instrumentation does not remove diagnostics or
assertions already on main. Historical failed runs remain failed evidence.

## Test-first, review and delivery

Establish a fresh green diagnostic/private-owner baseline on the current
qualified source. Add one focused runtime regression for independently owned
fixture directories and lifetime cleanup; the old shared-parent fixture must
fail before the implementation changes. Then use the canonical owner and
prove exact independent image contents, sibling survival, private-directory
cleanup, empty input and owner-backed UTF-8 path lifetime.

Authenticate the mechanical platform-owner transformation and unchanged
existing consumers. Run all old corruption tests, complete fast units,
native Windows private-owner controls, layering checks, both smoke domains,
relevant sanitizer/compiler selections and the original bounded fuzz gates.
Select closing checks from the actual change-aware plan. Preserve all
existing thresholds, assertions, workloads and process limits.

Review the design before implementation and obtain independent source review
before publication. Keep the source, runtime red/green, complete qualification,
review and node/ADR identities in the append-only ledger.

After normal squash and fresh actual-main qualification, link the exact
successor in #269, close the old draft, and delete only its verified unchanged
local/remote/tracking references plus the delivered successor references.
Preserve the exact test-only ADR-0192 work separately and resume its held
design afterward. Node43 and IN admission remain incomplete.
