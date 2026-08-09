#!/usr/bin/env python3
"""Render features.md → features.pdf via Mermaid SVG + Chrome headless."""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MD_PATH = ROOT / "features.md"
PDF_PATH = ROOT / "features.pdf"
BUILD_DIR = ROOT / "docs" / ".features-pdf-build"

CHROME_CANDIDATES = (
    "google-chrome-stable",
    "google-chrome",
    "chromium",
    "chromium-browser",
)


def find_chrome() -> str:
    for name in CHROME_CANDIDATES:
        path = shutil.which(name)
        if path:
            return path
    raise RuntimeError("No Chrome/Chromium found for PDF export")


def extract_mermaid_blocks(md: str) -> list[str]:
    return re.findall(r"```mermaid\s*\n(.*?)```", md, re.DOTALL)


def render_mermaid_svgs(blocks: list[str], out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    svgs: list[Path] = []
    for i, block in enumerate(blocks):
        mmd = out_dir / f"diagram-{i}.mmd"
        svg = out_dir / f"diagram-{i}.svg"
        mmd.write_text(block.strip() + "\n", encoding="utf-8")
        subprocess.run(
            ["npx", "--yes", "@mermaid-js/mermaid-cli", "-i", str(mmd), "-o", str(svg), "-b", "white"],
            check=True,
            cwd=ROOT,
        )
        svgs.append(svg)
    return svgs


def md_to_html_body(md: str, svg_paths: list[Path]) -> str:
    """Minimal markdown → HTML (headers, tables, lists, code, hr)."""
    lines = md.splitlines()
    html: list[str] = []
    in_code = False
    in_table = False
    diagram_idx = 0
    i = 0

    def flush_table(rows: list[str]) -> None:
        if not rows:
            return
        html.append("<table>")
        for ri, row in enumerate(rows):
            cells = [c.strip() for c in row.strip("|").split("|")]
            tag = "th" if ri == 0 else "td"
            html.append("<tr>" + "".join(f"<{tag}>{inline(c)}</{tag}>" for c in cells) + "</tr>")
        html.append("</table>")

    def inline(text: str) -> str:
        text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
        text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
        text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)
        return text

    table_rows: list[str] = []

    while i < len(lines):
        line = lines[i]

        if line.startswith("```"):
            fence = line.strip()
            if not in_code:
                in_code = True
                lang = fence[3:].strip()
                if lang == "mermaid":
                    svg = svg_paths[diagram_idx]
                    diagram_idx += 1
                    html.append(f'<div class="diagram"><img src="{svg.as_uri()}" alt="diagram"/></div>')
                    i += 1
                    while i < len(lines) and not lines[i].startswith("```"):
                        i += 1
                    in_code = False
                    i += 1
                    continue
                html.append(f'<pre><code class="{lang}">')
            else:
                html.append("</code></pre>")
                in_code = False
            i += 1
            continue

        if in_code:
            html.append(escape_html(line))
            i += 1
            continue

        if line.startswith("|"):
            if not in_table:
                in_table = True
                table_rows = []
            table_rows.append(line)
            i += 1
            continue
        elif in_table:
            flush_table(table_rows)
            table_rows = []
            in_table = False

        if line.startswith("#"):
            level = len(line) - len(line.lstrip("#"))
            text = line[level:].strip()
            html.append(f"<h{level}>{inline(text)}</h{level}>")
        elif line.strip() == "---":
            html.append("<hr/>")
        elif re.match(r"^\d+\.\s", line):
            if i == 0 or not lines[i - 1].startswith((" ", "\t")) and not re.match(r"^\d+\.", lines[i - 1] if i else ""):
                html.append("<ol>")
            html.append(f"<li>{inline(line[line.find('.') + 1 :].strip())}</li>")
            if i + 1 >= len(lines) or not re.match(r"^\d+\.", lines[i + 1]):
                html.append("</ol>")
        elif line.startswith("- "):
            if i == 0 or not lines[i - 1].startswith("- "):
                html.append("<ul>")
            html.append(f"<li>{inline(line[2:].strip())}</li>")
            if i + 1 >= len(lines) or not lines[i + 1].startswith("- "):
                html.append("</ul>")
        elif line.strip() == "":
            pass
        else:
            html.append(f"<p>{inline(line)}</p>")

        i += 1

    if in_table:
        flush_table(table_rows)

    return "\n".join(html)


def escape_html(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def wrap_html(body: str, title: str) -> str:
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<title>{escape_html(title)}</title>
<style>
  @page {{ margin: 18mm 16mm; }}
  body {{
    font-family: "Segoe UI", Helvetica, Arial, sans-serif;
    font-size: 10.5pt;
    line-height: 1.45;
    color: #1a1a1a;
    max-width: 920px;
    margin: 0 auto;
    padding: 12px 0;
  }}
  h1 {{ font-size: 22pt; border-bottom: 2px solid #007bff; padding-bottom: 6px; color: #0d3b66; }}
  h2 {{ font-size: 15pt; margin-top: 22px; color: #0d3b66; border-bottom: 1px solid #dee2e6; padding-bottom: 4px; }}
  h3 {{ font-size: 12pt; margin-top: 16px; color: #333; }}
  table {{ border-collapse: collapse; width: 100%; margin: 10px 0 16px; font-size: 9.5pt; }}
  th, td {{ border: 1px solid #ced4da; padding: 5px 8px; text-align: left; vertical-align: top; }}
  th {{ background: #e9ecef; font-weight: 600; }}
  tr:nth-child(even) td {{ background: #f8f9fa; }}
  code {{ background: #f1f3f5; padding: 1px 4px; border-radius: 3px; font-size: 9pt; }}
  pre {{ background: #f8f9fa; border: 1px solid #dee2e6; padding: 10px; overflow-x: auto; font-size: 8.5pt; }}
  hr {{ border: none; border-top: 1px solid #dee2e6; margin: 20px 0; }}
  ul, ol {{ margin: 6px 0 12px; padding-left: 22px; }}
  li {{ margin: 3px 0; }}
  .diagram {{
    page-break-inside: avoid;
    margin: 16px 0 20px;
    text-align: center;
  }}
  .diagram img {{
    max-width: 100%;
    height: auto;
  }}
  strong {{ color: #0d3b66; }}
  a {{ color: #007bff; text-decoration: none; }}
</style>
</head>
<body>
{body}
</body>
</html>
"""


def chrome_pdf(html_path: Path, pdf_path: Path) -> None:
    chrome = find_chrome()
    subprocess.run(
        [
            chrome,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            f"--print-to-pdf={pdf_path}",
            "--print-to-pdf-no-header",
            f"file://{html_path}",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def main() -> int:
    if not MD_PATH.is_file():
        print(f"Missing {MD_PATH}", file=sys.stderr)
        return 1

    md = MD_PATH.read_text(encoding="utf-8")
    blocks = extract_mermaid_blocks(md)
    print(f"Rendering {len(blocks)} Mermaid diagram(s)...")
    svgs = render_mermaid_svgs(blocks, BUILD_DIR)

    body = md_to_html_body(md, svgs)
    html = wrap_html(body, "ESPxx Next-Gen Sensoric — Feature Roadmap")

    html_path = BUILD_DIR / "features.html"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    html_path.write_text(html, encoding="utf-8")

    print(f"Writing PDF → {PDF_PATH}")
    chrome_pdf(html_path, PDF_PATH)
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
