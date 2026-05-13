#!/usr/bin/env python3
"""Generate one API reference page per module under ``src/cortex/``.

Writes static Markdown files into ``docs/reference/`` that mkdocstrings
renders into the API reference section of the site. Run this once before
``zensical build``; CI does it automatically via the docs workflow.

Why a standalone script: ``zensical`` (this project's static site
generator) doesn't run ``mkdocs-gen-files`` / ``mkdocs-literate-nav`` /
``mkdocs-section-index``. So we materialise the reference pages on disk
ourselves. Generated files are committed to keep the docs build
reproducible without re-running the generator.

Usage:
    python docs/gen_ref_pages.py [--check]

    --check  Exit non-zero if the generated tree differs from what is
             checked in.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO_ROOT / "src"
DOCS_DIR = Path(__file__).resolve().parent
REFERENCE_DIR = DOCS_DIR / "reference"
PACKAGE = "cortex"


def collect_modules() -> list[tuple[Path, str]]:
    """Walk the source tree and return (doc_path, identifier) pairs.

    ``doc_path`` is the .md path relative to ``docs/reference/``.
    ``identifier`` is the dotted module path passed to mkdocstrings.
    """
    out: list[tuple[Path, str]] = []
    for path in sorted((SRC_ROOT / PACKAGE).rglob("*.py")):
        module_path = path.relative_to(SRC_ROOT).with_suffix("")
        doc_path = Path(*module_path.parts[1:]).with_suffix(".md")
        parts = tuple(module_path.parts)

        if parts[-1] == "__init__":
            parts = parts[:-1]
            doc_path = doc_path.with_name("index.md") if doc_path.parts else Path(
                "index.md")
        elif parts[-1].startswith("_"):
            continue

        identifier = ".".join(parts) if parts else PACKAGE
        out.append((doc_path, identifier))
    return out


def render(modules: list[tuple[Path, str]]) -> dict[Path, str]:
    """Render every (doc_path -> content) the reference tree needs.

    Module pages get a heading + the mkdocstrings directive. The top-level
    index.md additionally lists every submodule for navigation.
    """
    pages: dict[Path, str] = {}

    # Per-module pages.
    for doc_path, identifier in modules:
        pages[doc_path] = f"# `{identifier}`\n\n::: {identifier}\n"

    # Replace the top-level index.md with a hand-shaped index. We avoid
    # running mkdocstrings on the top-level `cortex` package here because
    # its __init__ pulls in zmq/numpy/msgpack — fine inside a per-module
    # page, but it would inflate the index unnecessarily.
    top_lines = [
        "# API reference",
        "",
        "Auto-generated from the `cortex` package source. Re-run "
        "`python docs/gen_ref_pages.py` when modules are added or renamed.",
        "",
    ]
    # Group entries by their top-level subpackage (core / discovery / messages /
    # utils) so the listing is scannable.
    by_subpackage: dict[str, list[tuple[Path, str]]] = {}
    for doc_path, identifier in modules:
        if identifier == PACKAGE:
            continue
        parts = identifier.split(".")
        sub = parts[1] if len(parts) >= 2 else PACKAGE
        by_subpackage.setdefault(sub, []).append((doc_path, identifier))

    for sub in sorted(by_subpackage):
        top_lines.append(f"## `cortex.{sub}`")
        top_lines.append("")
        for doc_path, identifier in sorted(by_subpackage[sub]):
            rel_link = doc_path.as_posix()
            top_lines.append(f"- [`{identifier}`]({rel_link})")
        top_lines.append("")
    pages[Path("index.md")] = "\n".join(top_lines)

    return pages


def write_pages(pages: dict[Path, str]) -> None:
    if REFERENCE_DIR.exists():
        shutil.rmtree(REFERENCE_DIR)
    REFERENCE_DIR.mkdir(parents=True)
    for rel_path, content in pages.items():
        out = REFERENCE_DIR / rel_path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(content)


def check_pages(pages: dict[Path, str]) -> int:
    drift: list[str] = []
    expected_paths = {Path(p) for p in pages}
    actual_paths = {
        p.relative_to(REFERENCE_DIR)
        for p in REFERENCE_DIR.rglob("*.md")
    } if REFERENCE_DIR.exists() else set()

    for missing in sorted(expected_paths - actual_paths):
        drift.append(f"missing: docs/reference/{missing.as_posix()}")
    for extra in sorted(actual_paths - expected_paths):
        drift.append(f"unexpected: docs/reference/{extra.as_posix()}")
    for rel in sorted(expected_paths & actual_paths):
        on_disk = (REFERENCE_DIR / rel).read_text()
        if on_disk != pages[rel]:
            drift.append(f"out of date: docs/reference/{rel.as_posix()}")

    if drift:
        print("error: docs/reference/ is out of sync with src/cortex/:", file=sys.stderr)
        for line in drift:
            print(f"  - {line}", file=sys.stderr)
        print(
            "Run `python docs/gen_ref_pages.py` and commit the result.",
            file=sys.stderr,
        )
        return 1
    print(f"ok: docs/reference/ matches src/cortex/ ({len(pages)} pages)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if the generated tree differs from disk.",
    )
    args = ap.parse_args()

    modules = collect_modules()
    pages = render(modules)

    if args.check:
        return check_pages(pages)

    write_pages(pages)
    print(f"wrote {len(pages)} pages under {REFERENCE_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
