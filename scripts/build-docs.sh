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
:root{color-scheme:light;--ink:#12140f;--paper:#f7f6f1;--white:#fffefa;--muted:#62645c;--line:#d8d8cf;--acid:#a6e22e;--acid-dark:#548a08;--cyan:#0b8897;--orange:#ed6b2f;--dark:#11130f;--dark-soft:#1b1e17;--mono:"IBM Plex Mono","SFMono-Regular",Consolas,monospace;--sans:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;--serif:"IBM Plex Serif",Georgia,"Times New Roman",serif;--max:1180px}
*{box-sizing:border-box}
html{scroll-behavior:smooth;background:var(--dark)}
body{margin:0;color:var(--ink);background:var(--paper);font:16px/1.64 var(--sans)}
::selection{color:var(--ink);background:var(--acid)}
a{color:var(--cyan);text-decoration-thickness:1px;text-underline-offset:.2em}
a:hover{color:var(--acid-dark)}
img{display:block;max-width:100%}
.skip-link{position:fixed;top:8px;left:8px;z-index:100;padding:8px 12px;color:var(--ink);background:var(--white);border:1px solid var(--ink);transform:translateY(-160%)}
.skip-link:focus{transform:none}
.site-header{background:#faf9f5;border-top:5px solid var(--dark);border-bottom:1px solid var(--line)}
.header-inner{display:flex;align-items:center;justify-content:space-between;gap:28px;width:min(calc(100% - 40px),var(--max));min-height:76px;margin:0 auto}
.brand{display:inline-flex;align-items:center;gap:10px;color:var(--ink);font-weight:850;text-decoration:none}
.brand:hover{color:var(--ink)}
.brand-mark{width:38px;height:38px;overflow:hidden;flex:0 0 38px;border:1px solid var(--ink);border-radius:50%;background:var(--white)}
.brand-mark img{width:100%;height:100%;object-fit:cover;transform:scale(1.45) translateY(4%)}
.brand strong,.brand small{display:block}
.brand strong{font:800 .98rem/1.15 var(--mono)}
.brand small{margin-top:3px;color:var(--muted);font:.68rem/1.2 var(--mono)}
.nav{display:flex;flex-wrap:wrap;align-items:center;justify-content:flex-end;gap:8px 24px}
.nav a{color:var(--ink);font-size:.8rem;font-weight:750;text-decoration:none}
.nav a:hover{color:var(--acid-dark)}
.nav .source-link{padding:8px 12px;color:var(--white);background:var(--ink);border:1px solid var(--ink)}
.nav .source-link:hover{color:var(--white);background:var(--orange);border-color:var(--orange)}
main{width:min(calc(100% - 40px),920px);margin:0 auto;padding:88px 0 110px}
.doc-kicker{margin:0 0 20px;color:var(--acid-dark);font:750 .75rem/1.2 var(--mono);text-transform:uppercase;letter-spacing:.02em}
.doc-kicker::before{content:"// ";color:var(--orange)}
.doc-body{min-width:0}
.doc-body h1,.doc-body h2,.doc-body h3,.doc-body h4{font-family:var(--sans);letter-spacing:-.025em}
.doc-body h1{max-width:850px;margin:0 0 38px;font-size:clamp(3rem,7vw,5.5rem);font-weight:900;line-height:.92;letter-spacing:-.055em}
.doc-body h1 code{font:inherit;color:inherit;background:none;padding:0}
.doc-body h1+p{max-width:800px;margin-bottom:38px;color:#34372f;font:500 clamp(1.12rem,2.2vw,1.35rem)/1.55 var(--serif)}
.doc-body h2{margin:64px 0 22px;padding-top:23px;border-top:1px solid var(--ink);font-size:clamp(1.8rem,4vw,2.55rem);font-weight:830;line-height:1.04}
.doc-body h2::before{content:"/ ";color:var(--orange);font-family:var(--mono);font-size:.55em;vertical-align:.2em}
.doc-body h3{margin:38px 0 14px;font-size:1.3rem;font-weight:800;line-height:1.2}
.doc-body h4{margin:30px 0 10px;font-size:1rem;font-weight:800;text-transform:uppercase}
.doc-body p,.doc-body li{font-family:var(--serif)}
.doc-body p,.doc-body ul,.doc-body ol,.doc-body blockquote,.doc-body table,.doc-body pre{margin:0 0 1.25rem}
.doc-body ul,.doc-body ol{padding-left:1.55rem}
.doc-body li{padding-left:.2rem}
.doc-body li+li{margin-top:.3rem}
.doc-body li::marker{color:var(--acid-dark);font-family:var(--mono);font-weight:800}
.doc-body strong{font-family:var(--sans);font-size:.94em}
code{padding:.13em .36em;color:#26301d;background:#e7ecd9;font:500 .86em var(--mono)}
pre{padding:22px 24px;overflow:auto;color:#d5d8ce;background:var(--dark);border:1px solid #363a31;line-height:1.65}
pre code{padding:0;color:inherit;background:none;font-size:.78rem}
blockquote{padding:20px 24px;color:#44473f;background:var(--white);border-left:5px solid var(--orange)}
blockquote p{font-family:var(--serif)}
table{display:block;width:100%;overflow:auto;border-collapse:collapse;border-block:1px solid var(--ink)}
thead{color:var(--white);background:var(--dark)}
th,td{min-width:100px;padding:13px 14px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{font:600 .68rem/1.4 var(--mono);text-transform:uppercase}
td{font:.82rem/1.5 var(--mono)}
tbody tr:nth-child(even){background:rgba(255,255,255,.5)}
tbody tr:last-child td{border-bottom:0}
hr{margin:3rem 0;border:0;border-top:1px solid var(--line)}
.site-footer{padding:38px max(20px,calc((100vw - var(--max))/2));color:#bfc2b8;background:#090a08}
.footer-inner{display:flex;justify-content:space-between;gap:30px;width:min(100%,var(--max));margin:0 auto}
.footer-brand{color:var(--white);font:750 .84rem var(--mono)}
.footer-copy{max-width:720px;text-align:right;font-size:.76rem}
.footer-copy a{color:#d8dbd1}
@media(max-width:760px){.nav a:not(.source-link){display:none}.brand small{display:none}main{padding:62px 0 82px}.doc-body h1{font-size:clamp(2.65rem,13vw,4rem)}.doc-body h2{margin-top:52px}.footer-inner{display:block}.footer-copy{margin-top:18px;text-align:left}}
@media(max-width:420px){.header-inner{min-height:68px}.nav .source-link{padding:7px 9px;font-size:.72rem}main{width:min(calc(100% - 32px),920px)}pre{padding:18px 16px}}
@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}}
"""

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
    root = "../" * path.count(os.sep)
    home = root + "index.html"
    readme = root + "README.html"
    benchmarks = root + "BENCHMARKS.html"
    architecture = root + "docs/adr/README.html"
    mascot = root + "goblin.png"
    license_path = root + "LICENSE"
    header = (f'<a class="skip-link" href="#content">Skip to content</a>'
              f'<header class="site-header"><div class="header-inner">'
              f'<a class="brand" href="{home}" aria-label="Goblin Store home">'
              f'<span class="brand-mark"><img src="{mascot}" alt=""></span><span>'
              f'<strong>Goblin Store</strong><small>Technical documentation</small></span></a>'
              f'<nav class="nav" aria-label="Documentation navigation">'
              f'<a href="{home}">Home</a><a href="{readme}">README</a>'
              f'<a href="{benchmarks}">Benchmarks</a><a href="{architecture}">Architecture</a>'
              f'<a class="source-link" href="https://github.com/adamdeprince/goblin-store">View source</a>'
              f'</nav></div></header>')
    footer = (f'<footer class="site-footer"><div class="footer-inner">'
              f'<div class="footer-brand">Goblin Store</div><div class="footer-copy">'
              f'First byte from RAM. The rest is already on its way. &middot; &copy; 2026 Adam DePrince &middot; '
              f'<a href="{license_path}">Apache-2.0</a> &middot; '
              f'<a href="https://goblinreactor.com">Built by Goblin Reactor</a>'
              f'</div></div></footer>')
    return ('<!doctype html>\n<html lang="en"><head><meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
            '<meta name="theme-color" content="#11130f">\n'
            '<link rel="preconnect" href="https://fonts.googleapis.com">\n'
            '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
            '<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&amp;family=IBM+Plex+Serif:wght@400;500;600;700&amp;display=swap" rel="stylesheet">\n'
            f'<link rel="icon" href="{mascot}" type="image/png">\n'
            f'<title>{H.escape(title)} — Goblin Store</title>\n<style>{CSS}</style>\n'
            f'</head><body>{header}<main id="content"><p class="doc-kicker">Field notes from the cave</p>'
            f'<article class="doc-body">\n{body}\n</article></main>{footer}</body></html>\n')

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
