# ADR-0211: Pinned Intel macOS CI

- Status: Accepted
- Date: 2026-11-04
- Module graph node: `implement-advanced-sql`
- Prerequisite: accepted source222 `34a2bea`, ledger222, draft PR #294
- Related contracts: ADR-0009, ADR-0102/0106, ADR-0153 and ADR-0210

## Observed delivery blocker and existing contracts

CI41854069814 attempt1 executes and passes the Windows and Linux bodies.
Four macOS contexts never acquire hosted runners: their records have zero
steps, runner_id0 and explicit acquisition/capacity annotations. One reviewed
failed-only infrastructure recovery also ends with those four contexts
unexecuted. That recovery allowance is exhausted. Neither failed attempt
is reclassified as successful or requested again.

The user asks how to proceed instead of remaining blocked on that capacity.
GitHub documents `macos-latest` as arm64 and provides `macos-26-intel` as a
standard Intel runner. This public repository can use standard runners
without adopting a paid larger-runner configuration. A supported label is
not proof that a machine is immediately available.

ADR-0009 specifies Apple Clang on macOS, not a mandatory ARM64 CI profile.
Existing ARM64 reference measurements remain architecture-specific evidence,
not interchangeable machine timings. The current workflows use floating
macOS labels in the Debug/Release matrix, native-contract matrix and both
quality jobs. Their ccache keys distinguish OS/build configuration but not
architecture. The same canonical key scheme owns all three cache consumers.

## Decision

### Explicit macOS execution profile

Replace the five macOS runner selections in standard CI and Full quality
with `macos-26-intel`. Keep the same macOS major release, while explicitly
admitting x86_64 as this delivery's hosted macOS profile. The label does not
pin an image patch or toolchain patch; record actual hosted identities.

Retain every Debug/Release, native compatibility/benchmark, changed-source
quality and full-production quality command and deadline. Preserve Windows,
Linux, sanitizer, optional regression/fuzz configuration and all original
corpora, assertions, resource ceilings and seeds. Keep the existing PR
nonexecution policy for Ubuntu Debug and macOS native contracts. Those two
green contexts still do not represent executed bodies.

The matrix context names reflect the explicit runner label; the nine
standard-CI roles remain. Do not edit repository protection/rulesets, add
continue-on-error, skip a new owner or weaken any gate to obtain a merge.
The four existing macOS tool-installation bodies explicitly select
`shell: bash` and first execute `uname -m | grep -Fx x86_64`. This records
and checks the actual architecture before existing tool selection.
Explicit Bash supplies `-eo pipefail`; the unspecified default supplies
only `-e` and could hide a failed `uname` behind a successful `grep`.
The guard controls use the same no-profile/no-rc and error-mode options.
No new runner, dispatcher, platform abstraction or validation service is
introduced.

This is an intentional architecture-profile change, not equivalent ARM64
qualification. Retain historical ARM64 results with their original identities.
Do not claim fresh ARM64 execution, rewrite its benchmark archives, or
compare x86_64 elapsed times with ARM64 as a speedup. Future ARM64-specific
requirements need their own actual execution; this change claims only the
named profile.

### Architecture-owned compiler caches

Extend the existing ccache key and restore prefix uniformly to
OS/architecture/build-configuration/source-configuration identity by adding
`${{ runner.arch }}` to all three consumers. Keep the current cache directory,
size, compression, action pin and source-configuration hash.

There is no architecture-free restore fallback and no migration of old
ARM64 cache entries into the Intel profile. Cache hits remain build aids,
not execution evidence. A cache miss is not a reason to raise a deadline.

## Test-first, qualification and delivery

Add focused controls in the existing workflow-policy owner before changing
the workflows. Verify the actual matrix selections, both quality runners,
architecture checks, unchanged commands/limits/PR policy and all three
architecture-specific cache identities. Exercise the extracted architecture
check with correct/incorrect host output and query failure on the existing
Linux/macOS test hosts. Windows retains declaration checks without claiming
macOS shell execution. Preserve the complete existing owning selection.

Run the selected local tooling plan and complete applicable Python owners.
Reconcile unchanged C++ sources, images, configurations and complete fast
evidence; do not pretend local Windows/Linux or historical ARM64 execution
qualifies the new Intel host.

After independent source acceptance, make one normal source223 commit in
PR #294 and retain source222 as its accepted predecessor. This is a genuine
workflow/configuration repair, not a source-neutral retry of either old
attempt. Qualify the exact new head through all nine standard-CI contexts
and an actual Intel Full quality body. Authenticate the Intel architecture,
checkout, toolchain, complete runtime/quality bodies and all intentional skips.
Normal squash and actual-main qualification remain mandatory; actual main
also executes the previously skipped macOS native contracts before retirement.

Runner acquisition or a new configuration failure remains an explicit
failure. Do not silently restore ARM64, change providers or request repeated
attempts. General SQL prerequisites, IN and node43 remain incomplete.

## Qualification follow-up: preserve existing typed state

Source223 `dc94e37`, tree `4e12bf00`, passes all nine ordinary CI contexts,
including actual Intel Debug/Release and changed-source quality bodies.
The separate Intel Full quality run41869309910 executes but stops on four
`bugprone-optional-value-conversion` findings in binder and storage inspection.
Its actual analyzer is Homebrew LLVM22.1.8_2 with AppleClang21, not the existing
local LLVM23/libc++ configuration.

A complete44-translation-unit LLVM22 inventory under both existing local
GNU and libc++ compile databases distinguishes their diagnostics. Nine
same-optional transfers are mapped across binder, inspector, lowering,
optimizer, transaction and VM owners. Retain the checked optional directly
instead of unwrapping its scalar and reconstructing the same optional type.
Keep every presence check, typed error, required scalar/Result dereference,
visit order, writer epoch, rollback/trip decision and publication condition.
The affected values are existing IDs, sizes, an error code and a rowid;
no public type, allocation, persistent state transition or algorithm changes.

The aggregate-state helper also uses exact pointer deduction from
`std::get_if<State>`: `auto* const` retains the mutable State pointee and
top-level pointer constness. Its `Result<State*>` return and mutating
consumers stay unchanged; do not apply the analyzer's incompatible suggestion
to constify the pointee. An owned, one-declaration VFS-overlay control passes
the unchanged LLVM22/libc++ checks without modifying production.

The GNU diagnostic target's other exception-escape, uninitialized-assignment
and use-auto observations do not reproduce in the complete libc++ target.
Preserve those observations and configuration differences; do not broaden
this bounded repair, disable checks or call either local target equivalent
to the hosted Apple SDK. Actual hosted qualification remains required.

This is a behavior-preserving follow-up from the authenticated green ordinary
CI baseline, not a new behavior or an artificial failing-test exercise.
Use existing focused query/ordering/inspection/rollback/event/aggregate
controls, complete fast tiers and the actual source/batch verification plans.
Do not downgrade full-plan escalation for diagnostics or durability paths.
Keep all quality checks, warnings-as-errors, workloads and deadlines, then
qualify the new final head locally and on the declared hosted profiles before
normal integrated acceptance, squash and actual-main closure.

## References

- [GitHub-hosted runner labels and architectures](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [macOS 26 Intel image software](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-Readme.md)
- [GitHub Actions shell and pipeline failure semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idstepsshell)
