# ADR-0066: Markdown-Only Workflow Skipping

- Status: Accepted
- Date: 2026-10-12

## Context

The standard CI and full-quality workflows currently run for every configured
pull-request and `main` push event, even when every changed file is Markdown.
Those changes cannot alter the C++ build, sanitizer instrumentation, runtime
behavior, benchmark binaries, or clang-tidy results.

Running the complete build, sanitizer, quality, and performance matrix for a
documentation-only change delays review without adding executable evidence.
Documentation still requires accuracy, consistency, reference, and
dependency-order review, but those checks are performed through design and
content review rather than C++ execution.

GitHub path filtering is evaluated over the complete changed-file set. A
workflow is skipped only when every changed path matches `paths-ignore`; one
nonignored path causes the workflow to run normally.

## Decision

### 1. Skip automatic workflows for Markdown-only changes

The `pull_request` and `push` triggers in both `.github/workflows/ci.yml` and
`.github/workflows/full-quality.yml` ignore:

```yaml
paths-ignore:
  - "**/*.md"
```

This covers repository documentation, ADRs, benchmark reports, fixture
README files, and root Markdown files.

### 2. Fail closed for every non-Markdown artifact

Only the `.md` extension is ignored. Changes to source, headers, CMake,
workflow YAML, Python tools, JSON project metadata, text fixtures, generated
evidence, or any other file type continue to trigger the complete applicable
workflow.

A pull request that changes both Markdown and any non-Markdown file also runs
the complete workflow.

### 3. Preserve manual full-quality execution

`workflow_dispatch` remains unfiltered. A maintainer can still request the
full production clang-tidy workflow for any revision.

The existing `full-quality` pull-request label remains event-filtered by the
same changed-file policy. Applying it to a Markdown-only pull request does not
run production clang-tidy because no production artifact changed.

### 4. Lock both workflows and automatic events with a policy test

The workflow-policy test extracts the `pull_request` and `push` event blocks
from both workflow files and requires the exact Markdown ignore pattern.
This prevents one workflow or one automatic event from silently regaining the
expensive documentation-only path.

### 5. Revisit the policy if required status checks are introduced

The active default-branch ruleset currently protects deletion and non-fast-
forward updates but requires no status checks. GitHub documents that a
workflow skipped by path filtering remains pending if that workflow is
configured as a required check. Required checks must therefore be designed
around this policy before such a rule is enabled.

## Rejected alternatives

### Ignore the entire `docs/` directory

Rejected because extension-based filtering is narrower and continues to run
CI if a future non-Markdown tool, fixture, or generated artifact is placed
under `docs/`.

### Add a changed-file classification job

Rejected for the current repository because it would still allocate a runner
for every Markdown-only change. There are no required checks that need a
success result from a lightweight replacement job.

### Skip workflows through commit-message directives

Rejected because manual directives are easy to forget, can be applied to
mixed code changes, and do not encode the repository policy.

### Ignore `.txt`, `.json`, or YAML files

Rejected because those extensions include compatibility fixtures, project
metadata, workload contracts, and CI configuration that require validation.

## Consequences

### Positive

- Markdown-only pull requests and `main` pushes consume no automatic CI
  runners.
- Mixed changes retain every existing gate.
- The rule is narrow, visible in each workflow, and mechanically tested.
- Manual full-quality validation remains available.

### Negative

- Markdown syntax and links are not automatically validated because the
  repository has no dedicated documentation checker.
- Enabling required status checks later requires a compatible lightweight
  check design or removal of trigger-level path filtering.

## References

- ADR-0005: Test-Driven Development
- ADR-0006: AI-Native Sequential Workflow
- ADR-0046: Canonical Engineering and Reviewable Delivery
- ADR-0064: Production-Only Clang-Tidy Scope
- ADR-0065: Production-Only Sanitizer Instrumentation
- GitHub Actions workflow syntax: path filters
