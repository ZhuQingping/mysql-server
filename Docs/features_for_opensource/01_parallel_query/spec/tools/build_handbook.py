#!/usr/bin/env python3
"""Build and check the commit-bound Parallel Query AI documentation bundle.

The script intentionally uses only the Python standard library.  It treats
manifest.yaml as a small, controlled schema rather than requiring PyYAML.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable


SCRIPT = Path(__file__).resolve()
SPEC_ROOT = SCRIPT.parents[1]
FEATURE_ROOT = SCRIPT.parents[2]
REPO_ROOT = SCRIPT.parents[5]
MANIFEST = SPEC_ROOT / "manifest.yaml"
GENERATED_DIR = SPEC_ROOT / "generated"
INVENTORY_OUT = GENERATED_DIR / "inventory.md"
HANDBOOK_OUT = GENERATED_DIR / "parallel_query_ai_handbook.md"

INNODB_PQ_FILES = (
    Path("storage/innobase/include/row0pread_pq.h"),
    Path("storage/innobase/row/row0pread_pq.cc"),
    Path("storage/innobase/handler/ha_innodb_pq.cc"),
)
BOUND_TREE_PATHS = (
    "sql/parallel_query",
    *(path.as_posix() for path in INNODB_PQ_FILES),
    "mysql-test/suite/parallel_query/t",
    "mysql-test/suite/parallel_query/r",
)
LINK_RE = re.compile(r"(?<!!)\[([^\]]+)\]\(([^)]+)\)")
CODE_SPAN_RE = re.compile(r"`([^`\n]+)`")


class BuildError(RuntimeError):
    """A deterministic documentation build or validation failure."""


def git(*args: str) -> str:
    proc = subprocess.run(
        ("git", *args),
        cwd=REPO_ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        raise BuildError(
            f"git {' '.join(args)} failed ({proc.returncode}): "
            f"{proc.stderr.strip()}"
        )
    return proc.stdout.rstrip("\n")


def parse_manifest() -> tuple[dict[str, dict[str, str]], list[str], dict[str, list[str]]]:
    """Parse scalar sections, AI defaults, and handbook_order from the manifest."""

    sections: dict[str, dict[str, str]] = {}
    order: list[str] = []
    ai_defaults: dict[str, list[str]] = {"include": [], "exclude": []}
    current_section = ""
    in_order = False
    current_ai_default = ""

    for line_number, raw in enumerate(
        MANIFEST.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if not raw.startswith(" ") and raw.endswith(":"):
            current_section = raw[:-1]
            in_order = current_section == "handbook_order"
            current_ai_default = ""
            sections.setdefault(current_section, {})
            continue
        if in_order:
            match = re.fullmatch(r"  - (.+)", raw)
            if not match:
                raise BuildError(
                    f"unsupported manifest handbook_order syntax at line {line_number}"
                )
            order.append(match.group(1).strip())
            continue
        if current_section == "ai_defaults":
            key_match = re.fullmatch(r"  (include|exclude):", raw)
            if key_match:
                current_ai_default = key_match.group(1)
                continue
            item_match = re.fullmatch(r"    - (.+)", raw)
            if item_match and current_ai_default:
                ai_defaults[current_ai_default].append(item_match.group(1).strip())
                continue
            raise BuildError(
                f"unsupported manifest ai_defaults syntax at line {line_number}"
            )
        match = re.fullmatch(r"  ([A-Za-z0-9_]+):\s*(.*)", raw)
        if match and current_section:
            value = match.group(2).strip()
            sections[current_section][match.group(1)] = value

    if not order:
        raise BuildError("manifest handbook_order is empty")
    return sections, order, ai_defaults


def sorted_relative_files(root: Path, pattern: str) -> list[Path]:
    return sorted(path.relative_to(REPO_ROOT) for path in root.rglob(pattern))


def tracked_files(pathspec: str) -> set[Path]:
    output = git("ls-files", "--", pathspec)
    return {Path(line) for line in output.splitlines() if line}


def file_line_count(paths: Iterable[Path]) -> int:
    return sum((REPO_ROOT / path).read_bytes().count(b"\n") for path in paths)


def tree_digest(paths: Iterable[Path]) -> str:
    """Match: sorted paths | xargs shasum -a 256 | shasum -a 256."""

    aggregate = hashlib.sha256()
    for relative in sorted(paths):
        digest = hashlib.sha256((REPO_ROOT / relative).read_bytes()).hexdigest()
        aggregate.update(f"{digest}  {relative.as_posix()}\n".encode("utf-8"))
    return aggregate.hexdigest()


def collect_inventory() -> dict[str, object]:
    sql_files = sorted_relative_files(REPO_ROOT / "sql/parallel_query", "*")
    sql_files = [path for path in sql_files if (REPO_ROOT / path).is_file()]
    source_files = sorted((*sql_files, *INNODB_PQ_FILES))

    tests = sorted_relative_files(
        REPO_ROOT / "mysql-test/suite/parallel_query/t", "*.test"
    )
    results = sorted_relative_files(
        REPO_ROOT / "mysql-test/suite/parallel_query/r", "*.result-pq"
    )
    tracked_tests = tracked_files("mysql-test/suite/parallel_query/t/*.test")
    tracked_results = tracked_files(
        "mysql-test/suite/parallel_query/r/*.result-pq"
    )

    return {
        "head": git("rev-parse", "HEAD"),
        "branch": git("branch", "--show-current") or "DETACHED",
        # The specification binds only the listed PQ source/test assets.
        # Local build trees and logs must not turn a reproducible source
        # baseline into a dirty snapshot.
        "dirty": bool(git("status", "--porcelain", "--", *BOUND_TREE_PATHS)),
        "sql_files": sql_files,
        "source_files": source_files,
        "sql_lines": file_line_count(sql_files),
        "innodb_lines": file_line_count(INNODB_PQ_FILES),
        "source_digest": tree_digest(source_files),
        "tests": tests,
        "results": results,
        "tracked_tests": tracked_tests,
        "tracked_results": tracked_results,
        "untracked_tests": sorted(set(tests) - tracked_tests),
        "untracked_results": sorted(set(results) - tracked_results),
        "test_digest": tree_digest((*tests, *results)),
    }


def manifest_int(section: dict[str, str], key: str) -> int:
    try:
        return int(section[key])
    except (KeyError, ValueError) as exc:
        raise BuildError(f"manifest inventory.{key} is missing or not an integer") from exc


def validate_snapshot(
    sections: dict[str, dict[str, str]], inventory: dict[str, object]
) -> None:
    snapshot = sections.get("snapshot", {})
    expected_inventory = sections.get("inventory", {})
    implementation_commit = snapshot.get("implementation_commit", "")
    try:
        git("rev-parse", "--verify", f"{implementation_commit}^{{commit}}")
    except BuildError as exc:
        raise BuildError(
            f"manifest implementation_commit is not a valid commit: "
            f"{implementation_commit!r}"
        ) from exc

    # Documentation may be committed after the implementation baseline.  It
    # remains valid when the declared PQ source/test paths are unchanged.
    changed_bound_paths = git(
        "diff",
        "--name-only",
        implementation_commit,
        "HEAD",
        "--",
        *BOUND_TREE_PATHS,
    )
    checks = {
        "branch": (snapshot.get("branch"), inventory["branch"]),
        "bound_source_test_delta": (
            "clean",
            "dirty" if changed_bound_paths else "clean",
        ),
        "working_tree": (
            snapshot.get("working_tree"),
            "dirty" if inventory["dirty"] else "clean",
        ),
        "source_tree_sha256": (snapshot.get("source_tree_sha256"), inventory["source_digest"]),
        "test_tree_sha256": (snapshot.get("test_tree_sha256"), inventory["test_digest"]),
        "sql_parallel_query_files": (
            manifest_int(expected_inventory, "sql_parallel_query_files"),
            len(inventory["sql_files"]),
        ),
        "sql_parallel_query_lines": (
            manifest_int(expected_inventory, "sql_parallel_query_lines"),
            inventory["sql_lines"],
        ),
        "innodb_pq_core_lines": (
            manifest_int(expected_inventory, "innodb_pq_core_lines"),
            inventory["innodb_lines"],
        ),
        "workspace_mtr_tests": (
            manifest_int(expected_inventory, "workspace_mtr_tests"),
            len(inventory["tests"]),
        ),
        "tracked_mtr_tests": (
            manifest_int(expected_inventory, "tracked_mtr_tests"),
            len(inventory["tracked_tests"]),
        ),
        "workspace_result_pq_files": (
            manifest_int(expected_inventory, "workspace_result_pq_files"),
            len(inventory["results"]),
        ),
        "tracked_result_pq_files": (
            manifest_int(expected_inventory, "tracked_result_pq_files"),
            len(inventory["tracked_results"]),
        ),
    }
    failures = [
        f"{name}: manifest={expected!r}, workspace={actual!r}"
        for name, (expected, actual) in checks.items()
        if expected != actual
    ]
    if failures:
        raise BuildError("snapshot drift detected:\n  " + "\n  ".join(failures))


def markdown_list(paths: Iterable[Path]) -> str:
    values = list(paths)
    if not values:
        return "- none"
    return "\n".join(f"- `{path.as_posix()}`" for path in values)


def render_inventory(
    sections: dict[str, dict[str, str]], inventory: dict[str, object]
) -> str:
    snapshot = sections["snapshot"]
    lines = [
        "# Parallel Query 生成清单",
        "",
        "> **GENERATED — DO NOT EDIT.** 由 `spec/tools/build_handbook.py` 生成；",
        "> 修改 manifest、当前规格源文档或源码/测试资产后必须重新生成。",
        "",
        "## 1. 绑定状态",
        "",
        "| 项目 | 值 |",
        "|---|---|",
        f"| implementation commit | `{snapshot['implementation_commit']}` |",
        f"| branch | `{inventory['branch']}` |",
        f"| bound PQ source/test tree | `{'dirty' if inventory['dirty'] else 'clean'}` |",
        f"| captured at | `{snapshot['captured_at']}` |",
        f"| source hash scope | `{snapshot['source_tree_scope']}` |",
        f"| external source evidence | {snapshot['external_source_evidence_scope']} |",
        f"| test hash scope | `{snapshot['test_tree_scope']}` |",
        f"| build verified | `{snapshot['build_verified']}` |",
        f"| MTR verified | `{snapshot['mtr_verified']}` |",
        "",
        "`dirty` 只表示 manifest 声明的 PQ source/test 路径存在未提交修改；",
        "无关 build/log 文件不影响本清单。运行验证范围见",
        "`current/quality/verification_evidence.md`，不得由生成动作自行推导。",
        "",
        "## 2. 源码与测试统计",
        "",
        "| 范围 | workspace | tracked | SHA-256 tree digest |",
        "|---|---:|---:|---|",
        f"| `sql/parallel_query/` | {len(inventory['sql_files'])} files / {inventory['sql_lines']} lines | n/a | `{inventory['source_digest']}`（含下列 InnoDB PQ 文件） |",
        f"| InnoDB PQ core | {len(INNODB_PQ_FILES)} files / {inventory['innodb_lines']} lines | n/a | 同上 |",
        f"| PQ `.test` | {len(inventory['tests'])} | {len(inventory['tracked_tests'])} | `{inventory['test_digest']}`（与 result 合并） |",
        f"| PQ `.result-pq` | {len(inventory['results'])} | {len(inventory['tracked_results'])} | 同上 |",
        "",
        "## 3. SQL/PQ 源文件",
        "",
        markdown_list(inventory["sql_files"]),
        "",
        "## 4. InnoDB PQ 核心文件",
        "",
        markdown_list(INNODB_PQ_FILES),
        "",
        "## 5. Workspace PQ 测试",
        "",
        markdown_list(inventory["tests"]),
        "",
        "## 6. Workspace PQ 期望结果",
        "",
        markdown_list(inventory["results"]),
        "",
        "## 7. 未跟踪 PQ 测试 overlay",
        "",
        "### `.test`",
        "",
        markdown_list(inventory["untracked_tests"]),
        "",
        "### `.result-pq`",
        "",
        markdown_list(inventory["untracked_results"]),
        "",
    ]
    return "\n".join(lines)


def resolve_link(source: Path, target: str) -> Path | None:
    target = target.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1]
    if not target or target.startswith(("#", "http://", "https://", "mailto:")):
        return None
    path_text = target.split("#", 1)[0]
    if not path_text:
        return None
    return (source.parent / path_text).resolve()


def validate_markdown_links(sources: Iterable[Path]) -> None:
    failures: list[str] = []
    for source in sources:
        content = source.read_text(encoding="utf-8")
        for match in LINK_RE.finditer(content):
            target = match.group(2)
            resolved = resolve_link(source, target)
            if resolved is not None and not resolved.exists():
                failures.append(
                    f"{source.relative_to(REPO_ROOT)} -> {target}"
                )
    if failures:
        raise BuildError(
            "broken local Markdown links:\n  " + "\n  ".join(sorted(failures))
        )


def validate_ai_defaults(ai_defaults: dict[str, list[str]], order: list[str]) -> None:
    """Keep the AI reading route and historical exclusions from silently drifting."""

    includes = ai_defaults["include"]
    excludes = ai_defaults["exclude"]
    if "README.md" not in includes:
        raise BuildError("manifest ai_defaults.include must contain README.md")
    if not includes:
        raise BuildError("manifest ai_defaults.include is empty")

    for relative in includes:
        path = (SPEC_ROOT / relative).resolve()
        if not path.is_file() or SPEC_ROOT not in path.parents:
            raise BuildError(
                f"manifest ai_defaults.include is missing or outside spec: {relative}"
            )
        if relative != "README.md" and relative not in order:
            raise BuildError(
                f"manifest ai_defaults.include is not in handbook_order: {relative}"
            )

    required_exclusions = {"../PQ-Feature-Spec.md", "../specs/"}
    missing_exclusions = required_exclusions - set(excludes)
    if missing_exclusions:
        raise BuildError(
            "manifest ai_defaults.exclude lacks historical exclusions: "
            + ", ".join(sorted(missing_exclusions))
        )


def validate_explicit_references(sources: Iterable[Path]) -> None:
    """Validate explicit repo paths and test filenames in inline code spans."""

    repo_prefixes = ("sql/", "storage/", "mysql-test/", "Docs/")
    test_dir = REPO_ROOT / "mysql-test/suite/parallel_query"
    tests_by_name = {
        path.name for path in (test_dir / "t").glob("*.test")
    }
    results_by_name = {
        path.name for path in (test_dir / "r").glob("*.result-pq")
    }
    failures: list[str] = []

    for source in sources:
        content = source.read_text(encoding="utf-8")
        for token in CODE_SPAN_RE.findall(content):
            value = token.strip()
            if value in {".test", ".result-pq"}:
                continue
            if value.endswith(".test") and "/" not in value:
                matched = (
                    any(fnmatch.fnmatch(name, value) for name in tests_by_name)
                    if any(char in value for char in "*?[")
                    else value in tests_by_name
                )
                if not matched:
                    failures.append(
                        f"{source.relative_to(REPO_ROOT)} -> missing test {value}"
                    )
                continue
            if value.endswith(".result-pq") and "/" not in value:
                matched = (
                    any(fnmatch.fnmatch(name, value) for name in results_by_name)
                    if any(char in value for char in "*?[")
                    else value in results_by_name
                )
                if not matched:
                    failures.append(
                        f"{source.relative_to(REPO_ROOT)} -> missing result {value}"
                    )
                continue
            if not value.startswith(repo_prefixes):
                continue

            path_text = value.split("::", 1)[0]
            path_text = re.sub(r":\d+$", "", path_text)
            if any(char in path_text for char in "*?["):
                if not list(REPO_ROOT.glob(path_text)):
                    failures.append(
                        f"{source.relative_to(REPO_ROOT)} -> unmatched path {value}"
                    )
            elif not (REPO_ROOT / path_text).exists():
                failures.append(
                    f"{source.relative_to(REPO_ROOT)} -> missing path {value}"
                )

    if failures:
        raise BuildError(
            "broken explicit source/test references:\n  "
            + "\n  ".join(sorted(set(failures)))
        )


def rewrite_links_for_handbook(content: str, source: Path) -> str:
    def replace(match: re.Match[str]) -> str:
        label, target = match.groups()
        resolved = resolve_link(source, target)
        if resolved is None:
            return match.group(0)
        anchor = "#" + target.split("#", 1)[1] if "#" in target else ""
        relative = os.path.relpath(resolved, HANDBOOK_OUT.parent)
        return f"[{label}]({Path(relative).as_posix()}{anchor})"

    return LINK_RE.sub(replace, content)


def render_handbook(
    sections: dict[str, dict[str, str]],
    order: list[str],
    sources: list[Path],
    inventory: dict[str, object],
) -> str:
    snapshot = sections["snapshot"]
    lines = [
        "# Parallel Query AI Handbook",
        "",
        "> **GENERATED — DO NOT EDIT.** 这是按 `manifest.yaml` 顺序拼装的单文件读取包。",
        "> 权威源位于 `spec/current/`；静态核对与运行验证范围以 manifest 和",
        "> `current/quality/verification_evidence.md` 为准。",
        f"> 输入提交：`{snapshot['implementation_commit']}`；分支：`{inventory['branch']}`；",
        f"> 绑定 PQ source/test 树：`{'dirty' if inventory['dirty'] else 'clean'}`；采集日期：`{snapshot['captured_at']}`。",
        f"> 源码树：`{inventory['source_digest']}`；PQ 测试树：`{inventory['test_digest']}`。",
        f"> 源码 hash 范围：`{snapshot['source_tree_scope']}`；其他接入点仅为 path::symbol 静态证据。",
        "",
        "## 输入文档",
        "",
    ]
    for relative, source in zip(order, sources):
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        target = os.path.relpath(source, HANDBOOK_OUT.parent)
        lines.append(
            f"- [`{relative}`]({Path(target).as_posix()}) — `{digest}`"
        )

    for relative, source in zip(order, sources):
        content = source.read_text(encoding="utf-8").rstrip()
        content = rewrite_links_for_handbook(content, source)
        lines.extend(
            (
                "",
                "---",
                "",
                f"<!-- BEGIN SOURCE: {relative} -->",
                "",
                content,
                "",
                f"<!-- END SOURCE: {relative} -->",
            )
        )
    lines.append("")
    return "\n".join(lines)


def expected_sources(order: list[str], excludes: Iterable[str]) -> list[Path]:
    """Resolve handbook inputs without allowing a historical-document escape."""

    excluded_paths = [(SPEC_ROOT / item).resolve() for item in excludes]
    sources: list[Path] = []
    failures: list[str] = []
    for item in order:
        path = (SPEC_ROOT / item).resolve()
        try:
            path.relative_to(SPEC_ROOT)
        except ValueError:
            failures.append(f"handbook_order escapes spec/: {item}")
            continue
        if any(path == excluded or excluded in path.parents for excluded in excluded_paths):
            failures.append(f"handbook_order includes excluded historical input: {item}")
            continue
        if not path.is_file():
            failures.append(f"handbook_order input is missing: {item}")
            continue
        sources.append(path)
    if failures:
        raise BuildError("invalid handbook_order:\n  " + "\n  ".join(failures))
    return sources


def compare_or_write(path: Path, expected: str, check: bool) -> bool:
    if check:
        actual = path.read_text(encoding="utf-8") if path.exists() else ""
        if actual != expected:
            print(f"OUT-OF-DATE: {path.relative_to(REPO_ROOT)}", file=sys.stderr)
            return False
        print(f"OK: {path.relative_to(REPO_ROOT)}")
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(expected, encoding="utf-8")
    print(f"WROTE: {path.relative_to(REPO_ROOT)}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate snapshot, links, and generated output without writing",
    )
    args = parser.parse_args()

    try:
        sections, order, ai_defaults = parse_manifest()
        inventory = collect_inventory()
        validate_snapshot(sections, inventory)
        validate_ai_defaults(ai_defaults, order)
        sources = expected_sources(order, ai_defaults["exclude"])
        validate_markdown_links([SPEC_ROOT / "README.md", *sources])
        validate_explicit_references(sources)
        inventory_text = render_inventory(sections, inventory)
        handbook_text = render_handbook(sections, order, sources, inventory)
        results = (
            compare_or_write(INVENTORY_OUT, inventory_text, args.check),
            compare_or_write(HANDBOOK_OUT, handbook_text, args.check),
        )
        if not all(results):
            return 1
    except (BuildError, OSError, UnicodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(
        "Static documentation validation complete; runtime claims are defined "
        "only by current/quality/verification_evidence.md."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
