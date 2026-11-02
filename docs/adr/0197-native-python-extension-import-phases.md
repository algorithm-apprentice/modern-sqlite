# ADR-0197: Native Python Extension Import Phases

- Status: Accepted
- Date: 2026-11-03
- Module graph node: `implement-advanced-sql`
- Related contracts: ADR-0102, ADR-0106, ADR-0186, ADR-0194 and ADR-0195

## Observed boundary

PR #287's accepted INSERT-default sources and final premerge gates produce
normal main `3b5fa1b3`, tree `17e7beed`, at ledger190. Actual-main
CI41514181197 nevertheless fails the original native Python owner at10.13s
under its unchanged10s limit, before any of its29 test methods. All2222 native
unit bodies pass; the full selection stops after2242 of2257 cases. The
following15 cases and separate tooling are not execution evidence.

The existing CPython3.12.10 `-X importtime` output reports7358460 microseconds
inside `_hashlib` and7405760 microseconds cumulatively in `hashlib`. Existing
progress reaches unittest-ready around7.419s with31.25ms recorded owner CPU,
but never reaches stdlib-ready after `unittest.mock`. Import timing now
identifies an extension, not whether creation/loading or module execution
consumes that interval. No DLL, crypto-provider, security-software, filesystem
or OS cause has been established.

Three separate macOS jobs never acquire runners and have zero steps. Fresh
whole-quality41514181161 passes with an authenticated body, but neither that
success nor a zero-step infrastructure recovery can replace the failed native
owner. Actual-main acceptance and delivered-reference retirement remain open.

A parent-only experiment uses the existing signature-verified official
CPython3.12.10 and public `sys.setprofile` without replacing an import, loader
or provider. It observes `_imp.create_dynamic` and `_imp.exec_dynamic`
call/return events for `_hashlib` and `_ssl`. The spec/module is forwarded
through importlib's `_call_with_frames_removed` arguments. The first
zero-event collector assumption is preserved; its corrected observation
records all eight events and restores the profile under10s, without running
a suite or arming its watchdog. Local phases take milliseconds; this is not
a reproduction or explanation of the hosted stall.

## Decision

Reuse `read_compatibility_tool_test.py`'s existing progress reporter, clocks,
stderr channel and module-entry identity. Observe only the current thread
during the existing standard-library import span, and only on Windows.

The observer recognizes the existing `_imp.create_dynamic` and
`_imp.exec_dynamic` C-call events for `_hashlib` and `_ssl`. Decode the normal
direct or demonstrated forwarded spec/module argument, then report begin,
return or exception with the selected extension origin. A creation interval
includes loading and creation; it is not labelled as exclusively DLL loading.
Do not invoke or replace the operation, rewrite a spec, install another
importer, change module state or select a different provider.

Keep all original imports in their original relative order, including
`hashlib` and `unittest.mock`. Preserve the interpreter, `-X importtime`,
`-m unittest -v`, cwd, environment,29 methods, seven real CLI calls, every
assertion and the original10/8/1 limits. No preimport, warmup, DLL preread,
worker or additional watchdog moves initialization outside the CTest owner.
The non-Windows import path installs no profile handler.

A scoped owner installs its callback only when no profile handler exists.
If another handler is present, emit an explicit busy/unavailable diagnostic
without replacing, chaining or clearing it. A `finally` boundary retires
this owner's callback on normal completion or import/callback failure,
restoring its prior `None` state. Errors propagate; a failed operation must
not acquire a fabricated return/completion record.

This narrowly supersedes ADR-0195's no-profile-hook/module-unchanged clause
for these two extension phases. It changes no engine, crypto implementation,
provider, runner, security policy or validation limit. It is diagnostic
attribution, not a startup-latency fix or actual-main acceptance. A later
successful run does not explain an earlier failure or prove that host stalls
cannot recur.

## Test-first and delivery

Extend the existing isolated-import/progress test, retaining the29-method
population and all existing controls. A focused red precedes the observer.
Controlled direct/forwarded frames cover both target extensions, creation
and execution, begin/return/exception, unrelated-event exclusion, normal and
exceptional callback retirement, a pre-existing non-callable profile owner,
and non-Windows non-installation.

Cached reimport remains the no-suite/no-timer/population control, not proof
of cold-extension observation. The version-matched experiment supplies that
separate diagnostic evidence. Use the real native owning suite at its10s
limit and the applicable tooling/non-Windows controls. Reconcile unchanged
C++ sources, configurations, images and complete fast evidence; do not
reexecute or claim new C++ qualification for a Python-only correction.

Require independent exact-source review, a normal corrective source/PR,
fresh exact-head hosted/manual/integrated/main gates and qualified-descendant
retirement of the retained #287 source references. Preserve the failed main
and every incomplete or unsuccessful observation. No later production
prerequisite begins while this actual-main blocker remains unresolved.
