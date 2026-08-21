from pathlib import Path


repository_root = Path(__file__).resolve().parent.parent

project = "zIPC"
copyright = "2026, zIPC contributors"
release = (repository_root / "VERSION").read_text(encoding="utf-8").strip()
version = release

extensions = [
    "breathe",
    "myst_parser",
]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

exclude_patterns = [
    "_build",
    "Thumbs.db",
    ".DS_Store",
    "CODEX-HANDOFF.md",
    "benchmark-data/**",
]
primary_domain = "c"
highlight_language = "c"

breathe_projects = {
    "zIPC": str(repository_root / "build" / "docs" / "doxygen" / "xml"),
}
breathe_default_project = "zIPC"
breathe_domain_by_extension = {"h": "c"}

html_theme = "alabaster"
html_title = f"zIPC {release} documentation"
html_static_path = []
