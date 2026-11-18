title: tFire, the web browser
lede: tFire is tOS's own web browser. It opens pages over HTTPS and HTTP, lays out HTML and CSS, runs JavaScript and shows pictures, with tabs, find in a page, bookmarks and a history.
icon: tfire
---
![tFire's home page](shot:tfire/home.png)

tFire is in the dock. It is written for tOS: its page layout, its network code and its window are part of the system, built with a few well-known libraries (NetSurf's HTML and CSS libraries, the QuickJS JavaScript engine, and picture decoders), all listed at `tfire:licenses`.

## Using tFire

| To do this | Do this |
|---|---|
| Open a page | Type an address in the bar at the top and press Enter |
| Search the web | Type words instead of an address. tFire searches with DuckDuckGo |
| Follow a link | Click it |
| Go back and forward | The arrow buttons next to the address bar |
| Reload | The reload button |
| Go to the start page | The home button, or the address `tfire:home` |
| Open a new tab | Ctrl+T (more under "Tabs") |

![A search: tFire searches with DuckDuckGo](shot:tfire/search.png)

## What it shows

tFire understands the parts of the web that most pages are made of:

- **Text and layout:** headings, paragraphs, lists, tables, links and preformatted text, laid out for the width of the window.
- **Style sheets:** CSS, including fonts, colours, gradients and shadows.
- **Pictures:** PNG and JPEG, and more.
- **Animations:** CSS animations and transitions, and the Web Animations API.
- **JavaScript:** page scripts run, so pages that build themselves in the browser work.
- **Frames:** inline frames, which can talk to the page around them.
- **Redirects:** followed for you, with a back and forward history.

It hides what a page keeps out of sight (the `hidden` attribute, `display:none`, menus and navigation by their usual class names).

![A news article](shot:tfire/article.png)

![Wikipedia: text, tables and pictures](shot:tfire/u3-wikipedia.png)

![The Python home page, with its styles](shot:tfire/u3-python.png)

![A busy page: Hacker News](shot:tfire/u3-hackernews.png)

![A busy page: GitHub](shot:tfire/u3-github.png)

## Styles, animations and scripts

Pages that make heavy use of CSS draw correctly: gradients, shadows, different type sizes and fonts, and monospace code blocks.

![Type, shadows and gradients](shot:tfire/u6-style.png)

![Wikipedia with its styles](shot:tfire/u6-wikipedia.png)

![The style page, running](video:tfire/u6-style.mp4)

CSS animations, transitions and timing functions run, and the pointer can drive hover effects.

![Animations: keyframes, transitions and timing functions](shot:tfire/u5-anim.png)

![The animations page, running](video:tfire/u5-anim.mp4)

![Hovering a button changes it](shot:tfire/u5-hover.png)

![An inline frame that talks to its parent page](shot:tfire/u7-iframes.png)

## Downloads

A link to a file that is not a page (a zip archive, a PDF, a text file) is **downloaded**: tFire saves it in your Downloads folder and shows a bar with the name, the size and a **Show in Files** button.

![A download finished: Show in Files](shot:tfire/u4-download.png)

![Downloading, and opening the file's folder](video:tfire/u4-tfire.mp4)

![The Downloads folder, opened from the bar](shot:tfire/u4-files.png)

## Tabs

Press **Ctrl+T** for a new tab. Once there are two or more, a strip of tabs appears above the address bar: click a tab to switch to it, its **x** to close it, the **+** to open another. Up to eight tabs fit.

| To do this | Press |
|---|---|
| New tab | Ctrl+T |
| Close the tab | Ctrl+W |
| Open the last closed tab again | Ctrl+Shift+T |
| Next tab, previous tab | Ctrl+Tab, Ctrl+Shift+Tab |
| Go to tab 1 to 9 (9 is the last) | Ctrl+1 to Ctrl+9 |

When you close the last tab, tFire goes back to the start page. When you close tFire and open it again from the Dock, the tabs that were open come back.

![Three tabs, running on tOS](shot:tfire/u8-tabs.png)

## Find in a page

**Ctrl+F** opens the find bar at the bottom of the window. Type what to look for: every match is lit in yellow and the one you are on in orange, with the count next to the box ("1 of 12"). **Enter** goes to the next match, **Shift+Enter** to the one before, **Esc** closes the bar. Capital and small letters count the same for the letters A to Z; a match lights the whole word.

![Find in a page, on tOS](shot:tfire/u8-find.png)

## Bookmarks

**Ctrl+D**, or the star at the right end of the address bar, keeps the page you are on (the star is filled when it is kept). **Ctrl+Shift+O** opens `tfire:bookmarks`, where each bookmark can be removed; the start page lists them too. Up to 100 are kept, in `.tfire/bookmarks.txt` in your home folder.

## History

Every page you visit is kept, the newest first (300 at most), in `.tfire/history.txt` in your home folder. **Ctrl+H** opens `tfire:history`, where one entry can be removed or all of them cleared. The start page shows the latest ones.

## The address bar

The address bar has a caret you can move: **Left** and **Right**, and a click to place it. **Ctrl+A** selects all of it, **Ctrl+C** copies and **Ctrl+X** cuts, **Ctrl+V** pastes (tOS has no clipboard shared between apps, so this works between tFire's own windows). While you type, it suggests bookmarks and pages from your history: **Up** and **Down** choose one, **Enter** goes there, a click opens it. **Ctrl+L** moves to the bar from the page.

## Links and history

![Following a link from one article to another](shot:tfire/u4-wikipedia-link.png)

![The destination page](shot:tfire/u4-netscape.png)

## The start page

`tfire:home` is the start page: your bookmarks (or, until you have some, a short list of places to go: Wikipedia, Hacker News, DuckDuckGo, news, tOS updates), the pages you visited last, and what tFire can show.

![The start page](shot:tfire/u3-home.png)

![An earlier version of the start page](shot:tfire/home2.png)

## Licences

`tfire:licenses` lists the libraries inside tFire, and everything else in tOS that comes from other projects, with their versions and licences.

![tfire:licenses](shot:tfire/u3-licenses.png)

## Limits

- Pages that depend on features tFire does not have yet may look different from other browsers. Some picture formats (for example progressive JPEG) may not be decoded; their alt text shows instead.
- tFire is a young browser. If a page looks wrong, please report it with the address.

## Downloads and the Downloads folder

A link to a file that is not a page (a zip archive, a PDF, a text file) is **downloaded**: tFire saves it in your Downloads folder and shows a bar with the name, the size and a **Show in Files** button.

![A download finished: Show in Files](shot:tfire/u4-download.png)

![The Downloads folder, opened from the bar](shot:tfire/u4-files.png)

## Links and history

![Following a link from one article to another](shot:tfire/u4-wikipedia-link.png)

![The destination page](shot:tfire/u4-netscape.png)

## The start page

`tfire:home` is the start page: your bookmarks (or, until you have some, a short list of places to go: Wikipedia, Hacker News, DuckDuckGo, news, tOS updates), the pages you visited last, and what tFire can show.

![The start page](shot:tfire/u3-home.png)

![An earlier version of the start page](shot:tfire/home2.png)

## Limits

- Pages that depend on features tFire does not have yet may look different from other browsers. Some picture formats (for example progressive JPEG) may not be decoded; their alt text shows instead.
- There is a single TCP connection in tOS's own network code for the browser, so a page and its pictures load one after another.
- tFire is a young browser. If a page looks wrong, please report it with the address.

## Licences

`tfire:licenses` lists the libraries inside tFire, and everything else in tOS that comes from other projects, with their versions and licences.

![tfire:licenses](shot:tfire/u3-licenses.png)

## Related

- Downloads end up in Files: [Files](/tos/files-app).
- For tools that fetch pages from the Terminal, see [Networking](/tos/networking).
