# ADR-0260: Native Python Startup Budget

- Status: Accepted
- Date: 2026-11-14
- Module graph node: `implement-advanced-sql`
- Scope: the existing Windows `differential.read_compatibility_tool` owner
- Related contracts: ADR-0102/0106/0210/0259

## Observed boundary

PR #334 contains two independently accepted constructor corrections at
`5dab3978`, tree `5a52cbee`. Its complete selected local plan passed all fifteen
steps, including fast units, isolated sanitizer/GNU coverage, native
compatibility, fresh selected-source analysis and formatting.

Original CI43468837916 attempt1 passed eleven roles. Native Windows job
132035073776 stopped at2395/2410 tests when the compatibility-tool owner
exceeded its initial ten-second deadline. No `module-ready` or functional
test-entry record appeared.

The existing observer attributes about0.748s to `_hashlib` extension creation
and6.074s to extension execution. The standard-library unittest-ready record
appears7.408s after module entry, but full module readiness is not reached.
These observations establish that the failure belongs to startup/import
admission, not an observed functional-test assertion. They do not establish
a particular DLL, provider, filesystem, scheduler or security-product cause.

The user asks whether the Python startup timeout should increase. This
decision changes that resource policy explicitly, rather than claiming to
have optimized startup or inventing an engine correction.

Full quality43469261608 is a separate cancelled observation. Its30m57s job,
all46 Ninja action records and skipped formatting do not qualify that gate.
This startup-budget change neither explains nor repairs that cancellation.

## Decision

For this Windows owner only, change the initial CTest `TIMEOUT` from10 to30
seconds. Keep the existing `TIMEOUT_AFTER_MATCH` value of10 seconds and its
exact `test:null`, `phase:"module-ready"` readiness expression.

The initial budget covers interpreter startup, imports and discovery. The
existing same-process readiness boundary remains before every test body,
fixture and CLI child. Once that boundary is reached, CTest starts the
unchanged ten-second functional deadline. A later readiness record cannot
extend it. An import that never becomes ready still has a finite deadline.

The bounded total allowance changes from at most20 to at most40 seconds.
Startup allowance increases by20 seconds; this is a policy change, not
preservation of the previous whole-process ceiling. Thirty seconds is a
bounded engineering margin for cold-start variation, not a measured worst-case
guarantee. A later startup failure requires diagnosis, not another automatic
increase.

Preserve:

- The same interpreter-selection rule, import order, command and flags.
- `PROCESSORS 2`, working directory and all test/fixture/child populations.
- The ten-second functional limit, eight-second traceback timer and existing
  child deadlines.
- The exact readiness marker and its one-time transition.
- The non-Windows owner's combined ten-second deadline, adjacent CLI owner,
  all other test/model deadlines and every job budget.
- All diagnostics, assertions, failure classifications and original failures.

Add no wrapper, cache, prewarming process, alternate provider, new timer,
retry loop or change to production code. Reuse the existing Windows-specific
CTest property block. This narrowly supersedes ADR-0210's initial ten-second
allowance for this owner; the rest of that contract remains unchanged.

## Test-first correction and delivery

Update the existing actual-CMake registration test before changing the
registration. The native variant must expect initial30 and post-readiness10;
the non-native variant and adjacent CLI must remain10 without a new phased
deadline. Record the expected failure against the old native initial10 value.

Then make the smallest Windows-only property change and run the focused
integration control, complete owning policy/tool suites and existing
readiness/lifecycle checks. Retain real-CTest controls for delayed startup,
missing readiness, functional timeout, repeated readiness and nonzero exits.
Confirm that only the intended runtime property changes in actual discovery.
Do not add a long sleep to the shipped test or spend startup allowance on
functional work.

Reconcile unchanged production sources, binaries, configurations and complete
fast evidence under ADR-0102. Apply the actual correction and combined-PR
verification plans; a plan is not execution evidence. Configuration-independent
reuse still requires matching command/property/owner identities.

Admit one named final-gate remediation after the two accepted ADR-0259 commits
in PR #334, under ADR-0106. Design and implementation review precede a normal
corrective commit. Publication must qualify the actual new head through all
required hosted gates, including the complete native selection with the new
startup contract. A passing prefix or local observation does not replace it.

This is not a source-neutral replay exception. Do not rerun or reclassify the
failed original jobs, revive an exhausted allowance, waive the separate
quality gate, or claim merge/node completion before final qualification.
