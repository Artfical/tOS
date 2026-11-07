// Client-side search over DOCS_SEARCH_INDEX (generated at build time from
// the same content every page is built from, see gen_docs.py). No backend
// call: filtering happens entirely in the browser against the small index
// already loaded, so results appear as you type with no round trip.

function _docsScoreEntry(entry, terms){
  var haystackTitle = entry.title.toLowerCase();
  var haystackSection = entry.section.toLowerCase();
  var haystackText = entry.text.toLowerCase();
  var score = 0;
  for (var i = 0; i < terms.length; i++){
    var t = terms[i];
    if (!t) continue;
    if (haystackTitle.indexOf(t) !== -1) score += 10;
    if (haystackSection.indexOf(t) !== -1) score += 4;
    var idx = haystackText.indexOf(t);
    if (idx !== -1) score += 2;
    else return -1; // every term must appear somewhere for this entry to match
  }
  return score;
}

function _docsSnippet(entry, terms){
  var text = entry.text;
  var lower = text.toLowerCase();
  var idx = -1;
  for (var i = 0; i < terms.length; i++){
    idx = lower.indexOf(terms[i]);
    if (idx !== -1) break;
  }
  if (idx === -1) return text.slice(0, 140) + (text.length > 140 ? "..." : "");
  var start = Math.max(0, idx - 50);
  var end = Math.min(text.length, idx + 90);
  return (start > 0 ? "..." : "") + text.slice(start, end) + (end < text.length ? "..." : "");
}

document.addEventListener("DOMContentLoaded", function(){
  var btn = document.getElementById("docsSearchBtn");
  var overlay = document.getElementById("searchOverlay");
  var input = document.getElementById("searchInput");
  var results = document.getElementById("searchResults");
  if (!btn || !overlay || !input || !results || typeof DOCS_SEARCH_INDEX === "undefined") return;

  var ui = window.DOCS_UI_STRINGS || {};
  var typeToSearchLabel = ui.search_type_to_search || "Type to search the docs";
  var noResultsPrefix = ui.search_no_results_prefix || "No results for";

  var selectedIndex = -1;
  var currentMatches = [];

  function open(){
    overlay.classList.add("open");
    input.value = "";
    input.focus();
    render([]);
  }
  function close(){
    overlay.classList.remove("open");
  }

  function render(matches){
    currentMatches = matches;
    selectedIndex = matches.length ? 0 : -1;
    if (matches.length === 0) {
      results.innerHTML = input.value.trim()
        ? '<div class="search-empty">' + noResultsPrefix + ' "' + input.value.replace(/</g, "&lt;") + '"</div>'
        : '<div class="search-empty">' + typeToSearchLabel + '</div>';
      return;
    }
    var terms = input.value.toLowerCase().trim().split(/\s+/);
    var html = "";
    matches.forEach(function(m, i){
      html += '<a href="' + m.href + '" class="' + (i === selectedIndex ? "sel" : "") + '">';
      html += '<div class="r-section">' + m.section + "</div>";
      html += '<div class="r-title">' + m.title + "</div>";
      html += '<div class="r-snippet">' + _docsSnippet(m, terms) + "</div>";
      html += "</a>";
    });
    results.innerHTML = html;
  }

  function search(){
    var query = input.value.toLowerCase().trim();
    if (!query) { render([]); return; }
    var terms = query.split(/\s+/);
    var scored = DOCS_SEARCH_INDEX
      .map(function(entry){ return { entry: entry, score: _docsScoreEntry(entry, terms) }; })
      .filter(function(s){ return s.score >= 0; })
      .sort(function(a, b){ return b.score - a.score; })
      .slice(0, 8)
      .map(function(s){ return s.entry; });
    render(scored);
  }

  btn.addEventListener("click", open);
  overlay.addEventListener("click", function(e){ if (e.target === overlay) close(); });
  input.addEventListener("input", search);

  document.addEventListener("keydown", function(e){
    var isMod = e.ctrlKey || e.metaKey;
    if (isMod && e.key.toLowerCase() === "k") {
      e.preventDefault();
      overlay.classList.contains("open") ? close() : open();
      return;
    }
    if (!overlay.classList.contains("open")) return;
    if (e.key === "Escape") { close(); return; }
    if (e.key === "ArrowDown") {
      e.preventDefault();
      if (currentMatches.length) { selectedIndex = Math.min(selectedIndex + 1, currentMatches.length - 1); render(currentMatches); }
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      if (currentMatches.length) { selectedIndex = Math.max(selectedIndex - 1, 0); render(currentMatches); }
    } else if (e.key === "Enter") {
      if (selectedIndex >= 0 && currentMatches[selectedIndex]) {
        window.location.href = currentMatches[selectedIndex].href;
      }
    }
  });
});
