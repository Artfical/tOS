# tOS documentation sources

The pages of [docs.artfical.com/tos](https://docs.artfical.com/tos).

- `nav.json`: the sidebar and the tabs.
- `pages/*.md`: one page each, in a small Markdown (see the top of `tools/gen_docs_site.py`). Pictures are `![caption](shot:name.png)`, taken from `docs/screenshots/`.
- `assets/`: the style sheet and scripts, shared with the other docs on that host.

Build the site and put it where the docs server serves it:

```
tools/gen_docs_site.py /home/taiapp/tai-app/frontend/tos-docs /root/tos-update/pkg
```

The second argument (the package server's folder) fills the package list page. The server's `docs.artfical.com` host serves `/tos` from that folder.
