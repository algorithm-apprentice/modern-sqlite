import copy
import json
import pathlib
import unittest

from tools.project_graph import (
    GraphValidationError,
    next_ready_node,
    validate_project,
    validate_repository,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_json(path: pathlib.Path) -> dict:
    with path.open(encoding="utf-8") as source:
        return json.load(source)


class ProjectGraphTest(unittest.TestCase):
    def setUp(self) -> None:
        self.graph = load_json(ROOT / "project/module-graph.json")
        self.progress = {
            "schemaVersion": 1,
            "completedNodes": ["document-architecture"],
            "activeNode": "bootstrap-build",
            "activeNodeState": "in_progress",
        }

    def test_repository_graph_and_progress_are_valid(self) -> None:
        validate_project(self.graph, self.progress)

    def test_repository_metadata_is_consistent(self) -> None:
        validate_repository(ROOT)

    def test_missing_dependency_is_rejected(self) -> None:
        graph = copy.deepcopy(self.graph)
        graph["nodes"]["bootstrap-build"]["dependsOn"] = ["missing-node"]

        with self.assertRaisesRegex(GraphValidationError, "missing-node"):
            validate_project(graph, self.progress)

    def test_malformed_node_definition_is_rejected(self) -> None:
        graph = copy.deepcopy(self.graph)
        graph["nodes"]["bootstrap-build"] = []

        with self.assertRaisesRegex(GraphValidationError, "node definition"):
            validate_project(graph, self.progress)

    def test_cycle_is_rejected(self) -> None:
        graph = copy.deepcopy(self.graph)
        graph["nodes"]["document-architecture"]["dependsOn"] = ["bootstrap-build"]

        with self.assertRaisesRegex(GraphValidationError, "cycle"):
            validate_project(graph, self.progress)

    def test_non_topological_canonical_order_is_rejected(self) -> None:
        graph = copy.deepcopy(self.graph)
        graph["canonicalOrder"][0], graph["canonicalOrder"][1] = (
            graph["canonicalOrder"][1],
            graph["canonicalOrder"][0],
        )

        with self.assertRaisesRegex(GraphValidationError, "canonicalOrder"):
            validate_project(graph, self.progress)

    def test_completed_node_with_incomplete_dependency_is_rejected(self) -> None:
        progress = {
            "schemaVersion": 1,
            "completedNodes": ["bootstrap-build"],
            "activeNode": None,
            "activeNodeState": None,
        }

        with self.assertRaisesRegex(GraphValidationError, "document-architecture"):
            validate_project(self.graph, progress)

    def test_active_node_is_returned_until_it_is_completed(self) -> None:
        self.assertEqual("bootstrap-build", next_ready_node(self.graph, self.progress))

    def test_active_node_requires_a_known_state(self) -> None:
        progress = copy.deepcopy(self.progress)
        progress["activeNodeState"] = "unknown"

        with self.assertRaisesRegex(GraphValidationError, "activeNodeState"):
            validate_project(self.graph, progress)

    def test_active_state_without_active_node_is_rejected(self) -> None:
        progress = copy.deepcopy(self.progress)
        progress["activeNode"] = None

        with self.assertRaisesRegex(GraphValidationError, "activeNodeState"):
            validate_project(self.graph, progress)

    def test_first_canonical_ready_node_is_selected(self) -> None:
        progress = {
            "schemaVersion": 1,
            "completedNodes": ["document-architecture", "bootstrap-build"],
            "activeNode": None,
            "activeNodeState": None,
        }

        self.assertEqual("implement-bytes", next_ready_node(self.graph, progress))


if __name__ == "__main__":
    unittest.main()
