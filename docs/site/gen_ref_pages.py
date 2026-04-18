"""Generate one API reference page per module under ``src/cortex/``.

Executed by ``mkdocs-gen-files`` during the build. Emits:

- ``reference/<pkg>/<mod>.md`` for every non-dunder module,
- ``reference/<pkg>/index.md`` for every ``__init__.py``,
- ``reference/SUMMARY.md`` consumed by ``mkdocs-literate-nav``.

Keeping this generated means adding a new module needs zero doc edits.
"""

from pathlib import Path

import mkdocs_gen_files

# This script lives at ``docs/gen_ref_pages.py`` and is executed by
# mkdocs-gen-files with the mkdocs.yml directory as cwd. Anchor to the
# repo root so the generator finds ``src/cortex`` regardless of cwd.
REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO_ROOT / "src"
PACKAGE = "cortex"

nav = mkdocs_gen_files.Nav()

for path in sorted((SRC_ROOT / PACKAGE).rglob("*.py")):
    module_path = path.relative_to(SRC_ROOT).with_suffix("")
    doc_path = Path("reference", *module_path.parts[1:]).with_suffix(".md")
    parts = tuple(module_path.parts)

    if parts[-1] == "__init__":
        parts = parts[:-1]
        doc_path = doc_path.with_name("index.md")
    elif parts[-1].startswith("_"):
        continue

    nav_parts = parts[1:] if parts[1:] else ("cortex",)
    nav[nav_parts] = doc_path.relative_to("reference").as_posix()

    identifier = ".".join(parts) if parts else PACKAGE
    with mkdocs_gen_files.open(doc_path, "w") as f:
        f.write(f"# `{identifier}`\n\n")
        f.write(f"::: {identifier}\n")

    mkdocs_gen_files.set_edit_path(doc_path, path.relative_to(REPO_ROOT))

with mkdocs_gen_files.open("reference/SUMMARY.md", "w") as f:
    f.writelines(nav.build_literate_nav())
