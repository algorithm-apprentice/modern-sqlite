#!/usr/bin/env python3

import pathlib
import re
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
STANDARD_CI_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/ci.yml"
FULL_QUALITY_WORKFLOW = REPOSITORY_ROOT / ".github/workflows/full-quality.yml"


class CiWorkflowPolicyTest(unittest.TestCase):
    def test_standard_ci_is_not_triggered_by_labels(self) -> None:
        workflow = STANDARD_CI_WORKFLOW.read_text(encoding="utf-8")

        self.assertNotRegex(workflow, re.compile(r"^      - labeled$", re.MULTILINE))
        self.assertNotRegex(
            workflow, re.compile(r"^  full-quality:$", re.MULTILINE)
        )

    def test_full_quality_has_isolated_triggers_and_job(self) -> None:
        workflow = FULL_QUALITY_WORKFLOW.read_text(encoding="utf-8")

        for trigger in (
            "  workflow_dispatch:",
            "  pull_request:",
            "  push:",
            "      - labeled",
            "      - synchronize",
            "      - reopened",
            "      - main",
        ):
            with self.subTest(trigger=trigger):
                self.assertIn(trigger, workflow)

        self.assertRegex(workflow, re.compile(r"^  full-quality:$", re.MULTILINE))
        self.assertIn("github.event.label.name == 'full-quality'", workflow)
        self.assertIn(
            "contains(github.event.pull_request.labels.*.name, 'full-quality')",
            workflow,
        )
        self.assertIn("group: full-quality-", workflow)


if __name__ == "__main__":
    unittest.main()
