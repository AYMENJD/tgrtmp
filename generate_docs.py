"""Generate the API reference pages at MkDocs build time.

Pages are written through mkdocs-gen-files (virtual overlay) so they never
land in git. Data comes from introspecting the installed tgrtmp: signatures
and docstrings are read live, so the reference can never drift from code.
"""

from __future__ import annotations

import html
import inspect
import re

import mkdocs_gen_files

import tgrtmp

_VERSION = tgrtmp.__version__

# (dotted path, kind) in display order.
ENTRIES = [
    ("tgrtmp.publish", "function"),
    ("tgrtmp.Player", "class"),
    ("tgrtmp.stream", "function"),
    ("tgrtmp.Publisher", "class"),
    ("tgrtmp.Source", "class"),
    ("tgrtmp.SourceError", "exception"),
    ("tgrtmp.connect", "function"),
    ("tgrtmp.Client", "class"),
]

_FIRST_LINE = re.compile(r"^\s*(.+?)\s*(?:\n|$)")


def _resolve(path: str):
    obj = tgrtmp
    for part in path.split(".")[1:]:
        obj = getattr(obj, part)
    return obj


def _doc(obj) -> str:
    text = inspect.getdoc(obj) or ""
    return text.replace("\n", " ").strip()


def _short(obj) -> str:
    match = _FIRST_LINE.match(inspect.getdoc(obj) or "")
    return match.group(1).rstrip(".") if match else ""


def _fmt(annotation) -> str:
    if annotation is inspect.Parameter.empty or annotation is None:
        return ""
    text = str(annotation).replace("typing.", "")
    text = text.replace("tgrtmp.controller.Player", "Player")
    text = text.replace("tgrtmp._core.Client", "Client")
    return f"`{text}`"


def _params(obj) -> list[tuple[str, str]]:
    try:
        sig = inspect.signature(obj)
    except (TypeError, ValueError):
        return []

    parts = []
    for name, param in sig.parameters.items():
        if name in ("self",):
            continue
        typ = _fmt(param.annotation)
        if param.default is not inspect.Parameter.empty:
            default = param.default
            shown = f'"{default}"' if isinstance(default, str) else repr(default)
            suffix = f" = `{shown}`"
            parts.append((name, f"{typ}{suffix}"))
        else:
            parts.append((name, typ))
    return parts


def _returns(obj) -> str | None:
    try:
        sig = inspect.signature(obj)
    except (TypeError, ValueError):
        return None
    return _fmt(sig.return_annotation) or None


def _sig_block(call: str, params: list[tuple[str, str]], returns: str | None) -> str:
    lines = ['<div class="api-sig" markdown>', "", html.escape(call) + "("]
    if params:
        lines.append("<br>")
        last = len(params) - 1
        for i, (name, typ) in enumerate(params):
            comma = "," if i < last else ""
            lines.append(f"&nbsp;&nbsp;{html.escape(name)}: {typ}{comma}<br>")
    closing = ")"
    if returns:
        closing += f" → {returns}"
    lines += [closing, "", "</div>", ""]
    return "\n".join(lines)


def _meta_p(text: str) -> str:
    return f'<p class="api-meta" markdown>{text}</p>'


def _members(cls) -> list[tuple[str, str, object]]:
    out = []
    for name, obj in vars(cls).items():
        if name.startswith("_"):
            continue
        if isinstance(obj, property):
            out.append((name, "property", obj))
        elif callable(obj):
            out.append((name, "method", obj))
    return out


def _write(path: str, body: str) -> None:
    with mkdocs_gen_files.open(path, "w") as fh:
        fh.write(body)
    mkdocs_gen_files.set_edit_path(path, "generate_docs.py")


def _entry_page(path: str, kind: str, obj) -> None:
    name = path.rsplit(".", 1)[-1]
    lines = [
        f"[](){{ #{path} }}",
        f"# `{name}`",
        "",
        _meta_p(f"{kind.capitalize()} · tgrtmp {_VERSION}"),
        "",
    ]

    doc = _doc(obj)
    if doc:
        lines += [doc, ""]

    if kind == "class":
        init = _params(obj.__init__ if obj.__init__ is not object.__init__ else obj)
        lines.append(_sig_block(name, init, None))

        members = _members(obj)
        if members:
            lines += ["## Members", "", '<div class="api-fields" markdown>', ""]
            for member_name, member_kind, member in members:
                member_doc = _short(member)
                lines += [
                    f"### {member_name} {{ #{path}.{member_name} }}",
                    "",
                    f'<p class="api-type" markdown>{member_kind}</p>',
                    "",
                ]
                if member_kind == "method":
                    lines.append(
                        _sig_block(
                            f".{member_name}",
                            _params(member),
                            _returns(member),
                        )
                    )
                if member_doc:
                    lines += [f'<p class="api-desc" markdown>{member_doc}</p>', ""]
            lines += ["</div>", ""]
    else:
        lines.append(_sig_block(f"{name}()", _params(obj), _returns(obj)))

    _write(f"reference/entries/{path}.md", "\n".join(lines))


def main() -> None:
    items = []
    for path, kind in ENTRIES:
        obj = _resolve(path)
        _entry_page(path, kind, obj)
        items.append((path, kind, obj))

    lines = [
        "# API reference",
        "",
        _meta_p(f"tgrtmp {_VERSION}"),
        "",
    ]
    for path, kind, obj in items:
        name = path.rsplit(".", 1)[-1]
        short = _short(obj)
        suffix = f"; {short}" if short else ""
        lines.append(f"- [`{name}`][{path}]{suffix}")
    lines.append("")

    _write("reference/index.md", "\n".join(lines))

    with mkdocs_gen_files.open("reference/SUMMARY.md", "w") as nav:
        nav.write("* [Overview](index.md)\n")

        for path, kind, _ in items:
            name = path.rsplit(".", 1)[-1]
            nav.write(f"* [{name}](entries/{path}.md)\n")


# mkdocs-gen-files uses runpy.run_path, which sets __name__ to "<run_path>"
# (Python 3.11+), not "__main__".
if __name__ in ("__main__", "<run_path>"):
    main()
