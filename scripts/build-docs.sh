#!/usr/bin/env bash
#
# build-docs.sh — render every *.md in the repo to html/<same-path>.html.
#
# Mirrors the source tree (README.md -> html/README.html, docs/adr/0003-*.md ->
# html/docs/adr/0003-*.html, ...) and rewrites inter-doc `.md` links to `.html` so the output is
# browsable offline. The hand-authored html/index.html and mascot are preserved.
#
# The rendered Markdown under html/ is a GENERATED BUILD ARTIFACT and is .gitignored. The front
# page and mascot are committed source files. Re-run any time:
#
#     scripts/build-docs.sh
#
# Renderer: Python's `markdown` library (GFM-ish: tables, fenced code, anchors), bootstrapped into a
# local venv at .cache/docs-venv on first run. No system packages are touched.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

VENV=".cache/docs-venv"
if [ ! -x "$VENV/bin/python" ]; then
  echo "==> bootstrapping markdown renderer into $VENV (one-time)"
  python3 -m venv "$VENV"
  "$VENV/bin/python" -m pip install --quiet --upgrade pip >/dev/null
  "$VENV/bin/python" -m pip install --quiet 'markdown>=3.4'
fi

"$VENV/bin/python" - <<'PYEOF'
import html as H, os, re, shutil
import markdown

OUT = "html"
PRESERVE = {"index.html", "goblin.png"}

# Clean generated output without deleting the hand-authored landing page or its asset.
os.makedirs(OUT, exist_ok=True)
for name in os.listdir(OUT):
    if name in PRESERVE:
        continue
    path = os.path.join(OUT, name)
    if os.path.isdir(path):
        shutil.rmtree(path)
    else:
        os.remove(path)
def skip(d): return d in {".git", "third_party", OUT, ".cache"} or d.startswith("build")

CSS = """
:root{--fg:#1b1b1f;--bg:#fff;--muted:#5a5a66;--accent:#6b46c1;--border:#e3e3ea;--code-bg:#f6f6f9}
*{box-sizing:border-box}
body{margin:0;color:var(--fg);background:var(--bg);font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
main{max-width:54rem;margin:0 auto;padding:2.5rem 1.25rem 6rem}
h1,h2,h3,h4{line-height:1.25;margin:2rem 0 .6rem;font-weight:650}
h1{font-size:1.9rem;margin-top:0}
h2{font-size:1.4rem;border-bottom:1px solid var(--border);padding-bottom:.3rem}
h3{font-size:1.15rem}
a{color:var(--accent);text-decoration:none} a:hover{text-decoration:underline}
p,ul,ol,blockquote,table,pre{margin:0 0 1rem}
code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.9em;background:var(--code-bg);padding:.12em .35em;border-radius:4px}
pre{background:var(--code-bg);padding:1rem;border-radius:8px;overflow:auto;border:1px solid var(--border)}
pre code{background:none;padding:0}
blockquote{border-left:4px solid var(--accent);margin:0 0 1rem;padding:.2rem 0 .2rem 1rem;color:var(--muted)}
table{border-collapse:collapse;width:100%;font-size:.95em;display:block;overflow:auto}
th,td{border:1px solid var(--border);padding:.45rem .7rem;text-align:left} th{background:var(--code-bg)}
hr{border:0;border-top:1px solid var(--border);margin:2rem 0}
img{max-width:100%}
footer{margin-top:3rem;padding-top:1.25rem;border-top:1px solid var(--border);color:var(--muted);font-size:.85em}
footer a{color:var(--muted)}
"""

FOOTER = ('<footer>'
          '<a href="https://github.com/adamdeprince/goblin-store">Source &amp; issues on GitHub</a>'
          ' &middot; &copy; 2026 Adam DePrince &middot; '
          'Licensed under <a href="https://www.apache.org/licenses/LICENSE-2.0">Apache-2.0</a>'
          '</footer>')

def render(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r'^#\s+(.+)$', text, re.M)
    title = re.sub(r'[`*_]', '', (m.group(1).strip() if m else os.path.basename(path)))
    md = markdown.Markdown(extensions=["extra", "toc", "sane_lists", "smarty"], output_format="html5")
    body = md.convert(text)
    # Rewrite local .md links (not http(s)/mailto) to .html so the generated tree is browsable.
    body = re.sub(r'(href="(?!https?:|//|mailto:)[^"]*?)\.md(["#])', r'\1.html\2', body)
    # Drop the ".md" from link *text* so a "BENCHMARKS.md" label shows as "BENCHMARKS".
    body = body.replace(".md</code></a>", "</code></a>").replace(".md</a>", "</a>")
    return ('<!doctype html>\n<html lang="en"><head><meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
            f'<title>{H.escape(title)}</title>\n<style>{CSS}</style>\n'
            f'</head><body><main>\n{body}\n{FOOTER}\n</main></body></html>\n')

mds = []
for dp, dn, fns in os.walk("."):
    dn[:] = [d for d in dn if not skip(d)]
    mds += [os.path.relpath(os.path.join(dp, fn), ".") for fn in fns if fn.endswith(".md")]
mds.sort()

for md in mds:
    # Every Markdown page mirrors its source path, including the root README.
    out = os.path.join(OUT, md[:-3] + ".html")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(render(md))
    print("  ", out)

# Copy LICENSE into the site so in-page [LICENSE](LICENSE) links resolve.
if os.path.exists("LICENSE"):
    shutil.copy("LICENSE", os.path.join(OUT, "LICENSE"))
    print("   html/LICENSE")

print(f"==> generated {len(mds)} page(s) in {OUT}/  (hand-authored index.html preserved)")
PYEOF
