#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections.abc import Mapping, Sequence
from typing import Any


class GraphValidationError(ValueError):
    """Raised when project graph or progress metadata is inconsistent."""


def _is_string_sequence(value: object) -> bool:
    return isinstance(value, list) and all(isinstance(item, str) for item in value)


def _find_cycle(nodes: Mapping[str, Mapping[str, Any]]) -> list[str] | None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(node_id: str, path: list[str]) -> list[str] | None:
        if node_id in visiting:
            cycle_start = path.index(node_id)
            return path[cycle_start:] + [node_id]
        if node_id in visited:
            return None

        visiting.add(node_id)
        path.append(node_id)
        node = nodes[node_id]
        dependencies = node.get("dependsOn", []) if isinstance(node, Mapping) else []
        if not _is_string_sequence(dependencies):
            dependencies = []
        for dependency in dependencies:
            if dependency in nodes:
                cycle = visit(dependency, path)
                if cycle is not None:
                    return cycle
        path.pop()
        visiting.remove(node_id)
        visited.add(node_id)
        return None

    for node_id in nodes:
        cycle = visit(node_id, [])
        if cycle is not None:
            return cycle
    return None


def validate_project(graph: Mapping[str, Any], progress: Mapping[str, Any]) -> None:
    errors: list[str] = []
    nodes = graph.get("nodes")
    canonical_order = graph.get("canonicalOrder")

    if not isinstance(nodes, dict):
        errors.append("nodes must be an object")
        nodes = {}
    if not _is_string_sequence(canonical_order):
        errors.append("canonicalOrder must be an array of node IDs")
        canonical_order = []

    if len(canonical_order) != len(set(canonical_order)):
        errors.append("canonicalOrder contains duplicate node IDs")
    order_ids = set(canonical_order)
    node_ids = set(nodes)
    if order_ids != node_ids:
        errors.append(
            "canonicalOrder and nodes differ: "
            f"order-only={sorted(order_ids - node_ids)}, "
            f"nodes-only={sorted(node_ids - order_ids)}"
        )

    required_fields = {
        "layer",
        "dependsOn",
        "deliverable",
        "acceptance",
        "performanceSensitive",
    }
    for node_id, node in nodes.items():
        if not isinstance(node, dict):
            errors.append(f"{node_id}: node definition must be an object")
            continue
        missing_fields = sorted(required_fields - set(node))
        if missing_fields:
            errors.append(f"{node_id}: missing fields {missing_fields}")
        dependencies = node.get("dependsOn")
        if not _is_string_sequence(dependencies):
            errors.append(f"{node_id}: dependsOn must be an array of node IDs")
            continue
        for dependency in dependencies:
            if dependency not in nodes:
                errors.append(f"{node_id}: dependency {dependency} does not exist")

    cycle = _find_cycle(nodes)
    if cycle is not None:
        errors.append(f"dependency cycle detected: {' -> '.join(cycle)}")

    positions = {node_id: index for index, node_id in enumerate(canonical_order)}
    for node_id, node in nodes.items():
        if not isinstance(node, dict):
            continue
        for dependency in node.get("dependsOn", []):
            if dependency in positions and node_id in positions:
                if positions[dependency] >= positions[node_id]:
                    errors.append(
                        f"canonicalOrder places {dependency} after its dependent {node_id}"
                    )

    completed_nodes = progress.get("completedNodes")
    active_node = progress.get("activeNode")
    active_node_state = progress.get("activeNodeState")
    if not _is_string_sequence(completed_nodes):
        errors.append("completedNodes must be an array of node IDs")
        completed_nodes = []
    if len(completed_nodes) != len(set(completed_nodes)):
        errors.append("completedNodes contains duplicate node IDs")
    completed = set(completed_nodes)
    unknown_completed = sorted(completed - node_ids)
    if unknown_completed:
        errors.append(f"completedNodes contains unknown IDs: {unknown_completed}")
    if active_node is not None and not isinstance(active_node, str):
        errors.append("activeNode must be a node ID or null")
    elif isinstance(active_node, str):
        if active_node not in nodes:
            errors.append(f"activeNode {active_node} does not exist")
        if active_node in completed:
            errors.append(f"activeNode {active_node} is already completed")
    if active_node is None:
        if active_node_state is not None:
            errors.append("activeNodeState must be null when activeNode is null")
    elif active_node_state not in {"in_progress", "review_pending"}:
        errors.append(
            "activeNodeState must be in_progress or review_pending when activeNode is set"
        )

    for node_id in completed:
        if node_id not in nodes:
            continue
        node = nodes[node_id]
        if not isinstance(node, dict):
            continue
        for dependency in node.get("dependsOn", []):
            if dependency not in completed:
                errors.append(
                    f"completed node {node_id} has incomplete dependency {dependency}"
                )
    if isinstance(active_node, str) and active_node in nodes:
        active_definition = nodes[active_node]
        dependencies = (
            active_definition.get("dependsOn", [])
            if isinstance(active_definition, dict)
            else []
        )
        for dependency in dependencies:
            if dependency not in completed:
                errors.append(
                    f"active node {active_node} has incomplete dependency {dependency}"
                )

    if errors:
        raise GraphValidationError("\n".join(errors))


def next_ready_node(graph: Mapping[str, Any], progress: Mapping[str, Any]) -> str | None:
    validate_project(graph, progress)
    active_node = progress["activeNode"]
    if active_node is not None:
        return active_node

    completed = set(progress["completedNodes"])
    nodes = graph["nodes"]
    for node_id in graph["canonicalOrder"]:
        if node_id in completed:
            continue
        if all(dependency in completed for dependency in nodes[node_id]["dependsOn"]):
            return node_id
    return None


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise GraphValidationError(f"{path}: top-level JSON value must be an object")
    return value


def validate_repository(root: pathlib.Path) -> None:
    manifest_path = root / "project/manifest.json"
    graph_path = root / "project/module-graph.json"
    progress_path = root / "project/progress.json"
    manifest = _load_json(manifest_path)
    graph = _load_json(graph_path)
    progress = _load_json(progress_path)
    validate_project(graph, progress)

    errors: list[str] = []
    active_node = progress["activeNode"]
    if manifest.get("currentNode") != active_node:
        errors.append("manifest currentNode differs from progress activeNode")
    if graph.get("currentNode") != active_node:
        errors.append("graph currentNode differs from progress activeNode")
    if manifest.get("currentNodeState") != progress.get("activeNodeState"):
        errors.append("manifest currentNodeState differs from progress activeNodeState")

    entry_points = manifest.get("entryPoints")
    if not _is_string_sequence(entry_points):
        errors.append("manifest entryPoints must be an array of paths")
    else:
        for entry_point in entry_points:
            if not (root / entry_point).is_file():
                errors.append(f"manifest entry point does not exist: {entry_point}")

    dependency_document = root / "docs/dependency-dag.md"
    documented_order = re.findall(
        r"^\d+\. `([^`]+)`$",
        dependency_document.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if documented_order != graph["canonicalOrder"]:
        errors.append("documented canonical order differs from module-graph.json")

    if errors:
        raise GraphValidationError("\n".join(errors))


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Validate and query the project DAG")
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
        help="Repository root",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate", help="Validate graph, progress, and repository metadata")
    next_parser = subparsers.add_parser("next-ready", help="Print the active or next ready node")
    next_parser.add_argument("--json", action="store_true", help="Emit a JSON object")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    root = args.root.resolve()
    try:
        validate_repository(root)
        graph = _load_json(root / "project/module-graph.json")
        progress = _load_json(root / "project/progress.json")
        if args.command == "validate":
            print(f"Validated {len(graph['nodes'])} project nodes.")
            return 0

        node_id = next_ready_node(graph, progress)
        if args.json:
            if node_id is None:
                state = "complete"
            elif node_id == progress["activeNode"]:
                state = progress["activeNodeState"]
            else:
                state = "ready"
            print(json.dumps({"node": node_id, "state": state}, sort_keys=True))
        elif node_id is not None:
            print(node_id)
        return 0
    except (GraphValidationError, OSError, json.JSONDecodeError) as error:
        print(f"project graph error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
