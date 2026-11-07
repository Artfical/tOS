#!/usr/bin/env python3
"""Builds the tOS documentation site (docs.artfical.com/tos) from docs/site.

    tools/gen_docs_site.py OUTDIR [PACKAGE_DIR]

docs/site/nav.json     the sidebar and the tabs
docs/site/pages/*.md   one page each (a few lines of "key: value", a line "---", then the text)
docs/site/assets/      the style sheet and scripts, the same ones the other docs on that host use
OUTDIR                 receives <slug>.html, docs-search-index.js and the assets
PACKAGE_DIR            the update server's package folder (for the package list page)

The text is a small Markdown: ## and ### headings, paragraphs, - and 1. lists, ``` code, | tables |,
![caption](shot:name.png) pictures (from docs/screenshots), ![caption](video:name.mp4) short videos (from docs/videos), "> " notes, `code`, **bold**, [links](url). The page "package-list" gets its table from PACKAGE_DIR.
"""
import html, json, os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, '..', 'docs', 'site')
BASE = '/tos'
VER = '1'

CALLOUT_SVG = ('<svg class="callout-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
               'stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/>'
               '<line x1="12" y1="8" x2="12.01" y2="8"/></svg>')
SEARCH_SVG = ('<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
              'stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/>'
              '<line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>')


def href(slug):
    return BASE if slug == 'index' else f'{BASE}/{slug}'


def inline(t):
    t = re.sub(r'!\[\]\(icon:([a-z0-9_]+)\)', lambda m: '\x00I' + m.group(1) + '\x00', t)
    parts = re.split(r'(`[^`]+`)', t)
    out = []
    for p in parts:
        if p.startswith('`') and p.endswith('`') and len(p) > 1:
            out.append('<code>' + html.escape(p[1:-1]) + '</code>')
            continue
        p = html.escape(p, quote=False)
        p = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', p)
        p = re.sub(r'\[([^\]]+)\]\(([^)\s]+)\)', lambda m: '<a href="%s">%s</a>' % (m.group(2).replace('"', '%22'), m.group(1)), p)
        out.append(p)
    return re.sub('\x00I([a-z0-9_]+)\x00', lambda m: f'<img class="inline-icon" src="{icon_src(m.group(1))}" alt="">', ''.join(out))


def slugify(text, used):
    base = re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-') or 'section'
    s, n = base, 2
    while s in used:
        s = f'{base}-{n}'
        n += 1
    used.add(s)
    return s


SHOTS = set()      # screenshots the pages use
ICONS = set()      # app icons the pages use (third_party/whitesur-icons/svg, or docs/site/assets/icons)


def icon_src(name):
    ICONS.add(name)
    return f'{BASE}/img/icons/{name}.' + ('png' if os.path.exists(os.path.join(SITE, 'assets', 'icons', name + '.png')) else 'svg')
VIDEOS = set()     # videos the pages use


def render_body(lines, heads):
    """Markdown lines -> HTML; heads gets (level, id, text) for each heading."""
    out, i, used = [], 0, set()
    while i < len(lines):
        ln = lines[i]
        if not ln.strip():
            i += 1
            continue
        if ln.startswith('```'):
            i += 1
            buf = []
            while i < len(lines) and not lines[i].startswith('```'):
                buf.append(html.escape(lines[i], quote=False))
                i += 1
            i += 1
            out.append('<pre class="code-block">' + '\n'.join(buf) + '</pre>')
            continue
        m = re.match(r'!\[([^\]]*)\]\(shot:([^)\s]+)\)\s*$', ln)
        if m:
            SHOTS.add(m.group(2))
            cap = html.escape(m.group(1), quote=True)
            out.append(f'<figure class="shot"><img src="{BASE}/img/{m.group(2)}" alt="{cap}" loading="lazy"><figcaption>{cap}</figcaption></figure>')
            i += 1
            continue
        m = re.match(r'!\[([^\]]*)\]\(video:([^)\s]+)\)\s*$', ln)
        if m:
            VIDEOS.add(m.group(2))
            cap = html.escape(m.group(1), quote=True)
            out.append(f'<figure class="shot"><video src="{BASE}/img/{m.group(2)}" controls loop muted playsinline preload="metadata"></video><figcaption>{cap}</figcaption></figure>')
            i += 1
            continue
        m = re.match(r'(#{2,3}) +(.+)', ln)
        if m:
            lvl = len(m.group(1))
            text = m.group(2).strip()
            hid = slugify(re.sub(r'!\[\]\(icon:[a-z0-9_]+\)\s*', '', text), used)
            heads.append((lvl, hid, re.sub(r'!\[\]\(icon:[a-z0-9_]+\)\s*', '', text)))
            out.append(f'<h{lvl} id="{hid}">{inline(text)}</h{lvl}>')
            i += 1
            continue
        if ln.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip().strip('|').split('|')])
                i += 1
            head, body = rows[0], [r for r in rows[2:]] if len(rows) > 1 and set(''.join(rows[1])) <= set('-: ') else rows[1:]
            t = ['<table class="ref">', '<thead><tr>' + ''.join(f'<th>{inline(c)}</th>' for c in head) + '</tr></thead>', '<tbody>']
            for r in body:
                cells = []
                for k, c in enumerate(r):
                    cells.append(f'<td class="name">{inline(c)}</td>' if k == 0 else f'<td>{inline(c)}</td>')
                t.append('<tr>' + ''.join(cells) + '</tr>')
            t += ['</tbody>', '</table>']
            out.append('\n'.join(t))
            continue
        if ln.startswith('> '):
            buf = []
            while i < len(lines) and lines[i].startswith('>'):
                buf.append(lines[i][1:].strip())
                i += 1
            out.append('<div class="callout">' + CALLOUT_SVG + '<div class="callout-body"><p>' + inline(' '.join(buf)) + '</p></div></div>')
            continue
        m = re.match(r'(- |\d+\. )(.*)', ln)
        if m:
            tag = 'ul' if m.group(1) == '- ' else 'ol'
            items = []
            while i < len(lines):
                m2 = re.match(r'(- |\d+\. )(.*)', lines[i])
                if not m2 or (m2.group(1) == '- ') != (tag == 'ul'):
                    break
                items.append(m2.group(2))
                i += 1
                while i < len(lines) and lines[i].startswith('  ') and lines[i].strip():
                    items[-1] += ' ' + lines[i].strip()
                    i += 1
            out.append(f'<{tag}>' + ''.join(f'<li>{inline(x)}</li>' for x in items) + f'</{tag}>')
            continue
        buf = []
        while i < len(lines) and lines[i].strip() and not re.match(r'(```|#{2,3} |\||> |- |\d+\. |!\[)', lines[i]):
            buf.append(lines[i].strip())
            i += 1
        out.append('<p>' + inline(' '.join(buf)) + '</p>')
    return '\n'.join(out)


def parse_page(path):
    raw = open(path, encoding='utf-8').read().split('\n')
    meta, i = {}, 0
    while i < len(raw) and raw[i].strip() != '---':
        k, _, v = raw[i].partition(':')
        meta[k.strip()] = v.strip()
        i += 1
    return meta, raw[i + 1:]


def package_table(pkgdir):
    rows = []
    for name in sorted(os.listdir(pkgdir)):
        mp = os.path.join(pkgdir, name, 'meta.json')
        if not os.path.isfile(mp):
            continue
        m = json.load(open(mp))
        desc = m.get('desc', '').replace('|', '/')
        rows.append(f"| `{m['name']}` | {m['version']} | {m.get('license', '')} | {desc} |")
    return ['| Package | Version | Licence | What it is |', '|---|---|---|---|'] + rows


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[1]
    pkgdir = sys.argv[2] if len(sys.argv) > 2 else None
    nav = json.load(open(os.path.join(SITE, 'nav.json')))
    order = [it for sec in nav['sections'] for it in sec['items']]
    sec_of = {it['slug']: sec['section'] for sec in nav['sections'] for it in sec['items']}
    os.makedirs(out, exist_ok=True)

    # the shared scripts and style sheet, with the sidebar and the tabs filled in
    for f in ('docs.css', 'docs-search.js'):
        shutil.copy(os.path.join(SITE, 'assets', f), os.path.join(out, f))
    navjs = open(os.path.join(SITE, 'assets', 'docs-nav.js'), encoding='utf-8').read()
    tree = [{'section': s['section'], 'items': [{'label': it['label'], 'href': href(it['slug'])} for it in s['items']]} for s in nav['sections']]
    tabs = [{'label': t['label'], 'href': href(t['slug']), 'sections': t['sections']} for t in nav['tabs']]
    navjs = navjs.replace('/*NAV_TREE*/[]', json.dumps(tree, indent=2)).replace('/*TAB_TREE*/[]', json.dumps(tabs, indent=2))
    open(os.path.join(out, 'docs-nav.js'), 'w', encoding='utf-8').write(navjs)

    index = []
    for n, it in enumerate(order):
        slug = it['slug']
        meta, lines = parse_page(os.path.join(SITE, 'pages', slug + '.md'))
        if slug == 'package-list' and pkgdir:
            lines = [l for l in lines if l.strip() != '{{packages}}']
            lines += [''] + package_table(pkgdir)
        heads = []
        body = render_body(lines, heads)
        h2 = [h for h in heads if h[0] == 2 and h[2] != 'Related']
        covers = ''
        if len(h2) >= 3:
            covers = ('<div class="page-covers"><div class="covers-label">This page covers:</div><ul>' +
                      ''.join(f'<li><a href="#{h[1]}">{inline(h[2])}</a></li>' for h in h2) + '</ul></div>')
        prev_a = (f'<a class="prev" href="{href(order[n-1]["slug"])}"><span class="pager-label">Previous</span>'
                  f'<span class="pager-title">{html.escape(order[n-1]["label"])}</span></a>') if n else '<span></span>'
        next_a = (f'<a class="next next" href="{href(order[n+1]["slug"])}"><span class="pager-label">Next</span>'
                  f'<span class="pager-title">{html.escape(order[n+1]["label"])}</span></a>') if n + 1 < len(order) else ''
        title = meta.get('title', it['label'])
        icons = ''
        if meta.get('icon'):
            icons = '<div class="page-icons">' + ''.join(f'<img src="{icon_src(n.strip())}" alt="" width="64" height="64">' for n in meta['icon'].split(',')) + '</div>'
        page = f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{html.escape(title)} — tOS Docs</title>
<meta name="description" content="{html.escape(meta.get('lede', ''), quote=True)}">
<link rel="icon" type="image/png" href="https://artficdn.com.tr/assets/ai-artfical-favicon-v3.png">
<link rel="stylesheet" href="{BASE}/docs.css?v={VER}">
<style>.shot{{margin:20px 0 26px;max-width:760px}}.shot img,.shot video{{display:block;width:100%;height:auto;border-radius:10px;border:1px solid rgba(255,255,255,.12)}}.shot figcaption{{color:var(--muted);font-size:13px;margin-top:8px}}.page-icons{{display:flex;gap:14px;flex-wrap:wrap;margin:0 0 18px}}.page-icons img{{width:64px;height:64px;object-fit:contain}}.inline-icon{{width:24px;height:24px;vertical-align:middle;margin:-3px 8px 0 0}}</style>
</head>
<body>

<header class="top">
  <div class="top-left">
    <a class="top-brand" href="{BASE}">
      <span style="font-weight:700;font-size:17px;letter-spacing:-.01em;color:#fff">tOS</span>
      <span class="sep">/</span>
      <span class="docs-word">Docs</span>
    </a>
  </div>
  <button class="search-btn" id="docsSearchBtn" type="button">
    {SEARCH_SVG}
    <span>Search docs...</span>
    <kbd>Ctrl K</kbd>
  </button>
  <div class="top-right">
    <a href="https://github.com/Artfical/tOS">Source code</a>
    <a href="https://github.com/Artfical/tOS/blob/main/SECURITY.md">Report a vulnerability</a>
    <a class="btn" href="https://github.com/Artfical/tOS/releases">Download tOS</a>
  </div>
</header>

<div class="tab-bar">
  <div class="tab-bar-inner" id="tabBar"></div>
</div>

<div class="search-overlay" id="searchOverlay">
  <div class="search-modal">
    <input type="text" id="searchInput" placeholder="Search docs...">
    <div class="search-results" id="searchResults"></div>
  </div>
</div>

<div class="shell">
  <nav class="side" id="sideNav"></nav>

  <main class="content">

    <section class="doc-section" id="{slug}">
      {icons}
      <div class="kicker">{html.escape(sec_of[slug])}</div>
      <h1 class="page-title">{html.escape(title)}</h1>
      <p class="lede">{inline(meta.get('lede', ''))}</p>
      {covers}
{body}
    </section>
    <div class="page-pager">
      {prev_a}
      {next_a}
    </div>

  </main>

  <nav class="toc" id="tocNav"></nav>
</div>

<script src="{BASE}/docs-nav.js?v={VER}"></script>
<script src="{BASE}/docs-search-index.js?v={VER}"></script>
<script src="{BASE}/docs-search.js?v={VER}"></script>
<script src="/js/cdn-fallback.js?v=1"></script>
</body>
</html>
'''
        open(os.path.join(out, slug + '.html'), 'w', encoding='utf-8').write(page)
        text = re.sub(r'\s+', ' ', re.sub(r'<[^>]+>', ' ', html.unescape(body))).strip()
        index.append({'title': title, 'section': sec_of[slug], 'href': href(slug), 'text': (meta.get('lede', '') + ' ' + text)[:1800]})

    shots = os.path.join(SITE, '..', 'screenshots')
    for name in sorted(SHOTS):
        dst = os.path.join(out, 'img', name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(shots, name), dst)

    wsi = os.path.join(SITE, '..', '..', 'third_party', 'whitesur-icons', 'svg')
    for name in sorted(ICONS):
        os.makedirs(os.path.join(out, 'img', 'icons'), exist_ok=True)
        a = os.path.join(SITE, 'assets', 'icons', name + '.png')
        b = os.path.join(wsi, name + '.svg')
        shutil.copy(a if os.path.exists(a) else b, os.path.join(out, 'img', 'icons', os.path.basename(a if os.path.exists(a) else b)))

    vids = os.path.join(SITE, '..', 'videos')
    for name in sorted(VIDEOS):
        src = os.path.join(vids, name)
        if not os.path.exists(src):
            src = os.path.join(shots, name)               # e.g. tfire/u5-anim.mp4
        dst = os.path.join(out, 'img', name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(src, dst)

    open(os.path.join(out, 'docs-search-index.js'), 'w', encoding='utf-8').write(
        '// Generated by tools/gen_docs_site.py -- do not edit by hand.\nvar DOCS_SEARCH_INDEX = ' + json.dumps(index, indent=2, ensure_ascii=False) + ';\n')
    print(f'{len(order)} pages in {out}')


if __name__ == '__main__':
    main()
