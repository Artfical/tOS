// Shared sidebar + "on this page" component for every /tos/* page.
// Each page only needs <nav class="side" id="sideNav"></nav> and
// <nav class="toc" id="tocNav"></nav> placeholders; this fills both in from
// the tree below and from the current page's own headings, so no page has
// to hand-maintain a nav list that would drift from the others over time.

var DOCS_NAV_TREE = /*NAV_TREE*/[];

// Consolidates the sidebar's finer-grained sections into a small set of top
// tabs (the sidebar stays the primary nav; this is a second, coarser way to
// jump to the start of a broad area, matching each tab to whichever
// sections belong under it).
var DOCS_TAB_TREE = /*TAB_TREE*/[];

function renderDocsTabBar(currentSection){
  var mount = document.getElementById("tabBar");
  if (!mount) return;
  var html = "";
  DOCS_TAB_TREE.forEach(function(tab){
    var active = tab.sections.indexOf(currentSection) !== -1;
    html += '<a href="' + tab.href + '"' + (active ? ' class="active"' : "") + ">" + tab.label + "</a>";
  });
  mount.innerHTML = html;
}

function _docsChevronSvg(){
  return '<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 15 12 9 18"/></svg>';
}

function _docsIsCurrentOrSubpage(href, current){
  // A topic sub-page (/tos/<parent>--<topic>) isn't a row of its own in
  // this tree -- there would be far too many to list -- so its parent hub
  // page's row is highlighted instead, keeping the sidebar's sense of
  // "where am I" intact while drilled into a topic.
  if (href === current) return true;
  var parentSlug = href === "/tos" ? "index" : href.replace("/tos/", "");
  return current.indexOf("/tos/" + parentSlug + "--") === 0;
}

function renderDocsSidebar(){
  var mount = document.getElementById("sideNav");
  if (!mount) return;
  var current = window.location.pathname.replace(/\/$/, "") || "/tos";
  var html = "";

  DOCS_NAV_TREE.forEach(function(node){
    html += '<div class="grp-section-label">' + node.section + "</div>";
    html += '<div class="single">';
    node.items.forEach(function(it){
      if (it.children && it.children.length) {
        var childActive = it.children.some(function(c){ return c.href === current; });
        var isCurrent = it.href === current;
        var open = childActive || isCurrent;
        html += '<div class="grp' + (open ? " open" : "") + '">';
        html += '  <div class="grp-row' + (isCurrent ? " current" : "") + '">';
        html += '    <a class="grp-link" href="' + it.href + '">' + it.label + "</a>";
        html += '    <button class="grp-toggle" type="button" aria-label="Toggle ' + it.label + '">' + _docsChevronSvg() + "</button>";
        html += "  </div>";
        html += '  <div class="grp-children"><div class="grp-children-inner">';
        it.children.forEach(function(c){
          html += '<a href="' + c.href + '"' + (c.href === current ? ' class="active"' : "") + ">" + c.label + "</a>";
        });
        html += "  </div></div>";
        html += "</div>";
      } else {
        html += '<a href="' + it.href + '"' + (_docsIsCurrentOrSubpage(it.href, current) ? ' class="active"' : "") + ">" + it.label + "</a>";
      }
    });
    html += "</div>";
  });

  mount.innerHTML = html;

  mount.querySelectorAll(".grp-toggle").forEach(function(btn){
    btn.addEventListener("click", function(){
      btn.closest(".grp").classList.toggle("open");
    });
  });
  var act = mount.querySelector("a.active, .grp-row.current");
  if (act && act.scrollIntoView && window.innerWidth > 820) { try { act.scrollIntoView({block: "center"}); } catch (e) {} }
}

function _docsSlugify(text, used){
  var base = text.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "section";
  var slug = base, n = 2;
  while (used[slug]) { slug = base + "-" + n; n++; }
  used[slug] = true;
  return slug;
}

function renderDocsToc(){
  var tocMount = document.getElementById("tocNav");
  var content = document.querySelector("main.content");
  if (!tocMount || !content) return;

  // Every h2 and h3 in the page becomes its own "on this page" entry, h3
  // nested under whichever h2 precedes it, matching how the page actually
  // reads rather than only showing one entry per top-level section.
  var headings = Array.from(content.querySelectorAll("h2, h3")).filter(function(h){
    return !h.closest(".diagram-card, pre, .callout");
  });
  if (headings.length === 0) { tocMount.style.display = "none"; return; }

  var used = {};
  headings.forEach(function(h){
    if (!h.id) h.id = _docsSlugify(h.textContent, used);
    else used[h.id] = true;
  });

  var html = '<div class="toc-h">On this page</div>';
  headings.forEach(function(h){
    var cls = h.tagName === "H3" ? ' class="sub"' : "";
    html += '<a href="#' + h.id + '"' + cls + ">" + h.textContent + "</a>";
  });
  tocMount.innerHTML = html;

  var links = Array.from(tocMount.querySelectorAll("a"));
  function setActive(id){
    links.forEach(function(a){ a.classList.toggle("active", a.getAttribute("href") === "#" + id); });
  }

  // Heading-to-heading gaps vary too much in height for IntersectionObserver
  // to give a clean single "current" section (a short one can enter and
  // leave the viewport between scroll ticks); tracking the last heading
  // whose top has scrolled past a fixed offset is steadier here.
  var onScroll = function(){
    var offset = 96;
    var current = headings[0];
    for (var i = 0; i < headings.length; i++){
      if (headings[i].getBoundingClientRect().top - offset <= 0) current = headings[i];
      else break;
    }
    setActive(current.id);
  };
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();
}

function renderDocsCopyButton(){
  var h1 = document.querySelector("main.content h1.page-title");
  if (!h1) return;
  var row = document.createElement("div");
  row.className = "page-title-row";
  h1.parentNode.insertBefore(row, h1);
  row.appendChild(h1);

  var btn = document.createElement("button");
  btn.type = "button";
  btn.className = "copy-page-btn";
  var originalHTML = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg><span>Copy page</span>';
  btn.innerHTML = originalHTML;
  row.appendChild(btn);

  btn.addEventListener("click", function(){
    var article = document.querySelector("main.content");
    var text = article ? article.innerText.trim() : document.title;
    var done = function(){
      btn.innerHTML = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg><span>Copied</span>';
      setTimeout(function(){ btn.innerHTML = originalHTML; }, 1500);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function(){
        var ta = document.createElement("textarea");
        ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
        document.body.appendChild(ta); ta.select();
        try { document.execCommand("copy"); done(); } catch (e) {}
        document.body.removeChild(ta);
      });
    }
  });
}

document.addEventListener("DOMContentLoaded", function(){
  var mb = document.getElementById("menuBtn");
  if (mb) {
    mb.addEventListener("click", function(){ document.body.classList.toggle("nav-open"); });
    document.addEventListener("click", function(e){
      if (document.body.classList.contains("nav-open") && !e.target.closest("nav.side") && e.target !== mb) document.body.classList.remove("nav-open");
    });
  }
  renderDocsSidebar();
  renderDocsToc();
  renderDocsCopyButton();
  var kicker = document.querySelector("main.content .kicker");
  renderDocsTabBar(kicker ? kicker.textContent.trim() : "");
});
