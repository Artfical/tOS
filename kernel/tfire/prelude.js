/*
 * tFire's DOM: what a page's scripts see as window, document and their nodes, written over the natives of
 * js.c (__tf), which reach the real document tree. Run once in every page before its own scripts.
 */
(function () {
'use strict';
const T = globalThis.__tf;
delete globalThis.__tf;
const G = globalThis;

/* ------------------------------------------------------------------ small helpers */

const NS_HTML = 1, NS_MATHML = 2, NS_SVG = 3;
const NSURI = { 1: 'http://www.w3.org/1999/xhtml', 2: 'http://www.w3.org/1998/Math/MathML', 3: 'http://www.w3.org/2000/svg' };
function nsOf(uri) { return uri === NSURI[3] ? NS_SVG : uri === NSURI[2] ? NS_MATHML : NS_HTML; }
const def = (o, name, get, set) => Object.defineProperty(o, name, { get, set: set || undefined, configurable: true, enumerable: true });
const hidden = (o, name, value) => Object.defineProperty(o, name, { value, writable: true, configurable: true, enumerable: false });
function illegal() { throw new TypeError('Illegal constructor'); }
function str(v) { return v === null || v === undefined ? '' : String(v); }
function kebab(p) {
    if (p === 'cssFloat') return 'float';
    if (p.startsWith('--')) return p;
    let k = p.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
    if (/^(webkit|moz|ms|o)-/.test(k)) k = '-' + k;
    return k;
}
function camel(k) { return k.startsWith('--') ? k : k.replace(/^-/, '').replace(/-([a-z])/g, (m, c) => c.toUpperCase()); }
function list(arr) {
    hidden(arr, 'item', i => (i >= 0 && i < arr.length ? arr[i] : null));
    hidden(arr, 'namedItem', n => arr.find(e => e.id === n || e.getAttribute('name') === n) || null);
    return arr;
}
function children(n) { const r = []; for (let c = T.first(n); c; c = T.next(c)) r.push(c); return r; }
function elemChildren(n) { const r = []; for (let c = T.first(n); c; c = T.next(c)) if (T.type(c) === 2) r.push(c); return r; }
function logArgs(args) {
    return Array.prototype.map.call(args, a => {
        if (typeof a === 'string') return a;
        if (a instanceof Error) return a.name + ': ' + a.message;
        if (a instanceof Node) return '<' + a.nodeName.toLowerCase() + '>';
        try { return typeof a === 'object' && a !== null ? JSON.stringify(a).slice(0, 400) : String(a); } catch (e) { return String(a); }
    }).join(' ');
}

/* ------------------------------------------------------------------ mutation observers (fed by the methods below) */

const observers = [];
let mutationQueued = false;
function queueMutation(rec) {
    if (!observers.length) return;
    for (const mo of observers) {
        for (const o of mo._targets) {
            const t = rec.target;
            const hit = t === o.node || (o.opts.subtree && T.contains(o.node, t));
            if (!hit) continue;
            if (rec.type === 'childList' && !o.opts.childList) continue;
            if (rec.type === 'attributes' && !o.opts.attributes && !o.opts.attributeFilter && !o.opts.attributeOldValue) continue;
            if (rec.type === 'attributes' && o.opts.attributeFilter && !o.opts.attributeFilter.includes(rec.attributeName)) continue;
            if (rec.type === 'characterData' && !o.opts.characterData) continue;
            mo._records.push(rec);
            break;
        }
    }
    if (!mutationQueued) {
        mutationQueued = true;
        Promise.resolve().then(() => {
            mutationQueued = false;
            for (const mo of observers.slice()) {
                if (!mo._records.length) continue;
                const recs = mo._records; mo._records = [];
                try { mo._cb.call(mo, recs, mo); } catch (e) { console.error(e); }
            }
        });
    }
}
function mutated(target, added, removed, prev, next) {
    if (observers.length) queueMutation({ type: 'childList', target, addedNodes: list(added || []), removedNodes: list(removed || []),
        previousSibling: prev || null, nextSibling: next || null, attributeName: null, oldValue: null });
}

class MutationObserver {
    constructor(cb) { this._cb = cb; this._targets = []; this._records = []; }
    observe(node, opts) {
        opts = Object.assign({}, opts || {});
        if (opts.attributeOldValue || opts.attributeFilter) opts.attributes = true;
        this._targets = this._targets.filter(o => o.node !== node);
        this._targets.push({ node, opts });
        if (!observers.includes(this)) observers.push(this);
    }
    disconnect() { this._targets = []; const i = observers.indexOf(this); if (i >= 0) observers.splice(i, 1); }
    takeRecords() { const r = this._records; this._records = []; return r; }
}

/* ------------------------------------------------------------------ events */

let evTime0 = T.perf();
class Event {
    constructor(type, init) {
        if (arguments.length === 0) throw new TypeError("Failed to construct 'Event': 1 argument required");
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        this.isTrusted = false;
        this.timeStamp = T.perf() - evTime0;
        this._stop = false;
        this._stopNow = false;
        this._passive = false;
    }
    preventDefault() { if (this.cancelable && !this._passive) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }
    composedPath() { return this._path ? this._path.slice() : []; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this._stop; }
    set cancelBubble(v) { if (v) this._stop = true; }
    get srcElement() { return this.target; }
    initEvent(type, bubbles, cancelable) { this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class UIEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.view = i.view || G; this.detail = i.detail || 0; this.which = i.which || 0; } }
class MouseEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.clientX = this.x = this.pageX = this.screenX = this.offsetX = i.clientX || 0;
        this.clientY = this.y = this.screenY = this.offsetY = i.clientY || 0;
        this.pageY = (i.clientY || 0) + T.scrollY();
        this.button = i.button || 0; this.buttons = i.buttons || 0;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
        this.relatedTarget = i.relatedTarget || null;
        this.movementX = this.movementY = 0;
    }
    getModifierState() { return false; }
}
class PointerEvent extends MouseEvent { constructor(t, i) { super(t, i); i = i || {}; this.pointerId = 1; this.pointerType = i.pointerType || 'mouse'; this.isPrimary = true; this.width = this.height = 1; this.pressure = 0.5; } }
class WheelEvent extends MouseEvent { constructor(t, i) { super(t, i); i = i || {}; this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; } }
class KeyboardEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; this.charCode = i.charCode || 0;
        this.which = this.keyCode; this.location = 0; this.repeat = !!i.repeat; this.isComposing = false;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
    }
    getModifierState() { return false; }
}
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = (i && i.relatedTarget) || null; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || ''; this.isComposing = false; } }
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.detail = d; } }
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = (i && i.submitter) || null; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.lengthComputable = !!i.lengthComputable; this.loaded = i.loaded || 0; this.total = i.total || 0; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; this.ports = []; this.lastEventId = ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state !== undefined ? i.state : null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; this.filename = i.filename || ''; this.lineno = i.lineno || 0; this.colno = 0; } }
class PromiseRejectionEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.promise = i.promise; this.reason = i.reason; } }
class TransitionEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.propertyName = i.propertyName || ''; this.elapsedTime = i.elapsedTime || 0; this.pseudoElement = i.pseudoElement || ''; } }
class AnimationEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.animationName = i.animationName || ''; this.elapsedTime = i.elapsedTime || 0; this.pseudoElement = i.pseudoElement || ''; } }
class ClipboardEvent extends Event { constructor(t, i) { super(t, i); this.clipboardData = null; } }
class TouchEvent extends UIEvent { constructor(t, i) { super(t, i); this.touches = []; this.targetTouches = []; this.changedTouches = []; } }

/* listeners live on the object itself, out of sight */
function listeners(o, make) {
    let l = o.__ls;
    if (!l && make) { l = {}; hidden(o, '__ls', l); }
    return l;
}

class EventTarget {
    addEventListener(type, fn, opts) {
        if (!fn) return;
        const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
        const l = listeners(this, true);
        const arr = l[type] || (l[type] = []);
        if (arr.some(e => e.fn === fn && e.capture === capture)) return;
        const ent = { fn, capture, once: !!(opts && opts.once), passive: !!(opts && opts.passive) };
        arr.push(ent);
        if (opts && opts.signal) opts.signal.addEventListener('abort', () => this.removeEventListener(type, fn, opts));
    }
    removeEventListener(type, fn, opts) {
        const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
        const l = listeners(this, false);
        if (!l || !l[type]) return;
        l[type] = l[type].filter(e => !(e.fn === fn && e.capture === capture));
    }
    dispatchEvent(ev) { return dispatch(this, ev); }
}

/* an inline handler (onclick="...") becomes a function once, again when the attribute changes */
function inlineHandler(node, type) {
    if (!(node instanceof Element)) return null;
    const code = T.getAttr(node, 'on' + type);
    if (code === null) return null;
    const cache = node.__ih || (hidden(node, '__ih', {}), node.__ih);
    if (cache[type] && cache[type].code === code) return cache[type].fn;
    let fn = null;
    try { fn = new Function('event', 'with (this) {\n' + code + '\n}'); } catch (e) { console.error('inline on' + type + ': ' + e.message); }
    cache[type] = { code, fn };
    return fn;
}

function callListener(cur, ev, fn) {
    try {
        const r = typeof fn === 'function' ? fn.call(cur, ev) : fn && typeof fn.handleEvent === 'function' ? fn.handleEvent(ev) : undefined;
        if (r === false) ev.preventDefault();
    } catch (e) { reportError(e); }
}

/* phase 1 capturing (capture listeners), 2 at the target (all of them, in order), 3 bubbling (the others) */
function invoke(cur, ev, phase) {
    ev.currentTarget = cur;
    if (phase !== 1) {
        /* the on<type> property or attribute */
        const h = Object.prototype.hasOwnProperty.call(cur, '__on') && cur.__on[ev.type] !== undefined ? cur.__on[ev.type]
            : cur === G ? (typeof G['on' + ev.type] === 'function' ? G['on' + ev.type] : null) : inlineHandler(cur, ev.type);
        if (h) {
            if (ev.type === 'error' && cur === G && ev instanceof ErrorEvent) {
                try { if (h.call(cur, ev.message, ev.filename, ev.lineno, 0, ev.error) === true) ev.preventDefault(); } catch (e) {}
            } else callListener(cur, ev, h);
        }
    }
    const l = listeners(cur, false);
    if (!l || !l[ev.type]) return;
    for (const ent of l[ev.type].slice()) {
        if (ev._stopNow) break;
        if (phase === 1 && !ent.capture) continue;
        if (phase === 3 && ent.capture) continue;
        if (ent.once) cur.removeEventListener(ev.type, ent.fn, { capture: ent.capture });
        ev._passive = ent.passive;
        callListener(cur, ev, ent.fn);
        ev._passive = false;
    }
}

function dispatch(target, ev) {
    ev.target = target;
    const path = [target];
    if (target instanceof Node) {
        for (let p = T.parent(target); p; p = T.parent(p)) path.push(p);
        if (T.type(path[path.length - 1]) === 1 && ev.type !== 'load') path.push(G);
    }
    ev._path = path;
    for (let i = path.length - 1; i > 0 && !ev._stop; i--) { ev.eventPhase = 1; invoke(path[i], ev, 1); }
    if (!ev._stop) { ev.eventPhase = 2; invoke(target, ev, 2); }
    if (ev.bubbles) for (let i = 1; i < path.length && !ev._stop; i++) { ev.eventPhase = 3; invoke(path[i], ev, 3); }
    ev.eventPhase = 0;
    ev.currentTarget = null;
    return !ev.defaultPrevented;
}

function reportError(e) {
    const msg = e && e.message ? e.message : String(e);
    const ev = new ErrorEvent('error', { message: msg, error: e, cancelable: true, filename: e && e.fileName || '', lineno: e && e.lineNumber || 0 });
    let handled = false;
    try { dispatch(G, ev); handled = ev.defaultPrevented; } catch (x) {}
    if (!handled) T.log(2, 'Uncaught ' + (e && e.name ? e.name + ': ' : '') + msg + (e && e.stack ? '\n' + e.stack : ''));
}

/* on<event> properties for every kind of target */
const EVENTS = ['abort', 'animationcancel', 'animationiteration', 'transitioncancel', 'transitionrun', 'transitionstart', 'blur', 'cancel', 'change', 'click', 'close', 'contextmenu', 'dblclick', 'drag', 'dragend', 'dragenter',
    'dragleave', 'dragover', 'dragstart', 'drop', 'error', 'focus', 'focusin', 'focusout', 'input', 'invalid', 'keydown', 'keypress', 'keyup',
    'load', 'loadend', 'loadstart', 'mousedown', 'mouseenter', 'mouseleave', 'mousemove', 'mouseout', 'mouseover', 'mouseup', 'wheel',
    'pointerdown', 'pointerup', 'pointermove', 'pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'pointercancel',
    'reset', 'resize', 'scroll', 'select', 'submit', 'toggle', 'touchstart', 'touchend', 'touchmove', 'touchcancel', 'transitionend',
    'animationend', 'animationstart', 'readystatechange', 'progress', 'timeout', 'beforeinput', 'paste', 'copy', 'cut',
    'beforeunload', 'unload', 'pagehide', 'pageshow', 'popstate', 'hashchange', 'message', 'online', 'offline', 'storage',
    'visibilitychange', 'DOMContentLoaded', 'selectionchange', 'securitypolicyviolation', 'unhandledrejection', 'rejectionhandled'];
function onProps(proto, names) {
    for (const n of names) {
        Object.defineProperty(proto, 'on' + n, {
            get() { return Object.prototype.hasOwnProperty.call(this, '__on') && this.__on[n] !== undefined ? this.__on[n] : (this instanceof Element ? inlineHandler(this, n) : null); },
            set(fn) { if (!Object.prototype.hasOwnProperty.call(this, '__on')) hidden(this, '__on', {}); this.__on[n] = typeof fn === 'function' || (fn && typeof fn === 'object') ? fn : null; },
            configurable: true, enumerable: true,
        });
    }
}

/* ------------------------------------------------------------------ nodes */

const NT = { 1: 9, 2: 1, 3: 3, 4: 8, 5: 10, 6: 11 };

class Node extends EventTarget {
    constructor() { super(); illegal(); }
    get nodeType() { return NT[T.type(this)] || 1; }
    get nodeName() {
        switch (T.type(this)) {
        case 1: return '#document';
        case 2: { const n = T.name(this); return T.ns(this) === NS_HTML || T.ns(this) === 0 ? n.toUpperCase() : n; }
        case 3: return '#text';
        case 4: return '#comment';
        case 5: return 'html';
        default: return '#document-fragment';
        }
    }
    get nodeValue() { const t = T.type(this); return t === 3 || t === 4 ? T.text(this) : null; }
    set nodeValue(v) { const t = T.type(this); if (t === 3 || t === 4) this.data = v; }
    get textContent() { const t = T.type(this); return t === 1 || t === 5 ? null : T.text(this); }
    set textContent(v) {
        const t = T.type(this);
        if (t === 1 || t === 5) return;
        const old = observers.length ? children(this) : null;
        T.setText(this, str(v));
        if (old) mutated(this, children(this), old);
    }
    get parentNode() { return T.parent(this); }
    get parentElement() { const p = T.parent(this); return p && T.type(p) === 2 ? p : null; }
    get childNodes() { return list(children(this)); }
    get firstChild() { return T.first(this); }
    get lastChild() { return T.last(this); }
    get nextSibling() { return T.next(this); }
    get previousSibling() { return T.prev(this); }
    get ownerDocument() { return T.type(this) === 1 ? null : document; }
    get isConnected() { return T.connected(this); }
    get baseURI() { return document.baseURI; }
    hasChildNodes() { return !!T.first(this); }
    getRootNode() { let n = this; for (let p = T.parent(n); p; p = T.parent(p)) n = p; return n; }
    contains(o) { return !!o && T.contains(this, o); }
    isSameNode(o) { return this === o; }
    isEqualNode(o) { return !!o && o.nodeType === this.nodeType && (T.type(this) === 2 ? T.outer(this) === T.outer(o) : this.textContent === o.textContent); }
    compareDocumentPosition(o) {
        if (o === this) return 0;
        if (T.contains(this, o)) return 20;           /* contained by, following */
        if (T.contains(o, this)) return 10;           /* contains, preceding */
        if (!T.connected(this) || !T.connected(o)) return 1 | 32 | 4;
        for (let n = this; n; ) {                    /* walk forward in document order */
            let nx = T.first(n);
            if (!nx) { while (n && !T.next(n)) n = T.parent(n); nx = n ? T.next(n) : null; }
            n = nx;
            if (n === o) return 4;
        }
        return 2;
    }
    appendChild(c) { return this.insertBefore(c, null); }
    insertBefore(c, ref) {
        if (!(c instanceof Node)) throw new TypeError("Failed to execute 'insertBefore': parameter 1 is not of type 'Node'.");
        if (ref !== null && ref !== undefined && T.parent(ref) !== this) throw new DOMException('The node before which the new node is to be inserted is not a child of this node.', 'NotFoundError');
        const added = T.type(c) === 6 ? children(c) : [c];
        const oldParent = T.parent(c);
        if (oldParent && observers.length) mutated(oldParent, [], [c]);
        T.insert(this, c, ref || null);
        if (observers.length) mutated(this, added, [], T.prev(added[0]), ref || null);
        for (const a of added) connected(a);
        return c;
    }
    removeChild(c) {
        if (!c || T.parent(c) !== this) throw new DOMException('The node to be removed is not a child of this node.', 'NotFoundError');
        const prev = T.prev(c), next = T.next(c);
        T.remove(c);
        if (observers.length) mutated(this, [], [c], prev, next);
        disconnected(c);
        return c;
    }
    replaceChild(n, old) { this.insertBefore(n, old); return this.removeChild(old); }
    cloneNode(deep) { const c = T.clone(this, !!deep); upgrade(c); return c; }
    normalize() {}
    lookupNamespaceURI() { return null; }
    isDefaultNamespace() { return true; }
}
Object.assign(Node, { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7,
    COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
    DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4,
    DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16 });
Object.assign(Node.prototype, { ELEMENT_NODE: 1, TEXT_NODE: 3, COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_FRAGMENT_NODE: 11 });

/* custom elements: connectedCallback when one goes into the document */
function connected(n) {
    if (!customDefs.size || !T.connected(n)) return;
    const walk = e => {
        if (T.type(e) === 2) {
            upgrade(e);
            if (typeof e.connectedCallback === 'function') { try { e.connectedCallback(); } catch (x) { reportError(x); } }
        }
        for (let c = T.first(e); c; c = T.next(c)) walk(c);
    };
    walk(n);
}
function disconnected(n) {
    if (!customDefs.size) return;
    const walk = e => {
        if (T.type(e) === 2 && typeof e.disconnectedCallback === 'function') { try { e.disconnectedCallback(); } catch (x) { reportError(x); } }
        for (let c = T.first(e); c; c = T.next(c)) walk(c);
    };
    walk(n);
}

/* what children of all kinds can do (ParentNode, ChildNode) */
function toNodes(args) {
    return Array.prototype.map.call(args, a => (a instanceof Node ? a : T.createText(str(a))));
}
const ParentMixin = {
    get children() { return list(elemChildren(this)); },
    get childElementCount() { return elemChildren(this).length; },
    get firstElementChild() { for (let c = T.first(this); c; c = T.next(c)) if (T.type(c) === 2) return c; return null; },
    get lastElementChild() { for (let c = T.last(this); c; c = T.prev(c)) if (T.type(c) === 2) return c; return null; },
    append(...a) { for (const n of toNodes(a)) this.appendChild(n); },
    prepend(...a) { const f = T.first(this); for (const n of toNodes(a)) this.insertBefore(n, f); },
    replaceChildren(...a) { while (T.first(this)) this.removeChild(T.first(this)); this.append(...a); },
    querySelector(s) { return query(this, s, false); },
    querySelectorAll(s) { return list(query(this, s, true)); },
    getElementsByTagName(t) { t = String(t).toLowerCase(); return list(t === '*' ? T.queryAll(this, '*') : T.queryAll(this, t.replace(/[^a-z0-9_-]/g, ''))); },
    getElementsByTagNameNS(ns, t) { return this.getElementsByTagName(t); },
    getElementsByClassName(c) {
        const cls = String(c).trim().split(/\s+/).filter(Boolean);
        if (!cls.length) return list([]);
        return list(T.queryAll(this, cls.map(x => '.' + x.replace(/([^\w-])/g, '\\$1')).join('')));
    },
};
const ChildMixin = {
    get nextElementSibling() { for (let c = T.next(this); c; c = T.next(c)) if (T.type(c) === 2) return c; return null; },
    get previousElementSibling() { for (let c = T.prev(this); c; c = T.prev(c)) if (T.type(c) === 2) return c; return null; },
    remove() { const p = T.parent(this); if (p) p.removeChild(this); },
    before(...a) { const p = T.parent(this); if (p) for (const n of toNodes(a)) p.insertBefore(n, this); },
    after(...a) { const p = T.parent(this); if (!p) return; const nx = T.next(this); for (const n of toNodes(a)) p.insertBefore(n, nx); },
    replaceWith(...a) { const p = T.parent(this); if (!p) return; const nx = T.next(this); p.removeChild(this); for (const n of toNodes(a)) p.insertBefore(n, nx); },
};
function mixin(cls, m) { Object.defineProperties(cls.prototype, Object.getOwnPropertyDescriptors(m)); }

class CharacterData extends Node {
    get data() { return T.text(this); }
    set data(v) {
        const old = T.text(this);
        T.setText(this, str(v));
        if (observers.length) queueMutation({ type: 'characterData', target: this, oldValue: old, addedNodes: list([]), removedNodes: list([]) });
    }
    get length() { return this.data.length; }
    appendData(s) { this.data += s; }
    substringData(o, n) { return this.data.substr(o, n); }
    insertData(o, s) { const d = this.data; this.data = d.slice(0, o) + s + d.slice(o); }
    deleteData(o, n) { const d = this.data; this.data = d.slice(0, o) + d.slice(o + n); }
    replaceData(o, n, s) { const d = this.data; this.data = d.slice(0, o) + s + d.slice(o + n); }
}
mixin(CharacterData, ChildMixin);
class Text extends CharacterData {
    constructor(data) { return T.createText(str(data)); }
    get wholeText() { return this.data; }
    splitText(off) { const d = this.data; this.data = d.slice(0, off); const t = T.createText(d.slice(off)); this.after(t); return t; }
}
class CDATASection extends Text {}
class Comment extends CharacterData { constructor(data) { return T.createComment(str(data)); } }
class DocumentType extends Node { get name() { return 'html'; } get publicId() { return ''; } get systemId() { return ''; } }
mixin(DocumentType, ChildMixin);
class DocumentFragment extends Node {
    constructor() { return T.createFrag(); }
    getElementById(id) { return query(this, '#' + cssEscape(id), false); }
}
mixin(DocumentFragment, ParentMixin);
class ShadowRoot extends DocumentFragment {}

class DOMException extends Error {
    constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 0; }
}
for (const [n, c] of Object.entries({ INDEX_SIZE_ERR: 1, HIERARCHY_REQUEST_ERR: 3, NOT_FOUND_ERR: 8, NOT_SUPPORTED_ERR: 9,
    INVALID_STATE_ERR: 11, SYNTAX_ERR: 12, SECURITY_ERR: 18, NETWORK_ERR: 19, ABORT_ERR: 20, TIMEOUT_ERR: 23, DATA_CLONE_ERR: 25 })) DOMException[n] = c;

function cssEscape(s) { return String(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); }

/* ------------------------------------------------------------------ selectors */

/* Selectors the tree can match itself (tags, ids, classes, attributes, combinators) go to dom.c; the rest
 * (pseudo-classes, :not(), :is(), :has(), nth-child...) are matched here. */
const selCache = new Map();
function parseSelector(sel) {
    let c = selCache.get(sel);
    if (c) return c;
    let i = 0;
    const s = sel;
    const ws = () => { while (i < s.length && /\s/.test(s[i])) i++; };
    const ident = () => {
        let r = '';
        while (i < s.length) {
            const ch = s[i];
            if (ch === '\\') {
                i++;
                const hex = /^[0-9a-fA-F]{1,6}\s?/.exec(s.slice(i));
                if (hex) { r += String.fromCodePoint(parseInt(hex[0], 16)); i += hex[0].length; }
                else r += s[i++];
            } else if (/[\w\-\u0080-￿]/.test(ch)) { r += ch; i++; }
            else break;
        }
        return r;
    };
    const parenArg = () => {
        let depth = 1, start = ++i;
        while (i < s.length && depth) { if (s[i] === '(') depth++; else if (s[i] === ')') depth--; if (depth) i++; }
        const a = s.slice(start, i);
        i++;
        return a;
    };
    const compound = () => {
        const c = { tag: null, id: null, classes: [], attrs: [], pseudos: [] };
        let any = false;
        if (s[i] === '*') { i++; any = true; }
        else if (/[\w\\\-\u0080-￿]/.test(s[i] || '')) { c.tag = ident().toLowerCase(); any = true; }
        for (;;) {
            const ch = s[i];
            if (ch === '#') { i++; c.id = ident(); any = true; }
            else if (ch === '.') { i++; c.classes.push(ident()); any = true; }
            else if (ch === '[') {
                i++; ws();
                const name = ident().toLowerCase(); ws();
                let op = null, val = null, ci = false;
                if (s[i] !== ']') {
                    op = s[i] === '=' ? '=' : s.slice(i, i + 2); i += op.length; ws();
                    if (s[i] === '"' || s[i] === "'") { const q = s[i++]; let v = ''; while (i < s.length && s[i] !== q) { if (s[i] === '\\') i++; v += s[i++]; } i++; val = v; }
                    else val = ident();
                    ws();
                    if (s[i] === 'i' || s[i] === 'I') { ci = true; i++; ws(); } else if (s[i] === 's') { i++; ws(); }
                }
                if (s[i] === ']') i++;
                c.attrs.push({ name, op, val, ci });
                any = true;
            } else if (ch === ':') {
                i++;
                let el = false;
                if (s[i] === ':') { i++; el = true; }
                const name = ident().toLowerCase();
                const arg = s[i] === '(' ? parenArg() : null;
                c.pseudos.push({ name, arg, el });
                any = true;
            } else break;
        }
        if (!any) throw new DOMException("'" + sel + "' is not a valid selector.", 'SyntaxError');
        return c;
    };
    const groups = [];
    ws();
    while (i < s.length) {
        const chain = [compound()];               /* compounds and the combinators between them, left to right */
        for (;;) {
            const before = i;
            ws();
            if (i >= s.length || s[i] === ',' || s[i] === ')') break;
            let comb = ' ';
            if (s[i] === '>' || s[i] === '+' || s[i] === '~') { comb = s[i++]; ws(); }
            else if (i === before) break;
            chain.push(comb, compound());
        }
        groups.push(chain);
        ws();
        if (s[i] === ',') { i++; ws(); } else if (i < s.length) throw new DOMException("'" + sel + "' is not a valid selector.", 'SyntaxError');
    }
    if (!groups.length) throw new DOMException("'" + sel + "' is not a valid selector.", 'SyntaxError');
    c = groups;
    if (selCache.size > 500) selCache.clear();
    selCache.set(sel, c);
    return c;
}

function elemSiblings(e) { const p = T.parent(e); return p ? elemChildren(p) : [e]; }
function nth(arg, pos) {
    arg = arg.trim().toLowerCase();
    let a = 0, b = 0;
    if (arg === 'odd') { a = 2; b = 1; } else if (arg === 'even') { a = 2; b = 0; }
    else {
        const m = /^([+-]?\d*)n\s*(?:([+-])\s*(\d+))?/.exec(arg.split(/\s+of\s+/)[0]);
        if (m) { a = m[1] === '' || m[1] === '+' ? 1 : m[1] === '-' ? -1 : parseInt(m[1], 10); b = m[3] ? parseInt(m[3], 10) * (m[2] === '-' ? -1 : 1) : 0; }
        else b = parseInt(arg, 10) || 0;
    }
    if (a === 0) return pos === b;
    return (pos - b) / a >= 0 && (pos - b) % a === 0;
}

function matchPseudo(e, p, scope) {
    switch (p.name) {
    case 'not': return !matchesAny(e, p.arg, scope);
    case 'is': case 'matches': case 'where': case '-webkit-any': case '-moz-any': return matchesAny(e, p.arg, scope);
    case 'has': {
        const a = p.arg.trim();
        if (a[0] === '>') return elemChildren(e).some(k => matchesAny(k, a.slice(1), scope));
        if (a[0] === '+') { const n = e.nextElementSibling; return !!n && matchesAny(n, a.slice(1), scope); }
        if (a[0] === '~') { for (let n = e.nextElementSibling; n; n = n.nextElementSibling) if (matchesAny(n, a.slice(1), scope)) return true; return false; }
        return query(e, a, false) !== null;
    }
    case 'scope': return e === scope || (!scope && T.type(T.parent(e)) === 1);
    case 'root': return T.type(T.parent(e)) === 1;
    case 'first-child': return elemSiblings(e)[0] === e;
    case 'last-child': { const s = elemSiblings(e); return s[s.length - 1] === e; }
    case 'only-child': return elemSiblings(e).length === 1;
    case 'nth-child': return nth(p.arg, elemSiblings(e).indexOf(e) + 1);
    case 'nth-last-child': { const s = elemSiblings(e); return nth(p.arg, s.length - s.indexOf(e)); }
    case 'first-of-type': case 'last-of-type': case 'only-of-type': case 'nth-of-type': case 'nth-last-of-type': {
        const name = T.name(e), s = elemSiblings(e).filter(k => T.name(k) === name), i = s.indexOf(e);
        if (p.name === 'first-of-type') return i === 0;
        if (p.name === 'last-of-type') return i === s.length - 1;
        if (p.name === 'only-of-type') return s.length === 1;
        if (p.name === 'nth-of-type') return nth(p.arg, i + 1);
        return nth(p.arg, s.length - i);
    }
    case 'empty': return !T.first(e) || children(e).every(k => T.type(k) === 4 || (T.type(k) === 3 && T.text(k) === ''));
    case 'checked': return T.checked(e) || (T.name(e) === 'option' && e.selected);
    case 'disabled': return T.getAttr(e, 'disabled') !== null;
    case 'enabled': return /^(input|button|select|textarea|option|fieldset)$/.test(T.name(e)) && T.getAttr(e, 'disabled') === null;
    case 'required': return T.getAttr(e, 'required') !== null;
    case 'optional': return /^(input|select|textarea)$/.test(T.name(e)) && T.getAttr(e, 'required') === null;
    case 'read-only': return T.getAttr(e, 'readonly') !== null;
    case 'read-write': return /^(input|textarea)$/.test(T.name(e)) && T.getAttr(e, 'readonly') === null;
    case 'placeholder-shown': return T.getAttr(e, 'placeholder') !== null && T.value(e) === '';
    case 'link': case 'any-link': return (T.name(e) === 'a' || T.name(e) === 'area') && T.getAttr(e, 'href') !== null;
    case 'focus': case 'focus-within': case 'focus-visible': { const f = T.focused(); return !!f && (f === e || (p.name === 'focus-within' && T.contains(e, f))); }
    case 'target': return !!location.hash && e.id === decodeURIComponent(location.hash.slice(1));
    case 'defined': return true;
    case 'lang': { for (let n = e; n && T.type(n) === 2; n = T.parent(n)) { const l = T.getAttr(n, 'lang'); if (l !== null) return l.toLowerCase().startsWith(p.arg.toLowerCase()); } return false; }
    case 'valid': return true;
    case 'invalid': return false;
    default: return p.el;                          /* ::before and the like: the element itself; unknown classes: no */
    }
}

function attrTest(have, a) {
    if (have === null) return false;
    if (!a.op) return true;
    let h = have, v = a.val;
    if (a.ci) { h = h.toLowerCase(); v = v.toLowerCase(); }
    switch (a.op) {
    case '=': return h === v;
    case '~=': return v !== '' && h.split(/\s+/).includes(v);
    case '|=': return h === v || h.startsWith(v + '-');
    case '^=': return v !== '' && h.startsWith(v);
    case '$=': return v !== '' && h.endsWith(v);
    case '*=': return v !== '' && h.includes(v);
    }
    return false;
}

function matchCompound(e, c, scope) {
    if (T.type(e) !== 2) return false;
    if (c.tag && c.tag !== T.name(e).toLowerCase()) return false;
    if (c.id !== null && T.getAttr(e, 'id') !== c.id) return false;
    if (c.classes.length) {
        const cl = T.getAttr(e, 'class');
        if (cl === null) return false;
        const have = cl.split(/\s+/);
        for (const k of c.classes) if (!have.includes(k)) return false;
    }
    for (const a of c.attrs) if (!attrTest(T.getAttr(e, a.name), a)) return false;
    for (const p of c.pseudos) if (!matchPseudo(e, p, scope)) return false;
    return true;
}

function matchChain(e, chain, k, scope) {
    if (!matchCompound(e, chain[k], scope)) return false;
    if (k === 0) return true;
    const comb = chain[k - 1];
    if (comb === '>') { const p = T.parent(e); return !!p && T.type(p) === 2 && matchChain(p, chain, k - 2, scope); }
    if (comb === '+') { const p = e.previousElementSibling; return !!p && matchChain(p, chain, k - 2, scope); }
    if (comb === '~') { for (let p = e.previousElementSibling; p; p = p.previousElementSibling) if (matchChain(p, chain, k - 2, scope)) return true; return false; }
    for (let p = T.parent(e); p && T.type(p) === 2; p = T.parent(p)) if (matchChain(p, chain, k - 2, scope)) return true;
    return false;
}

function matchesAny(e, sel, scope) {
    const groups = typeof sel === 'string' ? parseSelector(sel) : sel;
    for (const g of groups) if (matchChain(e, g, g.length - 1, scope)) return true;
    return false;
}

const simpleSel = /^[\s\w\-#.,>*\[\]=~^$|+]*$/;
function query(root, sel, all) {
    sel = String(sel);
    if (simpleSel.test(sel) && !/[+~]\s*$|^\s*[>+~]|\\/.test(sel)) {
        try { parseSelector(sel); } catch (e) { throw e; }
        return all ? T.queryAll(root, sel) : T.query(root, sel);
    }
    const groups = parseSelector(sel);
    const scope = T.type(root) === 2 ? root : null;
    const out = [];
    const walk = n => {
        for (let c = T.first(n); c; c = T.next(c)) {
            if (T.type(c) !== 2) continue;
            for (const g of groups) if (matchChain(c, g, g.length - 1, scope)) { out.push(c); break; }
            if (!all && out.length) return true;
            if (walk(c) && !all) return true;
        }
        return false;
    };
    walk(root);
    return all ? out : out[0] || null;
}

/* ------------------------------------------------------------------ style="" as an object */

function parseDecls(text) {
    const m = new Map();
    if (!text) return m;
    let depth = 0, cur = '', q = null;
    const parts = [];
    for (const ch of text) {
        if (q) { if (ch === q) q = null; cur += ch; continue; }
        if (ch === '"' || ch === "'") q = ch;
        if (ch === '(') depth++;
        if (ch === ')') depth--;
        if (ch === ';' && !depth) { parts.push(cur); cur = ''; } else cur += ch;
    }
    parts.push(cur);
    for (const p of parts) {
        const i = p.indexOf(':');
        if (i < 0) continue;
        const k = p.slice(0, i).trim().toLowerCase(), v = p.slice(i + 1).trim();
        if (k) m.set(k.startsWith('--') ? p.slice(0, i).trim() : k, v);
    }
    return m;
}
function serializeDecls(m) { let s = ''; for (const [k, v] of m) s += (s ? ' ' : '') + k + ': ' + v + ';'; return s; }

const styleMethods = {
    getPropertyValue(k) { const m = parseDecls(T.getAttr(this._el, 'style')); return (m.get(k) || '').replace(/\s*!important$/, ''); },
    getPropertyPriority(k) { const m = parseDecls(T.getAttr(this._el, 'style')); return /!important$/.test(m.get(k) || '') ? 'important' : ''; },
    setProperty(k, v, prio) {
        const m = parseDecls(T.getAttr(this._el, 'style'));
        v = str(v).trim();
        if (v === '') m.delete(k); else m.set(k, prio === 'important' ? v + ' !important' : v);
        setAttribute(this._el, 'style', serializeDecls(m));
    },
    removeProperty(k) { const old = this.getPropertyValue(k); this.setProperty(k, ''); return old; },
    item(i) { return [...parseDecls(T.getAttr(this._el, 'style')).keys()][i] || ''; },
};
function styleOf(el) {
    if (el.__style) return el.__style;
    const target = Object.assign(Object.create(CSSStyleDeclaration.prototype), styleMethods);
    hidden(target, '_el', el);
    const p = new Proxy(target, {
        get(t, k) {
            if (typeof k === 'symbol' || k in t) return t[k];
            if (k === 'cssText') return T.getAttr(el, 'style') || '';
            if (k === 'length') return parseDecls(T.getAttr(el, 'style')).size;
            if (k === 'parentRule') return null;
            if (/^\d+$/.test(k)) return t.item(+k);
            return t.getPropertyValue(kebab(k));
        },
        set(t, k, v) {
            if (typeof k === 'symbol') { t[k] = v; return true; }
            if (k === 'cssText') { setAttribute(el, 'style', str(v)); return true; }
            const val = v === null || v === undefined ? '' : typeof v === 'number' && v !== 0 && !/^(opacity|zIndex|flex|flexGrow|flexShrink|order|lineHeight|fontWeight|zoom)$/.test(k) ? v + 'px' : String(v);
            t.setProperty(kebab(k), val);
            return true;
        },
        has(t, k) { return typeof k === 'string'; },
    });
    hidden(el, '__style', p);
    return p;
}
class CSSStyleDeclaration { constructor() { illegal(); } }

/* ------------------------------------------------------------------ classList */

class DOMTokenList {
    constructor(el, attr) { hidden(this, '_el', el); hidden(this, '_attr', attr || 'class'); }
    _get() { const v = T.getAttr(this._el, this._attr); return v ? v.split(/\s+/).filter(Boolean) : []; }
    _set(a) { setAttribute(this._el, this._attr, a.join(' ')); }
    get length() { return this._get().length; }
    get value() { return T.getAttr(this._el, this._attr) || ''; }
    set value(v) { setAttribute(this._el, this._attr, str(v)); }
    item(i) { return this._get()[i] || null; }
    contains(t) { return this._get().includes(String(t)); }
    add(...ts) { const a = this._get(); let ch = false; for (const t of ts) if (!a.includes(t)) { a.push(String(t)); ch = true; } if (ch || T.getAttr(this._el, this._attr) === null) this._set(a); }
    remove(...ts) { const a = this._get(); const b = a.filter(x => !ts.includes(x)); if (b.length !== a.length) this._set(b); }
    toggle(t, force) {
        const has = this.contains(t);
        const want = force === undefined ? !has : !!force;
        if (want && !has) this.add(t); else if (!want && has) this.remove(t);
        return want;
    }
    replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l.filter((x, k) => l.indexOf(x) === k)); return true; }
    supports() { return true; }
    forEach(fn, self) { this._get().forEach((v, i) => fn.call(self, v, i, this)); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
    toString() { return this.value; }
}

function datasetOf(el) {
    if (el.__ds) return el.__ds;
    const toAttr = k => 'data-' + k.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
    const p = new Proxy({}, {
        get(t, k) { if (typeof k === 'symbol') return undefined; const v = T.getAttr(el, toAttr(k)); return v === null ? undefined : v; },
        set(t, k, v) { setAttribute(el, toAttr(k), str(v)); return true; },
        deleteProperty(t, k) { removeAttribute(el, toAttr(k)); return true; },
        has(t, k) { return T.getAttr(el, toAttr(k)) !== null; },
        ownKeys() { const r = []; const a = T.attrs(el); for (let i = 0; i < a.length; i += 2) if (a[i].startsWith('data-')) r.push(a[i].slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase())); return r; },
        getOwnPropertyDescriptor(t, k) { const v = T.getAttr(el, toAttr(k)); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
    });
    hidden(el, '__ds', p);
    return p;
}

/* attribute changes go through here: observers hear of them */
function setAttribute(el, name, value) {
    name = String(name).toLowerCase();
    const old = observers.length ? T.getAttr(el, name) : null;
    T.setAttr(el, name, str(value));
    if (observers.length) queueMutation({ type: 'attributes', target: el, attributeName: name, attributeNamespace: null, oldValue: old, addedNodes: list([]), removedNodes: list([]) });
    if (customDefs.size && typeof el.attributeChangedCallback === 'function') {
        const obs = el.constructor && el.constructor.observedAttributes;
        if (obs && obs.includes(name)) { try { el.attributeChangedCallback(name, old, str(value)); } catch (e) { reportError(e); } }
    }
}
function removeAttribute(el, name) {
    name = String(name).toLowerCase();
    const old = T.getAttr(el, name);
    if (old === null) return;
    T.removeAttr(el, name);
    if (observers.length) queueMutation({ type: 'attributes', target: el, attributeName: name, attributeNamespace: null, oldValue: old, addedNodes: list([]), removedNodes: list([]) });
}

class Attr {
    constructor(el, name) { this.ownerElement = el; this.name = this.localName = this.nodeName = name; this.namespaceURI = null; this.prefix = null; this.specified = true; }
    get value() { return T.getAttr(this.ownerElement, this.name) || ''; }
    set value(v) { setAttribute(this.ownerElement, this.name, v); }
    get nodeValue() { return this.value; }
    get textContent() { return this.value; }
}
class NamedNodeMap {}

/* ------------------------------------------------------------------ elements */

class DOMRect {
    constructor(x, y, w, h) { this.x = x || 0; this.y = y || 0; this.width = w || 0; this.height = h || 0; }
    get left() { return Math.min(this.x, this.x + this.width); }
    get top() { return Math.min(this.y, this.y + this.height); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
    static fromRect(r) { r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); }
}
class DOMRectReadOnly extends DOMRect {}

function insertAdjacent(el, where, node) {
    switch (String(where).toLowerCase()) {
    case 'beforebegin': if (T.parent(el)) T.parent(el).insertBefore(node, el); break;
    case 'afterbegin': el.insertBefore(node, T.first(el)); break;
    case 'beforeend': el.appendChild(node); break;
    case 'afterend': if (T.parent(el)) T.parent(el).insertBefore(node, T.next(el)); break;
    default: throw new DOMException('bad position', 'SyntaxError');
    }
    return node;
}
function fragmentFrom(html, ctxTag) {
    const tmp = T.create(ctxTag && /^(tr|tbody|thead|tfoot|table|select|colgroup)$/.test(ctxTag) ? 'template' : 'div');
    T.setInner(tmp, str(html));
    const f = T.createFrag();
    while (T.first(tmp)) T.insert(f, T.first(tmp), null);
    return f;
}

class Element extends Node {
    get tagName() { return this.nodeName; }
    get localName() { return T.name(this); }
    get namespaceURI() { return NSURI[T.ns(this) || 1]; }
    get prefix() { return null; }
    get id() { return T.getAttr(this, 'id') || ''; }
    set id(v) { setAttribute(this, 'id', v); }
    get className() { return T.getAttr(this, 'class') || ''; }
    set className(v) { setAttribute(this, 'class', v); }
    get classList() { return this.__cl || (hidden(this, '__cl', new DOMTokenList(this)), this.__cl); }
    set classList(v) { setAttribute(this, 'class', v); }
    get slot() { return T.getAttr(this, 'slot') || ''; }
    get attributes() {
        const a = T.attrs(this), r = [];
        for (let i = 0; i < a.length; i += 2) r.push(new Attr(this, a[i]));
        for (const at of r) if (!(at.name in r)) r[at.name] = at;
        hidden(r, 'getNamedItem', n => r.find(x => x.name === String(n).toLowerCase()) || null);
        hidden(r, 'item', i => r[i] || null);
        return r;
    }
    getAttribute(n) { return T.getAttr(this, String(n)); }
    getAttributeNS(ns, n) { return T.getAttr(this, String(n)); }
    setAttribute(n, v) { setAttribute(this, n, v); }
    setAttributeNS(ns, n, v) { setAttribute(this, String(n).replace(/^\w+:/, ''), v); }
    removeAttribute(n) { removeAttribute(this, n); }
    removeAttributeNS(ns, n) { removeAttribute(this, n); }
    hasAttribute(n) { return T.getAttr(this, String(n)) !== null; }
    hasAttributeNS(ns, n) { return this.hasAttribute(n); }
    hasAttributes() { return T.attrs(this).length > 0; }
    getAttributeNames() { const a = T.attrs(this), r = []; for (let i = 0; i < a.length; i += 2) r.push(a[i]); return r; }
    toggleAttribute(n, force) {
        const has = this.hasAttribute(n);
        const want = force === undefined ? !has : !!force;
        if (want && !has) setAttribute(this, n, ''); else if (!want && has) removeAttribute(this, n);
        return want;
    }
    getAttributeNode(n) { return this.hasAttribute(n) ? new Attr(this, String(n).toLowerCase()) : null; }
    get innerHTML() { return T.inner(this); }
    set innerHTML(v) {
        const old = observers.length ? children(this) : null;
        if (old || customDefs.size) for (const c of children(this)) disconnected(c);
        T.setInner(this, str(v));
        if (old) mutated(this, children(this), old);
        for (const c of children(this)) connected(c);
    }
    get outerHTML() { return T.outer(this); }
    set outerHTML(v) { const p = T.parent(this); if (!p) return; const f = fragmentFrom(v, T.name(p)); p.insertBefore(f, this); p.removeChild(this); }
    get innerText() { return T.text(this); }
    set innerText(v) { this.textContent = v; }
    get outerText() { return this.innerText; }
    insertAdjacentHTML(where, html) { insertAdjacent(this, where, fragmentFrom(html, T.name(this))); }
    insertAdjacentElement(where, el) { return insertAdjacent(this, where, el); }
    insertAdjacentText(where, text) { insertAdjacent(this, where, T.createText(str(text))); }
    matches(s) { return matchesAny(this, String(s), this); }
    webkitMatchesSelector(s) { return this.matches(s); }
    msMatchesSelector(s) { return this.matches(s); }
    closest(s) { for (let e = this; e && T.type(e) === 2; e = T.parent(e)) if (matchesAny(e, String(s), null)) return e; return null; }
    getBoundingClientRect() { const r = T.rect(this); return new DOMRect(r[0], r[1] - T.scrollY(), r[2], r[3]); }
    getClientRects() { const r = this.getBoundingClientRect(); return list(r.width || r.height ? [r] : []); }
    get offsetWidth() { return T.rect(this)[2]; }
    get offsetHeight() { return T.rect(this)[3]; }
    get offsetLeft() { return T.rect(this)[0]; }
    get offsetTop() { return T.rect(this)[1]; }
    get offsetParent() { return T.style(this).display === 'none' ? null : document.body; }
    get clientWidth() { return this === document.documentElement ? T.vw() : T.rect(this)[2]; }
    get clientHeight() { return this === document.documentElement ? T.vh() : T.rect(this)[3]; }
    get clientTop() { return 0; }
    get clientLeft() { return 0; }
    get scrollWidth() { return this.clientWidth; }
    get scrollHeight() { return this === document.documentElement || this === document.body ? Math.max(T.rect(this)[3], T.vh()) : T.rect(this)[3]; }
    get scrollTop() { return this === document.documentElement || this === document.body ? T.scrollY() : 0; }
    set scrollTop(v) { if (this === document.documentElement || this === document.body) T.scrollTo(v | 0); }
    get scrollLeft() { return 0; }
    set scrollLeft(v) {}
    scrollIntoView(opt) { T.scrollTo(Math.max(0, T.rect(this)[1] - (opt && opt.block === 'center' ? (T.vh() / 2 | 0) : 0))); }
    scrollIntoViewIfNeeded() { this.scrollIntoView(); }
    scrollTo(x, y) { if (this === document.documentElement || this === document.body) G.scrollTo(x, y); }
    scrollBy() {}
    scroll(x, y) { this.scrollTo(x, y); }
    attachShadow(init) { hidden(this, '__shadow', this); return this; }
    get shadowRoot() { return this.__shadow || null; }
    animate(keyframes, options) { return elementAnimate(this, keyframes, options); }
    getAnimations() { return [...waapi.values()].filter(a => a.effect && a.effect.target === this && a.playState !== 'idle'); }
    requestFullscreen() { return Promise.reject(new DOMException('not allowed', 'NotAllowedError')); }
    requestPointerLock() {}
    setPointerCapture() {}
    releasePointerCapture() {}
    hasPointerCapture() { return false; }
    computedStyleMap() { return new Map(); }
    get assignedSlot() { return null; }
}
mixin(Element, ParentMixin);
mixin(Element, ChildMixin);

/* ------------------------------------------------------------------ HTML elements */

function reflect(cls, props) {
    for (const [prop, spec] of Object.entries(props)) {
        const [kind, attrName, dflt] = Array.isArray(spec) ? spec : [spec];
        const attr = attrName || prop.toLowerCase();
        let get, set;
        if (kind === 'bool') { get = function () { return T.getAttr(this, attr) !== null; }; set = function (v) { if (v) setAttribute(this, attr, ''); else removeAttribute(this, attr); }; }
        else if (kind === 'int') { get = function () { const v = parseInt(T.getAttr(this, attr), 10); return isNaN(v) ? (dflt || 0) : v; }; set = function (v) { setAttribute(this, attr, String(v | 0)); }; }
        else if (kind === 'url') { get = function () { const v = T.getAttr(this, attr); return v === null ? '' : T.resolve(document.baseURI, v) || v; }; set = function (v) { setAttribute(this, attr, v); }; }
        else { get = function () { const v = T.getAttr(this, attr); return v === null ? (dflt || '') : v; }; set = function (v) { setAttribute(this, attr, v); }; }
        Object.defineProperty(cls.prototype, prop, { get, set, configurable: true, enumerable: true });
    }
}

class HTMLElement extends Element {
    constructor() {
        /* new MyElement() for a custom element class: an element of its name */
        const name = customByClass.get(new.target);
        if (!name) illegal();
        const e = T.create(name, NS_HTML);
        Object.setPrototypeOf(e, new.target.prototype);
        return e;
    }
    get style() { return styleOf(this); }
    set style(v) { setAttribute(this, 'style', v); }
    get dataset() { return datasetOf(this); }
    get hidden() { return T.getAttr(this, 'hidden') !== null; }
    set hidden(v) { if (v) setAttribute(this, 'hidden', ''); else removeAttribute(this, 'hidden'); }
    get isContentEditable() { return false; }
    get contentEditable() { return T.getAttr(this, 'contenteditable') || 'inherit'; }
    focus() { if (T.connected(this)) T.focus(this); }
    blur() { if (T.focused() === this) T.focus(null); }
    click() {
        if (T.getAttr(this, 'disabled') !== null) return;
        const ok = dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true, view: G }));
        if (ok) clickDefault(this);
    }
    get offsetParent() { return super.offsetParent; }
    get innerText() { return T.text(this); }
    set innerText(v) { this.textContent = v; }
    get accessKeyLabel() { return ''; }
    showPopover() {} hidePopover() {} togglePopover() {}
}
reflect(HTMLElement, { title: 'str', lang: 'str', dir: 'str', accessKey: ['str', 'accesskey'], tabIndex: ['int', 'tabindex', -1],
    draggable: 'bool', spellcheck: 'bool', translate: 'bool', inert: 'bool', autofocus: 'bool', nonce: 'str', popover: 'str',
    enterKeyHint: ['str', 'enterkeyhint'], inputMode: ['str', 'inputmode'] });
onProps(HTMLElement.prototype, EVENTS);

/* what a click does when no listener prevented it (element.click() from a script; tFire does the same for real ones) */
function clickDefault(el) {
    for (let e = el; e && T.type(e) === 2; e = T.parent(e)) {
        const name = T.name(e);
        if ((name === 'a' || name === 'area') && T.getAttr(e, 'href') !== null) {
            const href = T.getAttr(e, 'href');
            if (/^\s*javascript:/i.test(href)) { try { (0, eval)(decodeURIComponent(href.replace(/^\s*javascript:/i, ''))); } catch (x) { reportError(x); } }
            else if (href.startsWith('#')) location.hash = href;
            else if (T.getAttr(e, 'download') === null) T.navigate(href, false);
            return;
        }
        if (name === 'input' || name === 'button') {
            const type = (T.getAttr(e, 'type') || (name === 'button' ? 'submit' : 'text')).toLowerCase();
            if (type === 'checkbox') { T.setChecked(e, !T.checked(e)); fire(e, 'input'); fire(e, 'change'); return; }
            if (type === 'radio') { if (!T.checked(e)) { T.setChecked(e, true); fire(e, 'input'); fire(e, 'change'); } return; }
            if (type === 'submit' || type === 'image') { const f = e.form; if (f) f.requestSubmit(e); return; }
            if (type === 'reset') { const f = e.form; if (f) f.reset(); return; }
            return;
        }
        if (name === 'label') {
            const c = e.control;
            if (c && c !== el && !T.contains(c, el)) { c.focus(); c.click(); }
            return;
        }
        if (name === 'summary') { const d = T.parent(e); if (d && T.name(d) === 'details') { d.open = !d.open; dispatch(d, new Event('toggle')); } return; }
    }
}
function fire(el, type) { return dispatch(el, new Event(type, { bubbles: true, cancelable: type !== 'change' && type !== 'input' })); }

class HTMLUnknownElement extends HTMLElement {}
class HTMLDivElement extends HTMLElement {} reflect(HTMLDivElement, { align: 'str' });
class HTMLSpanElement extends HTMLElement {}
class HTMLParagraphElement extends HTMLElement {}
class HTMLHeadingElement extends HTMLElement {}
class HTMLBRElement extends HTMLElement {}
class HTMLHRElement extends HTMLElement {}
class HTMLPreElement extends HTMLElement {}
class HTMLQuoteElement extends HTMLElement {} reflect(HTMLQuoteElement, { cite: 'url' });
class HTMLUListElement extends HTMLElement {}
class HTMLOListElement extends HTMLElement {} reflect(HTMLOListElement, { start: ['int', 'start', 1], reversed: 'bool', type: 'str' });
class HTMLLIElement extends HTMLElement {} reflect(HTMLLIElement, { value: 'int' });
class HTMLDListElement extends HTMLElement {}
class HTMLHtmlElement extends HTMLElement {}
class HTMLHeadElement extends HTMLElement {}
class HTMLBodyElement extends HTMLElement {} onProps(HTMLBodyElement.prototype, ['load', 'unload', 'beforeunload', 'hashchange', 'popstate', 'message', 'resize', 'online', 'offline', 'pageshow', 'pagehide', 'storage']);
class HTMLTitleElement extends HTMLElement { get text() { return T.text(this); } set text(v) { this.textContent = v; } }
class HTMLMetaElement extends HTMLElement {} reflect(HTMLMetaElement, { name: 'str', content: 'str', httpEquiv: ['str', 'http-equiv'], charset: 'str' });
class HTMLBaseElement extends HTMLElement {} reflect(HTMLBaseElement, { href: 'url', target: 'str' });
class HTMLLinkElement extends HTMLElement {
    get relList() { return new DOMTokenList(this, 'rel'); }
    get sheet() { return T.getAttr(this, 'rel') && /stylesheet/i.test(T.getAttr(this, 'rel')) ? new CSSStyleSheet() : null; }
}
reflect(HTMLLinkElement, { href: 'url', rel: 'str', media: 'str', type: 'str', as: 'str', crossOrigin: ['str', 'crossorigin'], integrity: 'str', hreflang: 'str', disabled: 'bool', sizes: 'str', referrerPolicy: ['str', 'referrerpolicy'], fetchPriority: ['str', 'fetchpriority'] });
class HTMLStyleElement extends HTMLElement { get sheet() { return new CSSStyleSheet(); } } reflect(HTMLStyleElement, { media: 'str', type: 'str', disabled: 'bool' });
class HTMLScriptElement extends HTMLElement {
    get text() { return T.text(this); }
    set text(v) { this.textContent = v; }
    static supports(t) { return t === 'classic' || t === 'module'; }
}
reflect(HTMLScriptElement, { src: 'url', type: 'str', async: 'bool', defer: 'bool', noModule: ['bool', 'nomodule'], charset: 'str', crossOrigin: ['str', 'crossorigin'], integrity: 'str', event: 'str', htmlFor: ['str', 'for'], referrerPolicy: ['str', 'referrerpolicy'], fetchPriority: ['str', 'fetchpriority'] });
class HTMLNoScriptElement extends HTMLElement {}
class HTMLTemplateElement extends HTMLElement {
    get content() {
        if (this.__content) return this.__content;
        const f = T.createFrag();
        while (T.first(this)) T.insert(f, T.first(this), null);
        hidden(this, '__content', f);
        return f;
    }
    get innerHTML() { return this.__content ? T.inner(this.__content) : T.inner(this); }
    set innerHTML(v) { const f = this.content; while (T.first(f)) T.remove(T.first(f)); const g = fragmentFrom(v); T.insert(f, g, null); }
}
class HTMLSlotElement extends HTMLElement { assignedNodes() { return []; } assignedElements() { return []; } }

class HTMLAnchorElement extends HTMLElement {
    get relList() { return new DOMTokenList(this, 'rel'); }
    get text() { return T.text(this); }
    set text(v) { this.textContent = v; }
    toString() { return this.href; }
}
reflect(HTMLAnchorElement, { href: 'url', target: 'str', rel: 'str', download: 'str', hreflang: 'str', type: 'str', name: 'str', ping: 'str', referrerPolicy: ['str', 'referrerpolicy'] });
/* the parts of a link's address */
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password']) {
    Object.defineProperty(HTMLAnchorElement.prototype, part, {
        get() { try { return new URL(this.href)[part]; } catch (e) { return ''; } },
        set(v) { try { const u = new URL(this.href); u[part] = v; this.href = u.href; } catch (e) {} },
        configurable: true, enumerable: true,
    });
}
class HTMLAreaElement extends HTMLAnchorElement {} reflect(HTMLAreaElement, { alt: 'str', coords: 'str', shape: 'str' });

class HTMLImageElement extends HTMLElement {
    get complete() { return true; }
    get naturalWidth() { return T.rect(this)[2] || (parseInt(T.getAttr(this, 'width'), 10) || 0); }
    get naturalHeight() { return T.rect(this)[3] || (parseInt(T.getAttr(this, 'height'), 10) || 0); }
    get width() { return T.rect(this)[2] || parseInt(T.getAttr(this, 'width'), 10) || 0; }
    set width(v) { setAttribute(this, 'width', String(v | 0)); }
    get height() { return T.rect(this)[3] || parseInt(T.getAttr(this, 'height'), 10) || 0; }
    set height(v) { setAttribute(this, 'height', String(v | 0)); }
    get currentSrc() { return this.src; }
    get x() { return T.rect(this)[0]; }
    get y() { return T.rect(this)[1]; }
    decode() { return Promise.resolve(); }
}
reflect(HTMLImageElement, { src: 'url', alt: 'str', srcset: 'str', sizes: 'str', useMap: ['str', 'usemap'], isMap: ['bool', 'ismap'], loading: 'str', decoding: 'str', crossOrigin: ['str', 'crossorigin'], referrerPolicy: ['str', 'referrerpolicy'], fetchPriority: ['str', 'fetchpriority'] });
class HTMLPictureElement extends HTMLElement {}
class HTMLSourceElement extends HTMLElement {} reflect(HTMLSourceElement, { src: 'url', srcset: 'str', type: 'str', media: 'str', sizes: 'str' });
class HTMLIFrameElement extends HTMLElement { get contentWindow() { return null; } get contentDocument() { return null; } }
reflect(HTMLIFrameElement, { src: 'url', srcdoc: 'str', name: 'str', width: 'str', height: 'str', allow: 'str', loading: 'str', sandbox: 'str', allowFullscreen: ['bool', 'allowfullscreen'] });
class HTMLEmbedElement extends HTMLElement {} reflect(HTMLEmbedElement, { src: 'url', type: 'str', width: 'str', height: 'str' });
class HTMLObjectElement extends HTMLElement {} reflect(HTMLObjectElement, { data: 'url', type: 'str', width: 'str', height: 'str', name: 'str' });

class HTMLMediaElement extends HTMLElement {
    play() { return Promise.reject(new DOMException('tFire does not play media yet', 'NotSupportedError')); }
    pause() {}
    load() {}
    canPlayType() { return ''; }
    get paused() { return true; }
    get ended() { return false; }
    get duration() { return NaN; }
    get readyState() { return 0; }
    get networkState() { return 3; }
    get buffered() { return { length: 0, start() { return 0; }, end() { return 0; } }; }
    get currentTime() { return 0; }
    set currentTime(v) {}
    get volume() { return 1; }
    set volume(v) {}
    get muted() { return T.getAttr(this, 'muted') !== null; }
    set muted(v) {}
    get playbackRate() { return 1; }
    set playbackRate(v) {}
    get textTracks() { return []; }
    addTextTrack() { return { addCue() {}, mode: 'hidden' }; }
}
reflect(HTMLMediaElement, { src: 'url', autoplay: 'bool', controls: 'bool', loop: 'bool', preload: 'str', crossOrigin: ['str', 'crossorigin'] });
class HTMLVideoElement extends HTMLMediaElement { get videoWidth() { return 0; } get videoHeight() { return 0; } } reflect(HTMLVideoElement, { poster: 'url', width: 'int', height: 'int', playsInline: ['bool', 'playsinline'] });
class HTMLAudioElement extends HTMLMediaElement {}
class HTMLTrackElement extends HTMLElement {}

/* a canvas: no pixels yet, but scripts that draw on one keep running */
function context2d(canvas) {
    const noop = () => {};
    const grad = { addColorStop: noop };
    const ctx = {
        canvas, fillStyle: '#000', strokeStyle: '#000', lineWidth: 1, font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic',
        globalAlpha: 1, globalCompositeOperation: 'source-over', lineCap: 'butt', lineJoin: 'miter', shadowBlur: 0, shadowColor: 'transparent',
        imageSmoothingEnabled: true, direction: 'ltr', filter: 'none',
        measureText: t => ({ width: str(t).length * (parseInt(ctx.font, 10) || 10) * 0.55, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 }),
        getImageData: (x, y, w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(Math.max(0, w * h * 4)) }),
        createImageData: (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(Math.max(0, w * h * 4)) }),
        createLinearGradient: () => grad, createRadialGradient: () => grad, createConicGradient: () => grad, createPattern: () => ({}),
        getTransform: () => ({ a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 }), isPointInPath: () => false, isPointInStroke: () => false,
        getLineDash: () => [], getContextAttributes: () => ({ alpha: true }),
    };
    for (const m of ['fillRect', 'strokeRect', 'clearRect', 'fillText', 'strokeText', 'beginPath', 'closePath', 'moveTo', 'lineTo', 'arc', 'arcTo',
        'bezierCurveTo', 'quadraticCurveTo', 'rect', 'roundRect', 'ellipse', 'fill', 'stroke', 'clip', 'save', 'restore', 'translate', 'rotate', 'scale',
        'transform', 'setTransform', 'resetTransform', 'drawImage', 'putImageData', 'setLineDash', 'drawFocusIfNeeded', 'reset'])
        ctx[m] = noop;
    return ctx;
}
class HTMLCanvasElement extends HTMLElement {
    getContext(kind) { if (kind !== '2d') return null; return this.__ctx || (hidden(this, '__ctx', context2d(this)), this.__ctx); }
    toDataURL() { return 'data:,'; }
    toBlob(cb) { setTimeout(() => cb(new Blob([])), 0); }
    captureStream() { return null; }
    transferControlToOffscreen() { illegal(); }
}
reflect(HTMLCanvasElement, { width: ['int', 'width', 300], height: ['int', 'height', 150] });

/* ---- forms */

function formOf(el) {
    const id = T.getAttr(el, 'form');
    if (id) { const f = T.byId(id); if (f && T.name(f) === 'form') return f; }
    for (let p = T.parent(el); p; p = T.parent(p)) if (T.type(p) === 2 && T.name(p) === 'form') return p;
    return null;
}
const fieldMixin = {
    get form() { return formOf(this); },
    get labels() { const id = this.id; return list([...(id ? T.queryAll(document, 'label[for=' + cssEscape(id) + ']') : []), ...(this.closest('label') ? [this.closest('label')] : [])]); },
    get validity() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; },
    get validationMessage() { return ''; },
    get willValidate() { return true; },
    checkValidity() { return true; },
    reportValidity() { return true; },
    setCustomValidity() {},
};
class HTMLFormElement extends HTMLElement {
    get elements() {
        const els = T.queryAll(this, 'input,select,textarea,button,fieldset,output,object');
        const id = this.id;
        if (id) for (const e of T.queryAll(document, '[form=' + cssEscape(id) + ']')) if (!els.includes(e)) els.push(e);
        for (const e of els) { const n = T.getAttr(e, 'name') || T.getAttr(e, 'id'); if (n && !(n in els)) els[n] = e; }
        return list(els);
    }
    get length() { return this.elements.length; }
    submit() { T.submit(this, null); }
    requestSubmit(submitter) {
        const ev = new SubmitEvent('submit', { bubbles: true, cancelable: true, submitter: submitter || null });
        if (dispatch(this, ev)) T.submit(this, submitter || null);
    }
    reset() {
        if (!dispatch(this, new Event('reset', { bubbles: true, cancelable: true }))) return;
        for (const e of this.elements) if ('defaultValue' in e && T.name(e) !== 'button') { if (/^(checkbox|radio)$/.test(e.type)) e.checked = e.defaultChecked; else e.value = e.defaultValue; }
    }
    checkValidity() { return true; }
    reportValidity() { return true; }
}
reflect(HTMLFormElement, { action: 'url', method: ['str', 'method', 'get'], target: 'str', enctype: ['str', 'enctype', 'application/x-www-form-urlencoded'],
    encoding: ['str', 'enctype', 'application/x-www-form-urlencoded'], name: 'str', acceptCharset: ['str', 'accept-charset'], noValidate: ['bool', 'novalidate'], autocomplete: 'str' });
/* form.q — a field by its name */
const formProxyKeys = /^[a-zA-Z_$][\w$-]*$/;

class HTMLInputElement extends HTMLElement {
    get value() { return T.value(this); }
    set value(v) { T.setValue(this, str(v)); }
    get defaultValue() { return T.getAttr(this, 'value') || ''; }
    set defaultValue(v) { setAttribute(this, 'value', v); }
    get checked() { return T.checked(this); }
    set checked(v) { T.setChecked(this, !!v); }
    get defaultChecked() { return T.getAttr(this, 'checked') !== null; }
    set defaultChecked(v) { if (v) setAttribute(this, 'checked', ''); else removeAttribute(this, 'checked'); }
    get type() { const t = (T.getAttr(this, 'type') || 'text').toLowerCase(); return /^(hidden|text|search|tel|url|email|password|date|month|week|time|datetime-local|number|range|color|checkbox|radio|file|submit|image|reset|button)$/.test(t) ? t : 'text'; }
    set type(v) { setAttribute(this, 'type', v); }
    get valueAsNumber() { const n = parseFloat(this.value); return isNaN(n) ? NaN : n; }
    set valueAsNumber(v) { this.value = String(v); }
    get valueAsDate() { const d = new Date(this.value); return isNaN(d) ? null : d; }
    set valueAsDate(d) { this.value = d ? d.toISOString().slice(0, 10) : ''; }
    get files() { return list([]); }
    get selectionStart() { return this.value.length; }
    set selectionStart(v) {}
    get selectionEnd() { return this.value.length; }
    set selectionEnd(v) {}
    get selectionDirection() { return 'none'; }
    get indeterminate() { return !!this.__ind; }
    set indeterminate(v) { hidden(this, '__ind', !!v); }
    get list() { const id = T.getAttr(this, 'list'); return id ? T.byId(id) : null; }
    select() { this.focus(); }
    setSelectionRange() {}
    setRangeText(t) { this.value = t; }
    stepUp(n) { this.value = String((parseFloat(this.value) || 0) + (n || 1) * (parseFloat(this.step) || 1)); }
    stepDown(n) { this.stepUp(-(n || 1)); }
    showPicker() {}
}
mixin(HTMLInputElement, fieldMixin);
reflect(HTMLInputElement, { name: 'str', placeholder: 'str', disabled: 'bool', readOnly: ['bool', 'readonly'], required: 'bool', multiple: 'bool',
    autocomplete: 'str', min: 'str', max: 'str', step: 'str', pattern: 'str', size: ['int', 'size', 20], maxLength: ['int', 'maxlength', -1],
    minLength: ['int', 'minlength', -1], accept: 'str', alt: 'str', src: 'url', formAction: ['url', 'formaction'], formMethod: ['str', 'formmethod'],
    formNoValidate: ['bool', 'formnovalidate'], formTarget: ['str', 'formtarget'], dirName: ['str', 'dirname'], capture: 'str', width: 'int', height: 'int' });
class HTMLButtonElement extends HTMLElement {
    get type() { const t = (T.getAttr(this, 'type') || 'submit').toLowerCase(); return t === 'button' || t === 'reset' ? t : 'submit'; }
    set type(v) { setAttribute(this, 'type', v); }
    get value() { return T.getAttr(this, 'value') || ''; }
    set value(v) { setAttribute(this, 'value', v); }
}
mixin(HTMLButtonElement, fieldMixin);
reflect(HTMLButtonElement, { name: 'str', disabled: 'bool', formAction: ['url', 'formaction'], formMethod: ['str', 'formmethod'], formNoValidate: ['bool', 'formnovalidate'], formTarget: ['str', 'formtarget'], popoverTargetElement: 'str' });
class HTMLTextAreaElement extends HTMLElement {
    get value() { return T.value(this); }
    set value(v) { T.setValue(this, str(v)); }
    get defaultValue() { return T.text(this); }
    set defaultValue(v) { this.textContent = v; }
    get type() { return 'textarea'; }
    get textLength() { return this.value.length; }
    get selectionStart() { return this.value.length; }
    set selectionStart(v) {}
    get selectionEnd() { return this.value.length; }
    set selectionEnd(v) {}
    select() { this.focus(); }
    setSelectionRange() {}
    setRangeText(t) { this.value = t; }
}
mixin(HTMLTextAreaElement, fieldMixin);
reflect(HTMLTextAreaElement, { name: 'str', placeholder: 'str', disabled: 'bool', readOnly: ['bool', 'readonly'], required: 'bool', rows: ['int', 'rows', 2], cols: ['int', 'cols', 20], maxLength: ['int', 'maxlength', -1], wrap: 'str', autocomplete: 'str' });
class HTMLSelectElement extends HTMLElement {
    get options() {
        const o = T.queryAll(this, 'option');
        hidden(o, 'add', (opt, before) => this.add(opt, before));
        hidden(o, 'remove', i => this.remove(i));
        Object.defineProperty(o, 'selectedIndex', { get: () => this.selectedIndex, set: v => { this.selectedIndex = v; } });
        return list(o);
    }
    get length() { return T.queryAll(this, 'option').length; }
    set length(n) { const o = T.queryAll(this, 'option'); while (o.length > n) T.remove(o.pop()); }
    get selectedOptions() { return list(T.queryAll(this, 'option').filter(o => o.selected)); }
    get selectedIndex() {
        const o = T.queryAll(this, 'option');
        const v = T.value(this);
        for (let i = 0; i < o.length; i++) if (o[i].selected) return i;
        return o.length ? o.findIndex(x => x.value === v) : -1;
    }
    set selectedIndex(i) { const o = T.queryAll(this, 'option'); o.forEach((x, k) => { x.selected = k === i; }); }
    get value() { return T.value(this); }
    set value(v) { T.setValue(this, str(v)); }
    get type() { return T.getAttr(this, 'multiple') !== null ? 'select-multiple' : 'select-one'; }
    item(i) { return T.queryAll(this, 'option')[i] || null; }
    namedItem(n) { return T.queryAll(this, 'option').find(o => o.id === n || T.getAttr(o, 'name') === n) || null; }
    add(opt, before) { const ref = typeof before === 'number' ? this.item(before) : before || null; (ref ? T.parent(ref) : this).insertBefore(opt, ref); }
    remove(i) { if (i === undefined) { ChildMixin.remove.call(this); return; } const o = this.item(i); if (o) T.parent(o).removeChild(o); }
    showPicker() {}
}
mixin(HTMLSelectElement, fieldMixin);
reflect(HTMLSelectElement, { name: 'str', disabled: 'bool', multiple: 'bool', required: 'bool', size: 'int', autocomplete: 'str' });
class HTMLOptionElement extends HTMLElement {
    constructor(text, value, defSel, sel) {
        if (customByClass.get(new.target)) return super();
        const o = T.create('option');
        if (text !== undefined) T.setText(o, str(text));
        if (value !== undefined) T.setAttr(o, 'value', str(value));
        if (defSel) T.setAttr(o, 'selected', '');
        if (sel) o.selected = true;
        return o;
    }
    get value() { const v = T.getAttr(this, 'value'); return v === null ? T.text(this).trim().replace(/\s+/g, ' ') : v; }
    set value(v) { setAttribute(this, 'value', v); }
    get text() { return T.text(this).trim().replace(/\s+/g, ' '); }
    set text(v) { this.textContent = v; }
    get label() { return T.getAttr(this, 'label') || this.text; }
    get selected() { const s = this.closest('select'); if (s && this.__sel !== undefined) return this.__sel; return T.getAttr(this, 'selected') !== null; }
    set selected(v) {
        const s = this.closest('select');
        if (v && s && T.getAttr(s, 'multiple') === null) for (const o of T.queryAll(s, 'option')) if (o !== this) { hidden(o, '__sel', false); T.removeAttr(o, 'selected'); }
        hidden(this, '__sel', !!v);
        if (v) T.setAttr(this, 'selected', ''); else T.removeAttr(this, 'selected');
        T.changed();
    }
    get defaultSelected() { return T.getAttr(this, 'selected') !== null; }
    set defaultSelected(v) { if (v) setAttribute(this, 'selected', ''); else removeAttribute(this, 'selected'); }
    get index() { const s = this.closest('select'); return s ? T.queryAll(s, 'option').indexOf(this) : 0; }
    get form() { const s = this.closest('select'); return s ? s.form : null; }
}
reflect(HTMLOptionElement, { disabled: 'bool' });
class HTMLOptGroupElement extends HTMLElement {} reflect(HTMLOptGroupElement, { label: 'str', disabled: 'bool' });
class HTMLLabelElement extends HTMLElement {
    get control() { const f = T.getAttr(this, 'for'); if (f) return T.byId(f); return T.query(this, 'input,select,textarea,button'); }
    get form() { const c = this.control; return c ? c.form : null; }
}
reflect(HTMLLabelElement, { htmlFor: ['str', 'for'] });
class HTMLFieldSetElement extends HTMLElement { get elements() { return list(T.queryAll(this, 'input,select,textarea,button')); } get form() { return formOf(this); } }
reflect(HTMLFieldSetElement, { disabled: 'bool', name: 'str' });
class HTMLLegendElement extends HTMLElement {}
class HTMLOutputElement extends HTMLElement { get value() { return T.text(this); } set value(v) { this.textContent = v; } }
class HTMLDataListElement extends HTMLElement { get options() { return list(T.queryAll(this, 'option')); } }
class HTMLProgressElement extends HTMLElement { get position() { return this.max ? this.value / this.max : -1; } }
reflect(HTMLProgressElement, { value: 'int', max: ['int', 'max', 1] });
class HTMLMeterElement extends HTMLElement {}

/* ---- tables */
class HTMLTableElement extends HTMLElement {
    get rows() { return list(T.queryAll(this, 'tr').filter(r => r.closest('table') === this)); }
    get tBodies() { return list(elemChildren(this).filter(e => T.name(e) === 'tbody')); }
    get tHead() { return elemChildren(this).find(e => T.name(e) === 'thead') || null; }
    get tFoot() { return elemChildren(this).find(e => T.name(e) === 'tfoot') || null; }
    get caption() { return elemChildren(this).find(e => T.name(e) === 'caption') || null; }
    createTBody() { const b = T.create('tbody'); this.appendChild(b); return b; }
    createTHead() { let h = this.tHead; if (!h) { h = T.create('thead'); this.insertBefore(h, T.first(this)); } return h; }
    createTFoot() { let f = this.tFoot; if (!f) { f = T.create('tfoot'); this.appendChild(f); } return f; }
    createCaption() { let c = this.caption; if (!c) { c = T.create('caption'); this.insertBefore(c, T.first(this)); } return c; }
    deleteTHead() { const h = this.tHead; if (h) h.remove(); }
    deleteTFoot() { const f = this.tFoot; if (f) f.remove(); }
    deleteCaption() { const c = this.caption; if (c) c.remove(); }
    insertRow(i) {
        const r = T.create('tr');
        const rows = this.rows;
        if (i === undefined || i < 0 || i >= rows.length) { const b = this.tBodies[this.tBodies.length - 1] || this; b.appendChild(r); }
        else T.parent(rows[i]).insertBefore(r, rows[i]);
        return r;
    }
    deleteRow(i) { const r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); }
}
reflect(HTMLTableElement, { border: 'str', width: 'str', cellPadding: ['str', 'cellpadding'], cellSpacing: ['str', 'cellspacing'], summary: 'str' });
class HTMLTableSectionElement extends HTMLElement {
    get rows() { return list(elemChildren(this).filter(e => T.name(e) === 'tr')); }
    insertRow(i) { const r = T.create('tr'); const rows = this.rows; this.insertBefore(r, i === undefined || i < 0 ? null : rows[i] || null); return r; }
    deleteRow(i) { const r = this.rows[i]; if (r) r.remove(); }
}
class HTMLTableRowElement extends HTMLElement {
    get cells() { return list(elemChildren(this).filter(e => T.name(e) === 'td' || T.name(e) === 'th')); }
    get rowIndex() { const t = this.closest('table'); return t ? t.rows.indexOf(this) : -1; }
    get sectionRowIndex() { const p = T.parent(this); return p ? elemChildren(p).indexOf(this) : -1; }
    insertCell(i) { const c = T.create('td'); const cells = this.cells; this.insertBefore(c, i === undefined || i < 0 ? null : cells[i] || null); return c; }
    deleteCell(i) { const c = this.cells[i]; if (c) c.remove(); }
}
class HTMLTableCellElement extends HTMLElement { get cellIndex() { const p = T.parent(this); return p ? elemChildren(p).indexOf(this) : -1; } }
reflect(HTMLTableCellElement, { colSpan: ['int', 'colspan', 1], rowSpan: ['int', 'rowspan', 1], headers: 'str', scope: 'str', abbr: 'str' });
class HTMLTableCaptionElement extends HTMLElement {}
class HTMLTableColElement extends HTMLElement {} reflect(HTMLTableColElement, { span: ['int', 'span', 1] });

class HTMLDetailsElement extends HTMLElement {} reflect(HTMLDetailsElement, { open: 'bool', name: 'str' });
class HTMLDialogElement extends HTMLElement {
    get open() { return T.getAttr(this, 'open') !== null; }
    set open(v) { if (v) setAttribute(this, 'open', ''); else removeAttribute(this, 'open'); }
    show() { this.open = true; }
    showModal() { this.open = true; }
    close(rv) { if (rv !== undefined) this.returnValue = rv; this.open = false; dispatch(this, new Event('close')); }
    requestClose(rv) { this.close(rv); }
}
class HTMLMenuElement extends HTMLElement {}
class HTMLTimeElement extends HTMLElement {} reflect(HTMLTimeElement, { dateTime: ['str', 'datetime'] });
class HTMLDataElement extends HTMLElement {} reflect(HTMLDataElement, { value: 'str' });
class HTMLModElement extends HTMLElement {} reflect(HTMLModElement, { cite: 'url', dateTime: ['str', 'datetime'] });
class HTMLMapElement extends HTMLElement { get areas() { return list(T.queryAll(this, 'area')); } } reflect(HTMLMapElement, { name: 'str' });
class HTMLParamElement extends HTMLElement {}
class HTMLFrameSetElement extends HTMLElement {}
class HTMLFrameElement extends HTMLElement {}

class SVGElement extends Element {
    get style() { return styleOf(this); }
    get dataset() { return datasetOf(this); }
    get ownerSVGElement() { return this.closest('svg'); }
    get viewportElement() { return this.closest('svg'); }
    get className() { const c = T.getAttr(this, 'class') || ''; return { baseVal: c, animVal: c }; }
    set className(v) { setAttribute(this, 'class', v); }
    focus() {} blur() {}
    getBBox() { const r = T.rect(this); return new DOMRect(0, 0, r[2], r[3]); }
    getCTM() { return null; }
    getScreenCTM() { return null; }
    getTotalLength() { return 0; }
    getPointAtLength() { return { x: 0, y: 0 }; }
}
onProps(SVGElement.prototype, EVENTS);
class SVGGraphicsElement extends SVGElement {}
class SVGSVGElement extends SVGGraphicsElement { createSVGPoint() { return { x: 0, y: 0, matrixTransform() { return this; } }; } createSVGMatrix() { return { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 }; } }
class MathMLElement extends Element { get style() { return styleOf(this); } }
onProps(MathMLElement.prototype, EVENTS);

const TAGS = {
    div: HTMLDivElement, span: HTMLSpanElement, p: HTMLParagraphElement, h1: HTMLHeadingElement, h2: HTMLHeadingElement, h3: HTMLHeadingElement,
    h4: HTMLHeadingElement, h5: HTMLHeadingElement, h6: HTMLHeadingElement, br: HTMLBRElement, hr: HTMLHRElement, pre: HTMLPreElement,
    blockquote: HTMLQuoteElement, q: HTMLQuoteElement, ul: HTMLUListElement, ol: HTMLOListElement, li: HTMLLIElement, dl: HTMLDListElement,
    html: HTMLHtmlElement, head: HTMLHeadElement, body: HTMLBodyElement, title: HTMLTitleElement, meta: HTMLMetaElement, base: HTMLBaseElement,
    link: HTMLLinkElement, style: HTMLStyleElement, script: HTMLScriptElement, noscript: HTMLNoScriptElement, template: HTMLTemplateElement,
    slot: HTMLSlotElement, a: HTMLAnchorElement, area: HTMLAreaElement, img: HTMLImageElement, picture: HTMLPictureElement, source: HTMLSourceElement,
    iframe: HTMLIFrameElement, embed: HTMLEmbedElement, object: HTMLObjectElement, video: HTMLVideoElement, audio: HTMLAudioElement,
    track: HTMLTrackElement, canvas: HTMLCanvasElement, form: HTMLFormElement, input: HTMLInputElement, button: HTMLButtonElement,
    textarea: HTMLTextAreaElement, select: HTMLSelectElement, option: HTMLOptionElement, optgroup: HTMLOptGroupElement, label: HTMLLabelElement,
    fieldset: HTMLFieldSetElement, legend: HTMLLegendElement, output: HTMLOutputElement, datalist: HTMLDataListElement, progress: HTMLProgressElement,
    meter: HTMLMeterElement, table: HTMLTableElement, thead: HTMLTableSectionElement, tbody: HTMLTableSectionElement, tfoot: HTMLTableSectionElement,
    tr: HTMLTableRowElement, td: HTMLTableCellElement, th: HTMLTableCellElement, caption: HTMLTableCaptionElement, col: HTMLTableColElement,
    colgroup: HTMLTableColElement, details: HTMLDetailsElement, dialog: HTMLDialogElement, menu: HTMLMenuElement, time: HTMLTimeElement,
    data: HTMLDataElement, ins: HTMLModElement, del: HTMLModElement, map: HTMLMapElement, param: HTMLParamElement, frameset: HTMLFrameSetElement,
    frame: HTMLFrameElement,
};
const KNOWN_PLAIN = /^(abbr|address|article|aside|b|bdi|bdo|big|center|cite|code|dd|dfn|dt|em|figcaption|figure|footer|header|hgroup|i|kbd|main|mark|nav|nobr|rp|rt|ruby|s|samp|search|section|small|strike|strong|sub|summary|sup|tt|u|var|wbr|acronym|basefont|font|marquee|noembed|noframes|plaintext|xmp|listing)$/;

/* ------------------------------------------------------------------ custom elements */

const customDefs = new Map(), customByClass = new Map(), customWaiting = new Map();
function upgrade(e) {
    if (!customDefs.size || T.type(e) !== 2) return;
    const cls = customDefs.get(T.name(e));
    if (!cls || e instanceof cls) return;
    Object.setPrototypeOf(e, cls.prototype);
    const obs = cls.observedAttributes;
    if (obs && typeof e.attributeChangedCallback === 'function')
        for (const a of obs) { const v = T.getAttr(e, a); if (v !== null) { try { e.attributeChangedCallback(a, null, v); } catch (x) { reportError(x); } } }
}
const customElements = {
    define(name, cls) {
        name = String(name).toLowerCase();
        if (customDefs.has(name)) throw new DOMException("'" + name + "' has already been defined as a custom element", 'NotSupportedError');
        customDefs.set(name, cls);
        customByClass.set(cls, name);
        for (const e of T.queryAll(document, name.replace(/([^\w-])/g, '\\$1'))) {
            upgrade(e);
            if (typeof e.connectedCallback === 'function') { try { e.connectedCallback(); } catch (x) { reportError(x); } }
        }
        const w = customWaiting.get(name);
        if (w) { customWaiting.delete(name); w.forEach(r => r(cls)); }
    },
    get(name) { return customDefs.get(String(name).toLowerCase()); },
    getName(cls) { return customByClass.get(cls) || null; },
    whenDefined(name) {
        name = String(name).toLowerCase();
        if (customDefs.has(name)) return Promise.resolve(customDefs.get(name));
        return new Promise(r => { const a = customWaiting.get(name) || []; a.push(r); customWaiting.set(name, a); });
    },
    upgrade(root) { if (root) { upgrade(root); for (const e of T.queryAll(root, '*')) upgrade(e); } },
};

/* which prototype a node gets when a script first sees it (asked by js.c) */
function protoFor(type, tag, ns) {
    switch (type) {
    case 1: return Document.prototype;
    case 3: return Text.prototype;
    case 4: return Comment.prototype;
    case 5: return DocumentType.prototype;
    case 6: return DocumentFragment.prototype;
    }
    if (ns === NS_SVG) return tag === 'svg' ? SVGSVGElement.prototype : SVGGraphicsElement.prototype;
    if (ns === NS_MATHML) return MathMLElement.prototype;
    const cls = TAGS[tag];
    if (cls) return cls.prototype;
    const custom = customDefs.get(tag);
    if (custom) return custom.prototype;
    return tag.includes('-') || KNOWN_PLAIN.test(tag) ? HTMLElement.prototype : HTMLUnknownElement.prototype;
}

/* ------------------------------------------------------------------ URL */

const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.-]*):(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
const DEFAULT_PORT = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21' };
class URLSearchParams {
    constructor(init) {
        hidden(this, '_l', []);
        hidden(this, '_u', null);
        if (init === undefined || init === null) return;
        if (typeof init === 'object') {
            const it = init[Symbol.iterator] ? init : Object.entries(init);
            for (const [k, v] of it) this._l.push([String(k), String(v)]);
        } else {
            let s = String(init);
            if (s[0] === '?') s = s.slice(1);
            for (const part of s.split('&')) {
                if (!part) continue;
                const i = part.indexOf('=');
                const dec = x => { try { return decodeURIComponent(x.replace(/\+/g, ' ')); } catch (e) { return x; } };
                this._l.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
            }
        }
    }
    _upd() { if (this._u) { const s = this.toString(); this._u._search = s ? '?' + s : ''; } }
    append(k, v) { this._l.push([String(k), String(v)]); this._upd(); }
    delete(k, v) { this._l = this._l.filter(e => !(e[0] === String(k) && (v === undefined || e[1] === String(v)))); this._upd(); }
    get(k) { const e = this._l.find(e => e[0] === String(k)); return e ? e[1] : null; }
    getAll(k) { return this._l.filter(e => e[0] === String(k)).map(e => e[1]); }
    has(k, v) { return this._l.some(e => e[0] === String(k) && (v === undefined || e[1] === String(v))); }
    set(k, v) {
        k = String(k);
        const i = this._l.findIndex(e => e[0] === k);
        if (i < 0) this._l.push([k, String(v)]);
        else { this._l[i][1] = String(v); this._l = this._l.filter((e, j) => j <= i || e[0] !== k); }
        this._upd();
    }
    sort() { this._l.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._upd(); }
    forEach(fn, self) { for (const [k, v] of this._l) fn.call(self, v, k, this); }
    keys() { return this._l.map(e => e[0])[Symbol.iterator](); }
    values() { return this._l.map(e => e[1])[Symbol.iterator](); }
    entries() { return this._l.map(e => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this._l.length; }
    toString() {
        const enc = x => encodeURIComponent(x).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
        return this._l.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
    }
}

class URL {
    constructor(url, base) {
        url = String(url).trim();
        let abs = url;
        if (base !== undefined && base !== null) {
            const b = base instanceof URL ? base.href : String(base);
            if (!URL_RE.test(b)) throw new TypeError("Failed to construct 'URL': Invalid base URL");
            abs = T.resolve(b, url);
            if (abs === null) {
                if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(url)) abs = url;
                else throw new TypeError("Failed to construct 'URL': Invalid URL");
            }
        }
        const m = URL_RE.exec(abs);
        if (!m) throw new TypeError("Failed to construct 'URL': Invalid URL '" + url + "'");
        this._protocol = m[1].toLowerCase() + ':';
        this._username = m[2] || '';
        this._password = m[3] || '';
        this._hostname = (m[4] || '').toLowerCase();
        this._port = m[5] && m[5] !== DEFAULT_PORT[this._protocol] ? m[5] : '';
        this._special = abs.slice(m[1].length + 1, m[1].length + 3) === '//';
        let path = m[6] || '';
        if (this._special && !path) path = '/';
        this._pathname = path;
        this._search = m[7] && m[7] !== '?' ? m[7] : '';
        this._hash = m[8] && m[8] !== '#' ? m[8] : '';
        hidden(this, '_sp', null);
    }
    get href() {
        const auth = this._username ? this._username + (this._password ? ':' + this._password : '') + '@' : '';
        return this._protocol + (this._special ? '//' + auth + this.host : '') + this._pathname + this._search + this._hash;
    }
    set href(v) { const u = new URL(v); Object.assign(this, { _protocol: u._protocol, _username: u._username, _password: u._password, _hostname: u._hostname, _port: u._port, _special: u._special, _pathname: u._pathname, _search: u._search, _hash: u._hash }); if (this._sp) this._sp._l = new URLSearchParams(this._search)._l; }
    get protocol() { return this._protocol; }
    set protocol(v) { v = String(v).replace(/:.*$/, '').toLowerCase(); if (v) this._protocol = v + ':'; }
    get username() { return this._username; }
    set username(v) { this._username = String(v); }
    get password() { return this._password; }
    set password(v) { this._password = String(v); }
    get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
    set host(v) { const [h, p] = String(v).split(':'); this._hostname = h.toLowerCase(); this._port = p && p !== DEFAULT_PORT[this._protocol] ? p : ''; }
    get hostname() { return this._hostname; }
    set hostname(v) { this._hostname = String(v).toLowerCase(); }
    get port() { return this._port; }
    set port(v) { v = String(v); this._port = v === DEFAULT_PORT[this._protocol] ? '' : v; }
    get pathname() { return this._pathname; }
    set pathname(v) { v = String(v); this._pathname = this._special && v[0] !== '/' ? '/' + v : v; }
    get search() { return this._search; }
    set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; if (this._sp) this._sp._l = new URLSearchParams(this._search)._l; }
    get hash() { return this._hash; }
    set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get origin() { return this._special ? this._protocol + '//' + this.host : 'null'; }
    get searchParams() {
        if (!this._sp) { const sp = new URLSearchParams(this._search); sp._u = this; hidden(this, '_sp', sp); }
        return this._sp;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
    static parse(u, b) { try { return new URL(u, b); } catch (e) { return null; } }
    static createObjectURL(blob) { const id = 'blob:' + location.origin + '/' + crypto.randomUUID(); blobs.set(id, blob); return id; }
    static revokeObjectURL(id) { blobs.delete(id); }
}
for (const k of ['_protocol', '_username', '_password', '_hostname', '_port', '_special', '_pathname', '_search', '_hash'])
    Object.defineProperty(URL.prototype, k, { writable: true, enumerable: false, configurable: true, value: '' });
const blobs = new Map();

/* ------------------------------------------------------------------ location and history */

let histState = null, histLength = 1;
const location = {
    get href() { return T.url(); },
    set href(v) { this.assign(v); },
    assign(v) {
        v = str(v);
        const target = T.resolve(T.url(), v);
        if (target && target.split('#')[0] === T.url().split('#')[0] && target.includes('#')) { this.hash = target.slice(target.indexOf('#')); return; }
        if (/^\s*javascript:/i.test(v)) { try { (0, eval)(decodeURIComponent(v.replace(/^\s*javascript:/i, ''))); } catch (e) { reportError(e); } return; }
        T.navigate(v, false);
    },
    replace(v) { T.navigate(str(v), true); },
    reload() { T.reload(); },
    toString() { return T.url(); },
    get ancestorOrigins() { return list([]); },
};
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
    Object.defineProperty(location, part, {
        get() { try { return new URL(T.url())[part]; } catch (e) { return ''; } },
        set(v) {
            const u = new URL(T.url());
            const oldUrl = u.href;
            u[part] = v;
            if (part === 'hash') {
                T.setUrl(u.href, false);
                histLength++;
                const id = decodeURIComponent(u.hash.slice(1));
                const t = id ? (T.byId(id) || T.query(document, 'a[name="' + id.replace(/"/g, '') + '"]')) : null;
                if (t) t.scrollIntoView(); else if (!id) T.scrollTo(0);
                setTimeout(() => dispatch(G, new HashChangeEvent('hashchange', { oldURL: oldUrl, newURL: u.href })), 0);
            } else T.navigate(u.href, false);
        },
        configurable: true, enumerable: true,
    });
}
const history = {
    get length() { return histLength; },
    get state() { return histState; },
    scrollRestoration: 'auto',
    pushState(state, title, url) { histState = state === undefined ? null : state; histLength++; if (url !== undefined && url !== null) T.setUrl(str(url), false); },
    replaceState(state, title, url) { histState = state === undefined ? null : state; if (url !== undefined && url !== null) T.setUrl(str(url), true); },
    back() { T.navigate('tfire:back', false); },
    forward() { T.navigate('tfire:forward', false); },
    go(n) { if (!n) T.reload(); else T.navigate(n < 0 ? 'tfire:back' : 'tfire:forward', false); },
};

/* ------------------------------------------------------------------ document */

let readyState = 'loading', currentScript = null;
class Document extends Node {
    constructor() { super(); }
    get documentElement() { for (let c = T.first(this); c; c = T.next(c)) if (T.type(c) === 2) return c; return null; }
    get head() { return T.query(this, 'head'); }
    get body() { const h = this.documentElement; if (!h) return null; for (let c = T.first(h); c; c = T.next(c)) if (T.type(c) === 2 && (T.name(c) === 'body' || T.name(c) === 'frameset')) return c; return null; }
    set body(b) { const old = this.body; if (old) old.replaceWith(b); else if (this.documentElement) this.documentElement.appendChild(b); }
    get doctype() { for (let c = T.first(this); c; c = T.next(c)) if (T.type(c) === 5) return c; return null; }
    get title() { const t = T.query(this, 'title'); return t ? T.text(t).trim().replace(/\s+/g, ' ') : ''; }
    set title(v) { let t = T.query(this, 'title'); if (!t) { t = T.create('title'); const h = this.head; if (h) h.appendChild(t); else return; } t.textContent = v; }
    get URL() { return T.url(); }
    get documentURI() { return T.url(); }
    get baseURI() { const b = T.query(this, 'base[href]'); return b ? T.resolve(T.url(), T.getAttr(b, 'href')) || T.url() : T.url(); }
    get location() { return location; }
    set location(v) { location.href = v; }
    get domain() { return location.hostname; }
    set domain(v) {}
    get referrer() { return ''; }
    get cookie() { return T.cookie(); }
    set cookie(v) { T.setCookie(str(v)); }
    get readyState() { return readyState; }
    get defaultView() { return G; }
    get activeElement() { return T.focused() || this.body; }
    get currentScript() { return currentScript; }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get inputEncoding() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get compatMode() { return 'CSS1Compat'; }
    get designMode() { return 'off'; }
    set designMode(v) {}
    get dir() { return this.documentElement ? this.documentElement.dir : ''; }
    set dir(v) { if (this.documentElement) this.documentElement.dir = v; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get lastModified() { return new Date().toLocaleString(); }
    get forms() { return list(T.queryAll(this, 'form')); }
    get images() { return list(T.queryAll(this, 'img')); }
    get links() { return list(T.queryAll(this, 'a[href],area[href]')); }
    get anchors() { return list(T.queryAll(this, 'a[name]')); }
    get scripts() { return list(T.queryAll(this, 'script')); }
    get embeds() { return list(T.queryAll(this, 'embed')); }
    get plugins() { return list([]); }
    get styleSheets() { return list(T.queryAll(this, 'style,link[rel~=stylesheet]').map(() => new CSSStyleSheet())); }
    get adoptedStyleSheets() { return []; }
    set adoptedStyleSheets(v) {}
    get fonts() { return fonts; }
    get scrollingElement() { return this.documentElement; }
    get fullscreenElement() { return null; }
    get fullscreenEnabled() { return false; }
    get pictureInPictureEnabled() { return false; }
    get pointerLockElement() { return null; }
    get timeline() { return timeline; }
    getAnimations() { return [...waapi.values()].filter(a => a.playState !== 'idle'); }
    get implementation() { return implementation; }
    get children() { return list(elemChildren(this)); }
    get childElementCount() { return elemChildren(this).length; }
    get firstElementChild() { return this.documentElement; }
    get lastElementChild() { return this.documentElement; }
    getElementById(id) { return T.byId(str(id)); }
    getElementsByName(n) { return list(T.queryAll(this, '[name]').filter(e => T.getAttr(e, 'name') === String(n))); }
    createElement(tag, opts) {
        tag = String(tag);
        if (!/^[a-zA-Z][\w.\-·-￿:]*$/.test(tag)) throw new DOMException("The tag name provided ('" + tag + "') is not a valid name.", 'InvalidCharacterError');
        const cls = customDefs.get(tag.toLowerCase());
        if (cls) { try { return new cls(); } catch (x) { reportError(x); } }   /* (its constructor makes the element) */
        const e = T.create(tag.toLowerCase(), NS_HTML);
        if (opts && opts.is) T.setAttr(e, 'is', opts.is);
        return e;
    }
    createElementNS(ns, tag) { tag = String(tag).replace(/^\w+:/, ''); const n = nsOf(ns); const e = T.create(n === NS_HTML ? tag.toLowerCase() : tag, n); return e; }
    createTextNode(s) { return T.createText(str(s)); }
    createComment(s) { return T.createComment(str(s)); }
    createCDATASection(s) { return T.createText(str(s)); }
    createProcessingInstruction() { return T.createComment(''); }
    createDocumentFragment() { return T.createFrag(); }
    createAttribute(n) { return { name: n, value: '' }; }
    createEvent(kind) {
        const k = String(kind).toLowerCase();
        const C = /mouse/.test(k) ? MouseEvent : /keyboard/.test(k) ? KeyboardEvent : /custom/.test(k) ? CustomEvent : /ui/.test(k) ? UIEvent : /focus/.test(k) ? FocusEvent : Event;
        return new C('');
    }
    createRange() { return new Range(); }
    createTreeWalker(root, what, filter) { return new TreeWalker(root, what === undefined ? 0xFFFFFFFF : what, filter); }
    createNodeIterator(root, what, filter) { return new TreeWalker(root, what === undefined ? 0xFFFFFFFF : what, filter); }
    importNode(n, deep) { return n.cloneNode(deep); }
    adoptNode(n) { if (T.parent(n)) T.parent(n).removeChild(n); return n; }
    elementFromPoint(x, y) { return T.hit(x | 0, y | 0) || this.body; }
    elementsFromPoint(x, y) { const r = []; for (let e = T.hit(x | 0, y | 0); e && T.type(e) === 2; e = T.parent(e)) r.push(e); return r; }
    caretRangeFromPoint() { return null; }
    getSelection() { return G.getSelection(); }
    hasFocus() { return true; }
    execCommand() { return false; }
    queryCommandSupported() { return false; }
    queryCommandState() { return false; }
    queryCommandEnabled() { return false; }
    write(...s) { T.write(s.join('')); }
    writeln(...s) { T.write(s.join('') + '\n'); }
    open() { return this; }
    close() {}
    exitFullscreen() { return Promise.resolve(); }
    exitPointerLock() {}
    startViewTransition(cb) { const p = Promise.resolve().then(() => cb && cb()); return { finished: p, ready: p, updateCallbackDone: p, skipTransition() {} }; }
    querySelector(s) { return query(this, s, false); }
    querySelectorAll(s) { return list(query(this, s, true)); }
    getElementsByTagName(t) { return ParentMixin.getElementsByTagName.call(this, t); }
    getElementsByTagNameNS(ns, t) { return ParentMixin.getElementsByTagName.call(this, t); }
    getElementsByClassName(c) { return ParentMixin.getElementsByClassName.call(this, c); }
    append(...a) { ParentMixin.append.call(this, ...a); }
    prepend(...a) { ParentMixin.prepend.call(this, ...a); }
}
onProps(Document.prototype, EVENTS);
class HTMLDocument extends Document {}
class XMLDocument extends Document {}

/* a document of its own (DOMParser, createHTMLDocument): a detached <html> in this one, which is enough
 * for what scripts mostly do with it: read and query markup */
function detachedDocument(html) {
    const root = T.create('html');
    T.setInner(root, str(html));
    let head = T.query(root, 'head'), body = T.query(root, 'body');
    if (!head) { head = T.create('head'); T.insert(root, head, T.first(root)); }
    if (!body) { body = T.create('body'); while (T.next(head)) T.insert(body, T.next(head), null); T.insert(root, body, null); }
    const d = Object.create(Document.prototype);
    Object.defineProperties(d, {
        documentElement: { value: root }, head: { value: head }, body: { value: body },
        title: { get: () => { const t = T.query(root, 'title'); return t ? T.text(t).trim() : ''; }, set: v => {} },
        querySelector: { value: s => query(root, s, false) }, querySelectorAll: { value: s => list(query(root, s, true)) },
        getElementById: { value: id => query(root, '#' + cssEscape(id), false) },
        getElementsByTagName: { value: t => ParentMixin.getElementsByTagName.call(root, t) },
        getElementsByClassName: { value: c => ParentMixin.getElementsByClassName.call(root, c) },
        createElement: { value: t => document.createElement(t) }, createTextNode: { value: t => document.createTextNode(t) },
        createDocumentFragment: { value: () => T.createFrag() }, childNodes: { get: () => list([root]) }, firstChild: { get: () => root },
        nodeType: { value: 9 }, readyState: { value: 'complete' }, URL: { value: 'about:blank' }, cookie: { value: '', writable: true },
        defaultView: { value: null }, implementation: { value: implementation }, body_: { value: body },
    });
    return d;
}
const implementation = {
    createHTMLDocument(title) { return detachedDocument('<head><title>' + str(title).replace(/</g, '&lt;') + '</title></head><body></body>'); },
    createDocument() { return detachedDocument(''); },
    createDocumentType(n) { return { name: n, nodeType: 10 }; },
    hasFeature() { return true; },
};
class DOMParser {
    parseFromString(s, type) { return detachedDocument(/xml/.test(type || '') && !/html/.test(type) ? str(s).replace(/<\?xml[^>]*\?>/, '') : s); }
}
class XMLSerializer { serializeToString(n) { return T.type(n) === 2 ? T.outer(n) : T.type(n) === 1 ? T.outer(n.documentElement) : T.text(n); } }

class Range {
    constructor() { this.startContainer = this.endContainer = this.commonAncestorContainer = document; this.startOffset = this.endOffset = 0; this.collapsed = true; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; }
    setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
    setStartBefore(n) { this.startContainer = n; } setStartAfter(n) { this.startContainer = n; }
    setEndBefore(n) { this.endContainer = n; } setEndAfter(n) { this.endContainer = n; }
    selectNode(n) { this.startContainer = this.endContainer = this.commonAncestorContainer = n; }
    selectNodeContents(n) { this.selectNode(n); }
    collapse() { this.collapsed = true; }
    cloneRange() { return Object.assign(new Range(), this); }
    cloneContents() { return T.createFrag(); }
    extractContents() { return T.createFrag(); }
    deleteContents() {}
    insertNode(n) { const c = this.startContainer; if (T.type(c) === 2) c.insertBefore(n, c.childNodes[this.startOffset] || null); }
    surroundContents(n) { this.insertNode(n); }
    createContextualFragment(html) { return fragmentFrom(html); }
    getBoundingClientRect() { return T.type(this.startContainer) === 2 ? this.startContainer.getBoundingClientRect() : new DOMRect(); }
    getClientRects() { return list([]); }
    detach() {}
    toString() { return ''; }
}
class StaticRange extends Range {}
class Selection {
    constructor() { this.rangeCount = 0; this.isCollapsed = true; this.anchorNode = this.focusNode = null; this.anchorOffset = this.focusOffset = 0; this.type = 'None'; }
    getRangeAt() { return new Range(); } addRange() {} removeAllRanges() {} removeRange() {} empty() {} collapse() {} collapseToEnd() {} collapseToStart() {}
    selectAllChildren() {} extend() {} setBaseAndExtent() {} containsNode() { return false; } deleteFromDocument() {} modify() {}
    toString() { return ''; }
}
const selection = new Selection();

const NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2,
    SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024 };
class TreeWalker {
    constructor(root, what, filter) { this.root = root; this.whatToShow = what; this.filter = filter || null; this.currentNode = root; this._ref = root; this.referenceNode = root; this.pointerBeforeReferenceNode = true; }
    _ok(n) {
        if (!(this.whatToShow & (1 << ((n.nodeType || 1) - 1)))) return 3;
        if (!this.filter) return 1;
        return typeof this.filter === 'function' ? this.filter(n) : this.filter.acceptNode(n);
    }
    _next(n) { let c = T.first(n); if (c) return c; while (n && n !== this.root) { if (T.next(n)) return T.next(n); n = T.parent(n); } return null; }
    _prev(n) { if (n === this.root) return null; let p = T.prev(n); if (!p) return T.parent(n); while (T.last(p)) p = T.last(p); return p; }
    nextNode() { for (let n = this._next(this.currentNode); n; n = this._next(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    previousNode() { for (let n = this._prev(this.currentNode); n; n = this._prev(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    parentNode() { for (let n = T.parent(this.currentNode); n && n !== T.parent(this.root); n = T.parent(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    firstChild() { for (let n = T.first(this.currentNode); n; n = T.next(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    lastChild() { for (let n = T.last(this.currentNode); n; n = T.prev(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    nextSibling() { for (let n = T.next(this.currentNode); n; n = T.next(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    previousSibling() { for (let n = T.prev(this.currentNode); n; n = T.prev(n)) if (this._ok(n) === 1) return (this.currentNode = n); return null; }
    detach() {}
}

class CSSStyleSheet {
    constructor() { this.cssRules = list([]); this.rules = this.cssRules; this.disabled = false; this.media = { mediaText: '', length: 0 }; this.ownerNode = null; this.href = null; this.type = 'text/css'; }
    insertRule(r, i) {
        /* a rule a script adds goes into a <style> of its own, so tFire's style engine sees it */
        let s = this.__el;
        if (!s) { s = T.create('style'); hidden(this, '__el', s); const h = document.head; if (h) h.appendChild(s); }
        T.setText(s, T.text(s) + '\n' + str(r));
        T.changed();
        return i || 0;
    }
    deleteRule() {}
    addRule(sel, decl) { this.insertRule(sel + '{' + decl + '}'); return -1; }
    removeRule() {}
    replace(t) { this.replaceSync(t); return Promise.resolve(this); }
    replaceSync(t) { let s = this.__el; if (!s) { s = T.create('style'); hidden(this, '__el', s); const h = document.head; if (h) h.appendChild(s); } T.setText(s, str(t)); T.changed(); }
}
const fonts = { ready: Promise.resolve(), status: 'loaded', size: 0, check: () => true, load: () => Promise.resolve([]), add() {}, delete() {}, clear() {},
    forEach() {}, has: () => false, addEventListener() {}, removeEventListener() {}, onloadingdone: null, [Symbol.iterator]: () => [][Symbol.iterator]() };
class FontFace { constructor(family) { this.family = family; this.status = 'loaded'; this.loaded = Promise.resolve(this); } load() { return Promise.resolve(this); } }

/* ------------------------------------------------------------------ timers */

let timerSeq = 1;
const timers = new Map();                 /* id -> { at, fn, args, every } */
function addTimer(fn, ms, args, every) {
    const id = timerSeq++;
    ms = Math.max(every ? 4 : 0, +ms || 0);
    timers.set(id, { at: T.perf() + ms, fn, args, every: every ? ms : 0, seq: id });
    return id;
}
function setTimeout(fn, ms, ...args) { return addTimer(fn, ms, args, false); }
function setInterval(fn, ms, ...args) { return addTimer(fn, ms, args, true); }
function clearTimeout(id) { timers.delete(id); }
let rafSeq = 1;
const rafs = new Map();
function requestAnimationFrame(fn) { const id = rafSeq++; rafs.set(id, fn); return id; }
function cancelAnimationFrame(id) { rafs.delete(id); }
function requestIdleCallback(fn, opts) { return setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 15 }), opts && opts.timeout ? Math.min(opts.timeout, 50) : 30); }
function queueMicrotask(fn) { Promise.resolve().then(fn).catch(reportError); }

/* the engine calls this every so often: what is due runs; returns how long until the next one (-1: nothing waits) */
let lastRaf = 0;
function tick() {
    const now = T.perf();
    if (rafs.size && now - lastRaf >= 16) {
        lastRaf = now;
        const due = [...rafs]; rafs.clear();
        for (const [, fn] of due) { try { fn(now); } catch (e) { reportError(e); } }
    }
    for (let guard = 0; guard < 1000; guard++) {
        let best = null, bestId = 0;
        for (const [id, t] of timers) if (t.at <= now && (!best || t.at < best.at || (t.at === best.at && t.seq < best.seq))) { best = t; bestId = id; }
        if (!best) break;
        if (best.every) { best.at = Math.max(best.at + best.every, now); best.seq = timerSeq++; } else timers.delete(bestId);
        try {
            if (typeof best.fn === 'function') best.fn.apply(G, best.args);
            else (0, eval)(String(best.fn));
        } catch (e) { reportError(e); }
        if (best.every && best.at <= now) best.at = now + best.every;   /* one turn each tick at most */
    }
    let next = rafs.size ? 16 : -1;
    for (const t of timers.values()) { const d = Math.max(0, t.at - T.perf()); if (next < 0 || d < next) next = d; }
    return next < 0 ? -1 : Math.ceil(next);
}

/* ------------------------------------------------------------------ fetch and XMLHttpRequest */

class Headers {
    constructor(init) {
        hidden(this, '_m', new Map());
        if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
        else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
        else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); const o = this._m.get(k); this._m.set(k, o === undefined ? String(v) : o + ', ' + v); }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    getSetCookie() { return []; }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(fn, self) { for (const [k, v] of [...this._m].sort()) fn.call(self, v, k, this); }
    keys() { return [...this._m.keys()].sort()[Symbol.iterator](); }
    values() { return [...this._m].sort().map(e => e[1])[Symbol.iterator](); }
    entries() { return [...this._m].sort()[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
function parseHeaders(raw) {
    const h = new Headers();
    const lines = str(raw).split(/\r?\n/);
    let status = '';
    for (let i = 0; i < lines.length; i++) {
        const l = lines[i];
        if (i === 0 && /^HTTP\//.test(l)) { status = l.replace(/^HTTP\/[\d.]+\s+\d+\s*/, ''); continue; }
        const c = l.indexOf(':');
        if (c > 0 && !/^set-cookie/i.test(l)) h.append(l.slice(0, c).trim(), l.slice(c + 1).trim());
    }
    return { headers: h, statusText: status };
}

/* a request body, and the content type it implies */
function encodeBody(body, headers) {
    if (body === undefined || body === null) return null;
    const ct = () => headers.get('content-type');
    if (typeof body === 'string') { if (!ct()) headers.set('content-type', 'text/plain;charset=UTF-8'); return body; }
    if (body instanceof URLSearchParams) { if (!ct()) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); return body.toString(); }
    if (body instanceof FormData) {
        const b = '----tFireFormBoundary' + Math.random().toString(36).slice(2);
        headers.set('content-type', 'multipart/form-data; boundary=' + b);
        let s = '';
        for (const [k, v] of body._l) {
            if (v instanceof Blob) s += '--' + b + '\r\nContent-Disposition: form-data; name="' + k + '"; filename="' + (v.name || 'blob') + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n' + v._text + '\r\n';
            else s += '--' + b + '\r\nContent-Disposition: form-data; name="' + k + '"\r\n\r\n' + v + '\r\n';
        }
        return s + '--' + b + '--\r\n';
    }
    if (body instanceof Blob) { if (!ct() && body.type) headers.set('content-type', body.type); return body._text; }
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) {
        const u8 = body instanceof ArrayBuffer ? new Uint8Array(body) : new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
        return new TextDecoder().decode(u8);
    }
    return String(body);
}
function headerLines(h) { let s = ''; h.forEach((v, k) => { if (!/[\r\n]/.test(k + v) && !/^(host|content-length|connection|cookie|user-agent|accept-encoding)$/.test(k)) s += k + ': ' + v + '\r\n'; }); return s; }

let fetchSeq = 1;
const pending = new Map();                /* id -> what is waiting for it */
function startRequest(method, url, headers, body, done) {
    const id = fetchSeq++;
    pending.set(id, done);
    const r = T.fetch(id, method, String(url), headerLines(headers), body);
    if (r < 0) { pending.delete(id); setTimeout(() => done(0, '', '', null, 'tFire could not start the request'), 0); return 0; }
    return id;
}
/* the engine: a request ended */
function fetchDone(id, status, url, headers, body, error) {
    const d = pending.get(id);
    if (!d) return;
    pending.delete(id);
    d(status, url, headers, body, error);
}

class Body {
    _init(body) { hidden(this, '_body', body === undefined || body === null ? null : String(body)); hidden(this, '_used', false); }
    get bodyUsed() { return this._used; }
    get body() { const t = this._body; return t === null ? null : new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(t)); c.close(); } }); }
    _take() { if (this._used) return Promise.reject(new TypeError('Body has already been consumed.')); this._used = true; return Promise.resolve(this._body || ''); }
    text() { return this._take(); }
    json() { return this._take().then(t => JSON.parse(t)); }
    arrayBuffer() { return this._take().then(t => new TextEncoder().encode(t).buffer); }
    bytes() { return this._take().then(t => new TextEncoder().encode(t)); }
    blob() { return this._take().then(t => new Blob([t], { type: this.headers.get('content-type') || '' })); }
    formData() { return this._take().then(t => { const f = new FormData(); new URLSearchParams(t).forEach((v, k) => f.append(k, v)); return f; }); }
}
class Request extends Body {
    constructor(input, init) {
        super();
        init = init || {};
        const src = input instanceof Request ? input : null;
        this.url = src ? src.url : T.resolve(T.url(), String(input)) || String(input);
        this.method = String(init.method || (src ? src.method : 'GET')).toUpperCase();
        this.headers = new Headers(init.headers || (src ? src.headers : undefined));
        this.signal = init.signal || (src ? src.signal : null) || new AbortController().signal;
        this.credentials = init.credentials || 'same-origin';
        this.mode = init.mode || 'cors';
        this.cache = init.cache || 'default';
        this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client';
        this.integrity = init.integrity || '';
        this.keepalive = !!init.keepalive;
        this._init(init.body !== undefined ? encodeBody(init.body, this.headers) : src ? src._body : null);
    }
    clone() { return new Request(this); }
}
class Response extends Body {
    constructor(body, init) {
        super();
        init = init || {};
        this.status = init.status === undefined ? 200 : init.status;
        this.statusText = init.statusText || '';
        this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
        this.ok = this.status >= 200 && this.status < 300;
        this.type = 'basic';
        this.url = init.url || '';
        this.redirected = !!init.redirected;
        this._init(body instanceof Blob ? body._text : body === undefined || body === null ? null : typeof body === 'string' ? body : encodeBody(body, this.headers));
    }
    clone() { if (this._used) throw new TypeError('Response body is already used'); return new Response(this._body, this); }
    static json(d, init) { const h = new Headers(init && init.headers); h.set('content-type', 'application/json'); return new Response(JSON.stringify(d), Object.assign({}, init, { headers: h })); }
    static error() { const r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
    static redirect(u, s) { return new Response(null, { status: s || 302, headers: { location: u } }); }
}

function fetch(input, init) {
    let req;
    try { req = new Request(input, init); } catch (e) { return Promise.reject(e); }
    return new Promise((resolve, reject) => {
        if (req.signal && req.signal.aborted) { reject(req.signal.reason || new DOMException('The operation was aborted.', 'AbortError')); return; }
        if (/^data:/.test(req.url)) { const m = /^data:([^,]*?)(;base64)?,(.*)$/s.exec(req.url); const d = m ? (m[2] ? atob(m[3]) : decodeURIComponent(m[3])) : ''; resolve(new Response(d, { headers: { 'content-type': m ? m[1] : '' } })); return; }
        if (/^blob:/.test(req.url)) { const b = blobs.get(req.url); if (b) resolve(new Response(b._text, { headers: { 'content-type': b.type } })); else reject(new TypeError('Failed to fetch')); return; }
        const id = startRequest(req.method, req.url, req.headers, req._body, (status, url, raw, body, error) => {
            if (error || !status) { reject(new TypeError('Failed to fetch' + (error ? ': ' + error : ''))); return; }
            const p = parseHeaders(raw);
            resolve(new Response(body, { status, statusText: p.statusText, headers: p.headers, url, redirected: url !== req.url }));
        });
        if (req.signal) req.signal.addEventListener('abort', () => { T.abort(id); pending.delete(id); reject(req.signal.reason || new DOMException('The operation was aborted.', 'AbortError')); });
    });
}

class XMLHttpRequestEventTarget extends EventTarget {}
onProps(XMLHttpRequestEventTarget.prototype, ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend']);
class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {}
class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
        super();
        this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.responseXML = null; this.responseURL = '';
        this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.upload = new XMLHttpRequestUpload();
        hidden(this, '_h', new Headers()); hidden(this, '_rh', new Headers()); hidden(this, '_id', 0); hidden(this, '_resp', null);
    }
    _state(s) { this.readyState = s; dispatch(this, new Event('readystatechange')); }
    open(method, url) { this._method = String(method).toUpperCase(); this._url = String(url); this._h = new Headers(); this.status = 0; this.responseText = ''; this._resp = null; this._state(1); }
    setRequestHeader(k, v) { this._h.append(k, v); }
    overrideMimeType() {}
    send(body) {
        if (this.readyState !== 1) throw new DOMException('The object state must be OPENED.', 'InvalidStateError');
        const b = body === undefined || body === null || this._method === 'GET' || this._method === 'HEAD' ? null : encodeBody(body, this._h);
        dispatch(this, new ProgressEvent('loadstart'));
        let timedOut = false;
        const tm = this.timeout ? setTimeout(() => { timedOut = true; T.abort(this._id); pending.delete(this._id); this._state(4); dispatch(this, new ProgressEvent('timeout')); dispatch(this, new ProgressEvent('loadend')); }, this.timeout) : 0;
        this._id = startRequest(this._method, this._url, this._h, b, (status, url, raw, text, error) => {
            if (timedOut) return;
            if (tm) clearTimeout(tm);
            if (error || !status) {
                this.status = 0;
                this._state(4);
                dispatch(this, new ProgressEvent('error'));
                dispatch(this, new ProgressEvent('loadend'));
                return;
            }
            const p = parseHeaders(raw);
            this._rh = p.headers;
            this.status = status; this.statusText = p.statusText; this.responseURL = url;
            this._state(2);
            this.responseText = text || '';
            this._state(3);
            const n = this.responseText.length;
            this._state(4);
            dispatch(this, new ProgressEvent('progress', { lengthComputable: true, loaded: n, total: n }));
            dispatch(this, new ProgressEvent('load', { lengthComputable: true, loaded: n, total: n }));
            dispatch(this, new ProgressEvent('loadend', { lengthComputable: true, loaded: n, total: n }));
        });
    }
    abort() { if (this._id) { T.abort(this._id); pending.delete(this._id); } if (this.readyState > 0 && this.readyState < 4) { this._state(4); dispatch(this, new ProgressEvent('abort')); dispatch(this, new ProgressEvent('loadend')); } this.readyState = 0; }
    getResponseHeader(k) { return this.readyState >= 2 ? this._rh.get(k) : null; }
    getAllResponseHeaders() { let s = ''; if (this.readyState >= 2) this._rh.forEach((v, k) => { s += k + ': ' + v + '\r\n'; }); return s; }
    get response() {
        if (this.readyState !== 4) return this.responseType === '' || this.responseType === 'text' ? this.responseText : null;
        switch (this.responseType) {
        case 'json': try { return JSON.parse(this.responseText); } catch (e) { return null; }
        case 'document': return detachedDocument(this.responseText);
        case 'arraybuffer': return new TextEncoder().encode(this.responseText).buffer;
        case 'blob': return new Blob([this.responseText], { type: this._rh.get('content-type') || '' });
        default: return this.responseText;
        }
    }
}
onProps(XMLHttpRequest.prototype, ['readystatechange']);
Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });
Object.assign(XMLHttpRequest.prototype, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });

class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
    static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('The operation timed out.', 'TimeoutError')), ms); return c.signal; }
    static any(signals) { const c = new AbortController(); for (const s of signals) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', () => c.abort(s.reason)); } return c.signal; }
}
class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
        const s = this.signal;
        if (s.aborted) return;
        s.aborted = true;
        s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
        const ev = new Event('abort');
        if (typeof s.onabort === 'function') s.onabort(ev);
        dispatch(s, ev);
    }
}

class EventSource extends EventTarget { constructor(url) { super(); this.url = url; this.readyState = 2; setTimeout(() => dispatch(this, new Event('error')), 0); } close() {} }
onProps(EventSource.prototype, ['open', 'message', 'error']);
class WebSocket extends EventTarget {
    constructor(url) { super(); this.url = String(url); this.readyState = 3; this.protocol = ''; this.extensions = ''; this.bufferedAmount = 0; this.binaryType = 'blob';
        setTimeout(() => { dispatch(this, new Event('error')); dispatch(this, Object.assign(new Event('close'), { code: 1006, reason: '', wasClean: false })); }, 0); }
    send() { throw new DOMException('WebSocket is closed', 'InvalidStateError'); }
    close() {}
}
onProps(WebSocket.prototype, ['open', 'message', 'error', 'close']);
Object.assign(WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });

/* ------------------------------------------------------------------ data: text, blobs, forms, streams */

class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s) {
        s = str(s);
        const out = [];
        for (let i = 0; i < s.length; i++) {
            let c = s.charCodeAt(i);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.length) { const d = s.charCodeAt(i + 1); if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; } }
            if (c < 0x80) out.push(c);
            else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 63));
            else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
            else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return new Uint8Array(out);
    }
    encodeInto(s, dst) { const b = this.encode(s); const n = Math.min(b.length, dst.length); dst.set(b.subarray(0, n)); return { read: s.length, written: n }; }
}
class TextDecoder {
    constructor(label) { this.encoding = (label || 'utf-8').toLowerCase().replace(/^utf8$/, 'utf-8'); this.fatal = false; this.ignoreBOM = false; }
    decode(buf) {
        if (!buf) return '';
        const b = buf instanceof ArrayBuffer ? new Uint8Array(buf) : new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
        if (this.encoding !== 'utf-8') { let s = ''; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]); return s; }
        let s = '', i = 0;
        if (b[0] === 0xEF && b[1] === 0xBB && b[2] === 0xBF) i = 3;
        const parts = [];
        while (i < b.length) {
            const c = b[i];
            let cp, n = 0;
            if (c < 0x80) cp = c;
            else if (c >= 0xF0 && c < 0xF8) { cp = c & 7; n = 3; }
            else if (c >= 0xE0) { cp = c & 15; n = 2; }
            else if (c >= 0xC0) { cp = c & 31; n = 1; }
            else cp = 0xFFFD;
            let ok = true;
            for (let k = 1; k <= n; k++) { const d = b[i + k]; if (d === undefined || (d & 0xC0) !== 0x80) { ok = false; break; } cp = (cp << 6) | (d & 63); }
            if (!ok) { cp = 0xFFFD; n = 0; }
            parts.push(cp);
            i += n + 1;
            if (parts.length >= 8192) { s += String.fromCodePoint(...parts); parts.length = 0; }
        }
        return s + (parts.length ? String.fromCodePoint(...parts) : '');
    }
}
function btoa(s) {
    s = str(s);
    const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        if (a > 255 || b > 255 || c > 255) throw new DOMException("Failed to execute 'btoa': The string to be encoded contains characters outside of the Latin1 range.", 'InvalidCharacterError');
        const n = (a << 16) | ((b || 0) << 8) | (c || 0);
        out += A[n >> 18] + A[(n >> 12) & 63] + (i + 1 < s.length ? A[(n >> 6) & 63] : '=') + (i + 2 < s.length ? A[n & 63] : '=');
    }
    return out;
}
function atob(s) {
    s = str(s).replace(/[\s=]/g, '');
    const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let out = '', bits = 0, acc = 0;
    for (const ch of s) {
        const v = A.indexOf(ch === '-' ? '+' : ch === '_' ? '/' : ch);
        if (v < 0) throw new DOMException("Failed to execute 'atob': The string to be decoded is not correctly encoded.", 'InvalidCharacterError');
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += String.fromCharCode((acc >> bits) & 255); }
    }
    return out;
}
class Blob {
    constructor(parts, opts) {
        let t = '';
        for (const p of parts || []) t += p instanceof Blob ? p._text : ArrayBuffer.isView(p) || p instanceof ArrayBuffer ? new TextDecoder('latin1').decode(p) : String(p);
        hidden(this, '_text', t);
        this.type = (opts && opts.type) || '';
    }
    get size() { return this._text.length; }
    text() { return Promise.resolve(this._text); }
    arrayBuffer() { const b = new Uint8Array(this._text.length); for (let i = 0; i < b.length; i++) b[i] = this._text.charCodeAt(i) & 255; return Promise.resolve(b.buffer); }
    bytes() { return this.arrayBuffer().then(b => new Uint8Array(b)); }
    slice(a, b, type) { return new Blob([this._text.slice(a, b)], { type }); }
    stream() { const t = this._text; return new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(t)); c.close(); } }); }
}
class File extends Blob { constructor(parts, name, opts) { super(parts, opts); this.name = String(name); this.lastModified = Date.now(); } }
class FileReader extends EventTarget {
    constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
    _done(r) { this.readyState = 2; this.result = r; setTimeout(() => { dispatch(this, new ProgressEvent('load')); dispatch(this, new ProgressEvent('loadend')); }, 0); }
    readAsText(b) { this._done(b._text); }
    readAsDataURL(b) { this._done('data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(b._text.replace(/[^\x00-\xff]/g, '?'))); }
    readAsArrayBuffer(b) { b.arrayBuffer().then(r => this._done(r)); }
    readAsBinaryString(b) { this._done(b._text); }
    abort() {}
}
onProps(FileReader.prototype, ['load', 'loadend', 'loadstart', 'progress', 'error', 'abort']);
class FormData {
    constructor(form, submitter) {
        hidden(this, '_l', []);
        if (form) for (const [k, v] of formEntries(form, submitter)) this._l.push([k, v]);
    }
    append(k, v, fn) { this._l.push([String(k), v instanceof Blob ? (fn ? Object.assign(v, { name: fn }) : v) : String(v)]); }
    set(k, v) { k = String(k); this.delete(k); this.append(k, v); }
    get(k) { const e = this._l.find(e => e[0] === String(k)); return e ? e[1] : null; }
    getAll(k) { return this._l.filter(e => e[0] === String(k)).map(e => e[1]); }
    has(k) { return this._l.some(e => e[0] === String(k)); }
    delete(k) { this._l = this._l.filter(e => e[0] !== String(k)); }
    forEach(fn, self) { for (const [k, v] of this._l) fn.call(self, v, k, this); }
    keys() { return this._l.map(e => e[0])[Symbol.iterator](); }
    values() { return this._l.map(e => e[1])[Symbol.iterator](); }
    entries() { return this._l.map(e => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
}
/* what a form sends: its successful controls, in order */
function formEntries(form, submitter) {
    const out = [];
    for (const e of form.elements) {
        const name = T.getAttr(e, 'name');
        const tag = T.name(e);
        if (!name || T.getAttr(e, 'disabled') !== null || e.closest('datalist')) continue;
        if (tag === 'button' || (tag === 'input' && /^(submit|image|reset|button)$/i.test(T.getAttr(e, 'type') || ''))) {
            if (e === submitter) out.push([name, T.getAttr(e, 'value') || '']);
            continue;
        }
        if (tag === 'input') {
            const type = (T.getAttr(e, 'type') || 'text').toLowerCase();
            if ((type === 'checkbox' || type === 'radio') && !T.checked(e)) continue;
            if (type === 'file') continue;
        }
        if (tag === 'select') { for (const o of T.queryAll(e, 'option')) if (o.selected && T.getAttr(o, 'disabled') === null) out.push([name, o.value]); if (!T.queryAll(e, 'option').some(o => o.selected)) { const f = T.query(e, 'option'); if (f && T.getAttr(e, 'multiple') === null) out.push([name, f.value]); } continue; }
        if (tag === 'object' || tag === 'fieldset' || tag === 'output') continue;
        out.push([name, T.value(e)]);
    }
    return out;
}

class ReadableStream {
    constructor(src) {
        hidden(this, '_q', []); hidden(this, '_done', false); hidden(this, '_w', []);
        const c = { enqueue: v => { this._q.push(v); this._flush(); }, close: () => { this._done = true; this._flush(); }, error: e => { this._err = e; this._flush(); }, desiredSize: 1 };
        if (src && src.start) { try { src.start(c); } catch (e) { this._err = e; } }
        hidden(this, '_pull', src && src.pull ? () => src.pull(c) : null);
        this.locked = false;
    }
    _flush() {
        while (this._w.length && (this._q.length || this._done || this._err)) {
            const w = this._w.shift();
            if (this._err) w.rej(this._err);
            else if (this._q.length) w.res({ value: this._q.shift(), done: false });
            else w.res({ value: undefined, done: true });
        }
    }
    getReader() {
        this.locked = true;
        const s = this;
        return {
            read() { return new Promise((res, rej) => { s._w.push({ res, rej }); s._flush(); if (s._w.length && s._pull) s._pull(); }); },
            releaseLock() { s.locked = false; }, cancel() { s._done = true; s._q.length = 0; s._flush(); return Promise.resolve(); },
            get closed() { return Promise.resolve(); },
        };
    }
    cancel() { this._done = true; return Promise.resolve(); }
    pipeTo(w) { const r = this.getReader(); const pump = () => r.read().then(({ value, done }) => { if (done) return w.close && w.close(); w.write(value); return pump(); }); return pump(); }
    pipeThrough(t) { this.pipeTo(t.writable); return t.readable; }
    tee() { return [this, this]; }
    async *[Symbol.asyncIterator]() { const r = this.getReader(); for (;;) { const { value, done } = await r.read(); if (done) return; yield value; } }
}
class WritableStream { constructor(sink) { this._s = sink || {}; } getWriter() { const s = this._s; return { write: v => Promise.resolve(s.write && s.write(v)), close: () => Promise.resolve(s.close && s.close()), releaseLock() {}, ready: Promise.resolve(), closed: Promise.resolve() }; } }
class TransformStream {
    constructor(t) {
        let ctl;
        this.readable = new ReadableStream({ start(c) { ctl = c; } });
        const tr = t || {};
        this.writable = { write: v => tr.transform ? tr.transform(v, ctl) : ctl.enqueue(v), close: () => { if (tr.flush) tr.flush(ctl); ctl.close(); } };
    }
}

/* ------------------------------------------------------------------ storage */

class Storage {
    constructor(session) { hidden(this, '_s', session); }
    getItem(k) { return T.storeGet(this._s, String(k)); }
    setItem(k, v) { T.storeSet(this._s, String(k), String(v)); }
    removeItem(k) { T.storeSet(this._s, String(k), null); }
    clear() { T.storeClear(this._s); }
    key(i) { return T.storeKeys(this._s)[i] || null; }
    get length() { return T.storeKeys(this._s).length; }
}
function storage(session) {
    const s = new Storage(session);
    return new Proxy(s, {
        get(t, k) { if (typeof k === 'symbol' || k in t) { const v = t[k]; return typeof v === 'function' ? v.bind(t) : v; } const v = T.storeGet(session, k); return v === null ? undefined : v; },
        set(t, k, v) { if (typeof k === 'symbol') return false; T.storeSet(session, k, String(v)); return true; },
        deleteProperty(t, k) { T.storeSet(session, String(k), null); return true; },
        has(t, k) { return k in t || T.storeGet(session, String(k)) !== null; },
        ownKeys() { return T.storeKeys(session); },
        getOwnPropertyDescriptor(t, k) { const v = T.storeGet(session, String(k)); return v === null ? undefined : { value: v, enumerable: true, configurable: true, writable: true }; },
    });
}
const localStorage = storage(false), sessionStorage = storage(true);

/* ------------------------------------------------------------------ the rest of window */

const counts = {}, timesStart = {};
let groupDepth = 0;
function clog(level) { return (...a) => T.log(level, '  '.repeat(groupDepth) + logArgs(a)); }
const console = {
    log: clog(0), info: clog(0), debug: clog(0), warn: clog(1), error: clog(2), trace: clog(0), dir: clog(0), dirxml: clog(0),
    table: clog(0), group: (...a) => { if (a.length) clog(0)(...a); groupDepth++; }, groupCollapsed: (...a) => { if (a.length) clog(0)(...a); groupDepth++; },
    groupEnd: () => { if (groupDepth) groupDepth--; }, assert: (c, ...a) => { if (!c) clog(2)('Assertion failed:', ...a); },
    count: (l = 'default') => { counts[l] = (counts[l] || 0) + 1; clog(0)(l + ': ' + counts[l]); }, countReset: (l = 'default') => { counts[l] = 0; },
    time: (l = 'default') => { timesStart[l] = T.perf(); }, timeEnd: (l = 'default') => { clog(0)(l + ': ' + (T.perf() - (timesStart[l] || 0)) + ' ms'); delete timesStart[l]; },
    timeLog: (l = 'default', ...a) => { clog(0)(l + ': ' + (T.perf() - (timesStart[l] || 0)) + ' ms', ...a); }, timeStamp() {}, profile() {}, profileEnd() {}, clear() {},
};

const UA = 'Mozilla/5.0 (X11; tOS i686) AppleWebKit/537.36 (KHTML, like Gecko) tFire/3.0 Safari/537.36';
const navigator = {
    userAgent: UA, appVersion: UA.slice(8), appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',
    vendor: 'tOS', vendorSub: '', platform: 'Linux i686', oscpu: 'tOS i686', language: 'tr-TR', languages: ['tr-TR', 'tr', 'en-US', 'en'],
    onLine: true, cookieEnabled: true, doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0, deviceMemory: 1, webdriver: false, pdfViewerEnabled: false,
    plugins: list([]), mimeTypes: list([]), javaEnabled: () => false,
    userAgentData: { brands: [{ brand: 'tFire', version: '3' }], mobile: false, platform: 'tOS', getHighEntropyValues: () => Promise.resolve({ platform: 'tOS', mobile: false }), toJSON() { return { brands: this.brands, mobile: false, platform: 'tOS' }; } },
    sendBeacon(url, data) { try { const h = new Headers(); const b = encodeBody(data, h); startRequest('POST', T.resolve(T.url(), String(url)) || url, h, b, () => {}); return true; } catch (e) { return false; } },
    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve(''), write: () => Promise.resolve(), read: () => Promise.resolve([]) },
    permissions: { query: () => Promise.resolve({ state: 'denied', onchange: null, addEventListener() {} }) },
    mediaDevices: { getUserMedia: () => Promise.reject(new DOMException('no camera', 'NotAllowedError')), enumerateDevices: () => Promise.resolve([]), addEventListener() {} },
    connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener() {}, removeEventListener() {} },
    storage: { estimate: () => Promise.resolve({ quota: 1 << 26, usage: 0 }), persist: () => Promise.resolve(false), persisted: () => Promise.resolve(false) },
    locks: { request: (n, o, fn) => Promise.resolve((typeof o === 'function' ? o : fn)({ name: n })) },
    vibrate: () => false, share: () => Promise.reject(new DOMException('not supported', 'NotAllowedError')), canShare: () => false,
    registerProtocolHandler() {}, getGamepads: () => [], getBattery: () => Promise.resolve({ level: 1, charging: true, addEventListener() {} }),
};
const screen = {
    get width() { return Math.max(T.vw(), 1024); }, get height() { return Math.max(T.vh(), 768); },
    get availWidth() { return this.width; }, get availHeight() { return this.height; }, availLeft: 0, availTop: 0,
    colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {}, removeEventListener() {}, lock: () => Promise.reject(new Error('no')) },
};

const perfMarks = [];
const performance = {
    now: () => T.perf(),
    get timeOrigin() { return T.now() - T.perf(); },
    timing: { navigationStart: T.now() - T.perf(), loadEventEnd: 0, domContentLoadedEventEnd: 0, responseStart: 0, domInteractive: 0 },
    navigation: { type: 0, redirectCount: 0 },
    mark(n, o) { const m = { name: n, entryType: 'mark', startTime: T.perf(), duration: 0, detail: o && o.detail }; perfMarks.push(m); return m; },
    measure(n) { const m = { name: n, entryType: 'measure', startTime: 0, duration: T.perf() }; perfMarks.push(m); return m; },
    getEntries: () => perfMarks.slice(), getEntriesByName: n => perfMarks.filter(m => m.name === n), getEntriesByType: t => perfMarks.filter(m => m.entryType === t),
    clearMarks() {}, clearMeasures() {}, clearResourceTimings() {}, setResourceTimingBufferSize() {}, toJSON() { return {}; },
    memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 1 << 28 },
    addEventListener() {}, removeEventListener() {},
};
class PerformanceObserver { constructor(cb) { this._cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } }
PerformanceObserver.supportedEntryTypes = [];

const crypto = {
    getRandomValues(a) {
        if (!ArrayBuffer.isView(a)) throw new TypeError("Failed to execute 'getRandomValues'");
        const b = new Uint8Array(T.random(a.byteLength));
        new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(b);
        return a;
    },
    randomUUID() {
        const b = new Uint8Array(T.random(16));
        b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
        const h = [...b].map(x => x.toString(16).padStart(2, '0')).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
    },
    subtle: { digest: () => Promise.reject(new DOMException('tFire has no SubtleCrypto yet', 'NotSupportedError')) },
};

function matchMedia(q) {
    q = String(q);
    const test = () => {
        const w = T.vw(), h = T.vh();
        return q.split(',').some(part => {
            part = part.trim().toLowerCase();
            const neg = /^not\b/.test(part);
            let ok = !(/\b(print|speech)\b/.test(part) && !/\bscreen\b/.test(part));
            for (const c of part.match(/\(([^)]*)\)/g) || []) {
                const m = /\(\s*([\w-]+)\s*(?::\s*([^)]*))?\)/.exec(c);
                if (!m) continue;
                const f = m[1], v = (m[2] || '').trim();
                const px = x => parseFloat(x) * (/em$/.test(x) ? 16 : 1);
                let r = true;
                if (f === 'min-width') r = w >= px(v); else if (f === 'max-width') r = w <= px(v);
                else if (f === 'min-height') r = h >= px(v); else if (f === 'max-height') r = h <= px(v);
                else if (f === 'orientation') r = v === (w >= h ? 'landscape' : 'portrait');
                else if (f === 'prefers-color-scheme') r = v === 'light';
                else if (f === 'prefers-reduced-motion') r = v === 'reduce';
                else if (f === 'prefers-contrast') r = v === 'no-preference';
                else if (f === 'hover' || f === 'any-hover') r = v === 'hover' || !v;
                else if (f === 'pointer' || f === 'any-pointer') r = v === 'fine' || !v;
                else if (f === 'display-mode') r = v === 'browser';
                else if (f === 'min-resolution' || f === '-webkit-min-device-pixel-ratio') r = parseFloat(v) <= 1;
                else if (f === 'max-resolution' || f === '-webkit-max-device-pixel-ratio') r = parseFloat(v) >= 1;
                else if (f === 'monochrome' || f === 'forced-colors' || f === 'inverted-colors') r = v === 'none' || v === '0';
                if (!r) ok = false;
            }
            return neg ? !ok : ok;
        });
    };
    const mq = Object.create(EventTarget.prototype);
    Object.defineProperties(mq, { matches: { get: test, enumerable: true }, media: { value: q, enumerable: true } });
    mq.onchange = null;
    mq.addListener = fn => mq.addEventListener('change', fn);
    mq.removeListener = fn => mq.removeEventListener('change', fn);
    return mq;
}

function getComputedStyle(el, pseudo) {
    if (!(el instanceof Element)) throw new TypeError("Failed to execute 'getComputedStyle': parameter 1 is not of type 'Element'.");
    const o = T.style(el);
    if (pseudo) { o.content = 'none'; o.display = 'inline'; }
    const inl = parseDecls(T.getAttr(el, 'style'));
    for (const [k, v] of inl) { const c = camel(k); if (!(c in o)) o[c] = v.replace(/\s*!important$/, ''); }
    for (const k of Object.keys(o)) { const kb = kebab(k); if (kb !== k && !(kb in o)) Object.defineProperty(o, kb, { value: o[k], enumerable: false }); }
    o.getPropertyValue = k => { k = String(k); if (k.startsWith('--')) return ''; const v = o[camel(k)]; return v === undefined ? '' : String(v); };
    o.getPropertyPriority = () => '';
    o.item = i => Object.keys(o)[i] || '';
    Object.defineProperty(o, 'length', { value: Object.keys(o).length, enumerable: false });
    return o;
}

/* ------------------------------------------------------------------ element.animate() (the Web Animations API) */

/* the engine moves them (anim.c), like CSS animations: here only the objects that stand for them */
const waapi = new Map();                  /* engine id -> Animation */
const timeline = { get currentTime() { return T.perf(); } };
class AnimationPlaybackEvent extends Event {
    constructor(t, i) { super(t, i); i = i || {}; this.currentTime = i.currentTime ?? null; this.timelineTime = i.timelineTime ?? null; }
}
class AnimationEffect {
    constructor(timing) { this._timing = timing || {}; }
    getTiming() { return Object.assign({ delay: 0, endDelay: 0, fill: 'auto', iterationStart: 0, iterations: 1, duration: 'auto', direction: 'normal', easing: 'linear' }, this._timing); }
    getComputedTiming() {
        const t = this.getTiming(), d = +t.duration || 0;
        return Object.assign(t, { duration: d, activeDuration: d * t.iterations, endTime: t.delay + d * t.iterations + t.endDelay, localTime: null, progress: null, currentIteration: null });
    }
    updateTiming(t) { Object.assign(this._timing, t || {}); }
}
class KeyframeEffect extends AnimationEffect {
    constructor(target, keyframes, options) {
        super(typeof options === 'number' ? { duration: options } : Object.assign({}, options || {}));
        this.target = target || null;
        this._kf = keyframes;
        this.composite = 'replace';
        this.pseudoElement = null;
    }
    getKeyframes() { return framesOf(this._kf); }
    setKeyframes(k) { this._kf = k; }
}
/* keyframes as given (a list of frames, or one object of lists) -> frames with offsets */
function framesOf(kf) {
    let frames = [];
    if (kf == null) return frames;
    if (typeof kf[Symbol.iterator] === 'function' && typeof kf !== 'string') frames = [...kf].map(f => Object.assign({}, f));
    else if (typeof kf === 'object') {
        const keys = Object.keys(kf).filter(k => k !== 'offset' && k !== 'easing' && k !== 'composite');
        const n = Math.max(1, ...keys.map(k => Array.isArray(kf[k]) ? kf[k].length : 1));
        for (let i = 0; i < n; i++) {
            const f = {};
            for (const k of keys) {
                const v = kf[k];
                if (Array.isArray(v)) { if (v.length === 1 ? i === n - 1 : i < v.length) f[k] = v.length === 1 ? v[0] : v[i]; }
                else if (i === n - 1) f[k] = v;
            }
            if (Array.isArray(kf.offset) && kf.offset[i] != null) f.offset = kf.offset[i];
            if (kf.easing) f.easing = Array.isArray(kf.easing) ? kf.easing[i % kf.easing.length] : kf.easing;
            frames.push(f);
        }
    }
    const n = frames.length;
    if (n === 1 && frames[0].offset == null) frames[0].offset = 1;          /* one frame: where it goes */
    frames.forEach((f, i) => { if (f.offset == null) f.offset = n > 1 ? i / (n - 1) : 1; f.computedOffset = f.offset; });
    return frames;
}
function framesText(kf) {
    return framesOf(kf).map(f => {
        let d = '';
        for (const k of Object.keys(f)) {
            if (k === 'offset' || k === 'computedOffset' || k === 'composite') continue;
            if (k === 'easing') { d += 'animation-timing-function:' + f[k] + ';'; continue; }
            d += (k === 'cssFloat' ? 'float' : kebab(k)) + ':' + f[k] + ';';
        }
        return (Math.max(0, Math.min(1, +f.offset)) * 100) + '%{' + d + '}';
    }).join('');
}
const DIRS = { normal: 0, reverse: 1, alternate: 2, 'alternate-reverse': 3 };
const FILLS = { none: 0, auto: 0, forwards: 1, backwards: 2, both: 3 };
class Animation extends EventTarget {
    constructor(effect, tl) {
        super();
        this.effect = effect || null;
        this.timeline = tl || timeline;
        this.id = '';
        this.onfinish = null; this.oncancel = null; this.onremove = null;
        this.replaceState = 'active';
        hidden(this, '_id', 0); hidden(this, '_rate', 1); hidden(this, '_idle', true);
        this._promise();
    }
    _promise() {
        let res, rej;
        hidden(this, '_finished', new Promise((a, b) => { res = a; rej = b; }));
        this._finished.catch(() => {});
        hidden(this, '_res', res); hidden(this, '_rej', rej);
    }
    _start() {
        if (this._id || !this.effect || !this.effect.target) return;
        const t = this.effect.getTiming();
        const it = t.iterations === Infinity ? -1 : Math.max(0, Math.round((+t.iterations || 0) * 1000));
        this._id = T.animate(this.effect.target, framesText(this.effect._kf), Math.max(0, +t.duration || 0), +t.delay || 0,
            String(t.easing || 'linear'), it, DIRS[t.direction] || 0, FILLS[t.fill] || 0);
        this._idle = false;
        if (this._id) waapi.set(this._id, this);
        if (this._rate < 0) T.animCtl(this._id, 4, 0);
    }
    get finished() { return this._finished; }
    get ready() { return Promise.resolve(this); }
    get pending() { return false; }
    get playState() { return this._idle || !this._id ? 'idle' : ['idle', 'running', 'paused', 'finished'][T.animInfo(this._id)[0]]; }
    get currentTime() { return this._id && !this._idle ? T.animInfo(this._id)[1] : null; }
    set currentTime(v) { if (!this._id) this._start(); if (this._id) T.animCtl(this._id, 5, Math.max(0, +v | 0)); }
    get startTime() { const c = this.currentTime; return c == null ? null : T.perf() - c; }
    set startTime(v) { if (v != null) this.currentTime = T.perf() - v; }
    get playbackRate() { return this._rate; }
    set playbackRate(v) { v = +v; if ((v < 0) !== (this._rate < 0) && this._id) T.animCtl(this._id, 4, 0); this._rate = v; }
    updatePlaybackRate(v) { this.playbackRate = v; }
    play() {
        if (!this._id) { this._start(); return; }
        if (this.playState === 'finished') this._promise();
        T.animCtl(this._id, 0, 0);
    }
    pause() { if (!this._id) this._start(); if (this._id) T.animCtl(this._id, 1, 0); }
    finish() { if (!this._id) this._start(); if (this._id) T.animCtl(this._id, 3, 0); }
    reverse() {
        if (!this._id) { this._rate = -this._rate; this._start(); return; }
        if (this.playState === 'finished') this._promise();
        this._rate = -this._rate;
        T.animCtl(this._id, 4, 0);
        T.animCtl(this._id, 0, 0);
    }
    cancel() {
        if (this._id) { T.animCtl(this._id, 2, 0); waapi.delete(this._id); this._id = 0; }
        if (this._idle) return;
        this._idle = true;
        this._rej(new DOMException('The user aborted a request.', 'AbortError'));
        this._promise();
        const ev = new AnimationPlaybackEvent('cancel', { timelineTime: T.perf() });
        if (typeof this.oncancel === 'function') try { this.oncancel(ev); } catch (e) { reportError(e); }
        this.dispatchEvent(ev);
    }
    persist() { this.replaceState = 'persisted'; }
    commitStyles() {
        const el = this.effect && this.effect.target;
        const f = this.effect ? framesOf(this.effect._kf) : [];
        const last = this._rate < 0 ? f[0] : f[f.length - 1];
        if (el && last) for (const k of Object.keys(last)) if (!/^(offset|computedOffset|easing|composite)$/.test(k)) el.style[camel(kebab(k))] = last[k];
    }
    _done() {
        const ev = new AnimationPlaybackEvent('finish', { currentTime: this.currentTime, timelineTime: T.perf() });
        this._res(this);
        if (typeof this.onfinish === 'function') try { this.onfinish(ev); } catch (e) { reportError(e); }
        this.dispatchEvent(ev);
    }
}
function elementAnimate(el, keyframes, options) {
    const o = typeof options === 'number' ? { duration: options } : Object.assign({}, options || {});
    const a = new Animation(new KeyframeEffect(el, keyframes, o));
    if (o.id) a.id = String(o.id);
    a.play();
    return a;
}

class IntersectionObserver {
    constructor(cb, opts) { this._cb = cb; this.root = (opts && opts.root) || null; this.rootMargin = (opts && opts.rootMargin) || '0px'; this.thresholds = [].concat((opts && opts.threshold) || 0); this._t = []; }
    observe(el) {
        if (this._t.includes(el)) return;
        this._t.push(el);
        /* whatever is observed is taken to be in view: lazy loaders then load everything */
        setTimeout(() => {
            if (!this._t.includes(el)) return;
            const r = el.getBoundingClientRect();
            try { this._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: new DOMRect(0, 0, T.vw(), T.vh()), time: T.perf() }], this); } catch (e) { reportError(e); }
        }, 0);
    }
    unobserve(el) { this._t = this._t.filter(x => x !== el); }
    disconnect() { this._t = []; }
    takeRecords() { return []; }
}
class ResizeObserver {
    constructor(cb) { this._cb = cb; this._t = []; }
    observe(el) {
        this._t.push(el);
        setTimeout(() => {
            if (!this._t.includes(el)) return;
            const r = el.getBoundingClientRect();
            const box = [{ inlineSize: r.width, blockSize: r.height }];
            try { this._cb([{ target: el, contentRect: new DOMRect(0, 0, r.width, r.height), borderBoxSize: box, contentBoxSize: box, devicePixelContentBoxSize: box }], this); } catch (e) { reportError(e); }
        }, 0);
    }
    unobserve(el) { this._t = this._t.filter(x => x !== el); }
    disconnect() { this._t = []; }
}

function structuredClone(v) {
    const seen = new Map();
    const c = x => {
        if (x === null || typeof x !== 'object') { if (typeof x === 'function' || typeof x === 'symbol') throw new DOMException('could not be cloned', 'DataCloneError'); return x; }
        if (seen.has(x)) return seen.get(x);
        let r;
        if (x instanceof Date) r = new Date(x.getTime());
        else if (x instanceof RegExp) r = new RegExp(x.source, x.flags);
        else if (x instanceof Map) { r = new Map(); seen.set(x, r); for (const [k, val] of x) r.set(c(k), c(val)); return r; }
        else if (x instanceof Set) { r = new Set(); seen.set(x, r); for (const val of x) r.add(c(val)); return r; }
        else if (ArrayBuffer.isView(x)) r = x.slice();
        else if (x instanceof ArrayBuffer) r = x.slice(0);
        else if (x instanceof Node) throw new DOMException('could not be cloned', 'DataCloneError');
        else { r = Array.isArray(x) ? [] : {}; seen.set(x, r); for (const k of Object.keys(x)) r[k] = c(x[k]); return r; }
        seen.set(x, r);
        return r;
    };
    return c(v);
}

class BroadcastChannel extends EventTarget { constructor(n) { super(); this.name = n; } postMessage() {} close() {} }
class MessageChannel { constructor() { const mk = () => Object.assign(Object.create(EventTarget.prototype), { postMessage() {}, start() {}, close() {}, onmessage: null }); this.port1 = mk(); this.port2 = mk(); } }
function Image(w, h) { const i = T.create('img'); if (w !== undefined) T.setAttr(i, 'width', String(w)); if (h !== undefined) T.setAttr(i, 'height', String(h)); return i; }
Image.prototype = HTMLImageElement.prototype;
function Audio(src) { const a = T.create('audio'); if (src) T.setAttr(a, 'src', String(src)); return a; }
Audio.prototype = HTMLAudioElement.prototype;

let scrollX = 0;
function scrollTo(x, y) {
    if (x && typeof x === 'object') { y = x.top; x = x.left; }
    if (y !== undefined && y !== null && !isNaN(y)) T.scrollTo(Math.max(0, y | 0));
}

/* ------------------------------------------------------------------ window */

T.setup(protoFor, engineEvent, windowEvent, tick, fetchDone, imageLoaded, s => { currentScript = s; });
const document = T.doc();
const windowProps = {
    window: G, self: G, top: G, parent: G, frames: G, globalThis: G, document, location, history, navigator, screen, console, performance, crypto,
    localStorage, sessionStorage, customElements, visualViewport: null, frameElement: null, opener: null, closed: false, name: '', status: '',
    length: 0, origin: undefined, isSecureContext: true, crossOriginIsolated: false, devicePixelRatio: 1, locationbar: { visible: true },
    menubar: { visible: true }, toolbar: { visible: true }, statusbar: { visible: true }, scrollbars: { visible: true }, personalbar: { visible: true },
    EventTarget, Event, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent, CustomEvent, SubmitEvent, ProgressEvent,
    MessageEvent, PopStateEvent, HashChangeEvent, ErrorEvent, PromiseRejectionEvent, TransitionEvent, AnimationEvent,
    Animation, AnimationEffect, KeyframeEffect, AnimationPlaybackEvent, ClipboardEvent, TouchEvent,
    Node, CharacterData, Text, CDATASection, Comment, DocumentType, DocumentFragment, ShadowRoot, Element, HTMLElement, Document, HTMLDocument, XMLDocument,
    Attr, NamedNodeMap, DOMTokenList, DOMRect, DOMRectReadOnly, DOMException, CSSStyleDeclaration, CSSStyleSheet, FontFace,
    MutationObserver, IntersectionObserver, ResizeObserver, PerformanceObserver, DOMParser, XMLSerializer, Range, StaticRange, Selection, NodeFilter, TreeWalker,
    URL, URLSearchParams, Headers, Request, Response, AbortController, AbortSignal, XMLHttpRequest, XMLHttpRequestUpload, XMLHttpRequestEventTarget,
    EventSource, WebSocket, TextEncoder, TextDecoder, Blob, File, FileReader, FormData, ReadableStream, WritableStream, TransformStream, Storage,
    BroadcastChannel, MessageChannel, Image, Audio, Option: HTMLOptionElement, SVGElement, SVGGraphicsElement, SVGSVGElement, MathMLElement,
    HTMLUnknownElement, HTMLDivElement, HTMLSpanElement, HTMLParagraphElement, HTMLHeadingElement, HTMLBRElement, HTMLHRElement, HTMLPreElement,
    HTMLQuoteElement, HTMLUListElement, HTMLOListElement, HTMLLIElement, HTMLDListElement, HTMLHtmlElement, HTMLHeadElement, HTMLBodyElement,
    HTMLTitleElement, HTMLMetaElement, HTMLBaseElement, HTMLLinkElement, HTMLStyleElement, HTMLScriptElement, HTMLNoScriptElement, HTMLTemplateElement,
    HTMLSlotElement, HTMLAnchorElement, HTMLAreaElement, HTMLImageElement, HTMLPictureElement, HTMLSourceElement, HTMLIFrameElement, HTMLEmbedElement,
    HTMLObjectElement, HTMLMediaElement, HTMLVideoElement, HTMLAudioElement, HTMLTrackElement, HTMLCanvasElement, HTMLFormElement, HTMLInputElement,
    HTMLButtonElement, HTMLTextAreaElement, HTMLSelectElement, HTMLOptionElement, HTMLOptGroupElement, HTMLLabelElement, HTMLFieldSetElement,
    HTMLLegendElement, HTMLOutputElement, HTMLDataListElement, HTMLProgressElement, HTMLMeterElement, HTMLTableElement, HTMLTableSectionElement,
    HTMLTableRowElement, HTMLTableCellElement, HTMLTableCaptionElement, HTMLTableColElement, HTMLDetailsElement, HTMLDialogElement, HTMLMenuElement,
    HTMLTimeElement, HTMLDataElement, HTMLModElement, HTMLMapElement, HTMLParamElement, HTMLFrameSetElement, HTMLFrameElement,
    setTimeout, setInterval, clearTimeout, clearInterval: clearTimeout, requestAnimationFrame, cancelAnimationFrame, requestIdleCallback,
    cancelIdleCallback: clearTimeout, queueMicrotask, fetch, atob, btoa, structuredClone, getComputedStyle, matchMedia, scrollTo, scroll: scrollTo,
    scrollBy(x, y) { if (x && typeof x === 'object') y = x.top; T.scrollTo(Math.max(0, T.scrollY() + (y | 0))); },
    alert(m) { T.alert(str(m)); }, confirm(m) { T.log(1, 'confirm(): ' + str(m) + ' -> true'); return true; }, prompt(m, d) { T.log(1, 'prompt(): ' + str(m)); return d === undefined ? '' : String(d); },
    print() {}, stop() {}, focus() {}, blur() {}, close() {}, moveTo() {}, resizeTo() {}, moveBy() {}, resizeBy() {},
    open(url) { if (url) T.navigate(String(url), false); return null; },
    getSelection: () => selection,
    postMessage(data, origin) { setTimeout(() => dispatch(G, new MessageEvent('message', { data: structuredCloneSafe(data), origin: location.origin, source: G })), 0); },
    reportError, find: () => false, captureEvents() {}, releaseEvents() {},
};
function structuredCloneSafe(d) { try { return structuredClone(d); } catch (e) { return d; } }
for (const [k, v] of Object.entries(windowProps)) {
    if (k === 'globalThis') continue;
    Object.defineProperty(G, k, { value: v, writable: true, configurable: true, enumerable: false });
}
Object.defineProperty(G, 'origin', { get: () => location.origin, configurable: true });
for (const [k, get] of [['innerWidth', () => T.vw()], ['innerHeight', () => T.vh()], ['outerWidth', () => T.vw()], ['outerHeight', () => T.vh() + 80],
    ['scrollX', () => scrollX], ['scrollY', () => T.scrollY()], ['pageXOffset', () => scrollX], ['pageYOffset', () => T.scrollY()],
    ['screenX', () => 0], ['screenY', () => 0], ['screenLeft', () => 0], ['screenTop', () => 0], ['event', () => undefined]])
    Object.defineProperty(G, k, { get, set(v) { Object.defineProperty(G, k, { value: v, writable: true, configurable: true }); }, configurable: true });
/* window's own events: listeners on a target of its own, on<event> as plain properties */
const winTarget = new EventTarget();
for (const m of ['addEventListener', 'removeEventListener']) Object.defineProperty(G, m, { value: (...a) => winTarget[m](...a), writable: true, configurable: true });
Object.defineProperty(G, 'dispatchEvent', { value: ev => dispatch(G, ev), writable: true, configurable: true });
Object.defineProperty(G, '__ls', { get: () => listeners(winTarget, true), configurable: true });
for (const n of EVENTS) if (!(('on' + n) in G)) Object.defineProperty(G, 'on' + n, { value: null, writable: true, configurable: true, enumerable: false });

/* unhandled promise rejections are reported like errors */
/* ------------------------------------------------------------------ what the engine calls */

const MOUSE = /^(click|dblclick|mousedown|mouseup|mousemove|mouseover|mouseout|mouseenter|mouseleave|contextmenu|auxclick)$/;
const POINTER = /^pointer/;
const KEY = /^key/;
function engineEvent(node, type, x, y, key, keyname) {
    let ev;
    const vy = y - T.scrollY();
    if (MOUSE.test(type)) ev = new MouseEvent(type, { bubbles: type !== 'mouseenter' && type !== 'mouseleave', cancelable: true, clientX: x, clientY: vy, button: 0, buttons: type === 'mousedown' ? 1 : 0, detail: 1, view: G });
    else if (POINTER.test(type)) ev = new PointerEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: vy, view: G });
    else if (KEY.test(type)) {
        const code = keyname.length === 1 ? (/[a-z]/i.test(keyname) ? 'Key' + keyname.toUpperCase() : /\d/.test(keyname) ? 'Digit' + keyname : keyname === ' ' ? 'Space' : '') : keyname;
        ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: keyname, code, keyCode: key, charCode: type === 'keypress' ? key : 0, view: G });
    }
    else if (type === 'focus' || type === 'blur') ev = new FocusEvent(type, { bubbles: false });
    else if (type === 'focusin' || type === 'focusout') ev = new FocusEvent(type, { bubbles: true });
    else if (type === 'input' || type === 'beforeinput') ev = new InputEvent(type, { bubbles: true, cancelable: type === 'beforeinput', data: keyname || null, inputType: key === 8 ? 'deleteContentBackward' : 'insertText' });
    else if (type === 'submit') ev = new SubmitEvent(type, { bubbles: true, cancelable: true, submitter: null });
    else if (type === 'load' || type === 'error') ev = new Event(type, { bubbles: false });
    else if (type === 'toggle' || type === 'close') ev = new Event(type, { bubbles: false });
    else if (type === '__waapi') { const a = waapi.get(x); if (a) a._done(); return false; }
    else if (/^animation/.test(type)) ev = new AnimationEvent(type, { bubbles: true, animationName: keyname, elapsedTime: key / 1000, pseudoElement: '' });
    else if (/^transition/.test(type)) ev = new TransitionEvent(type, { bubbles: true, propertyName: keyname, elapsedTime: key / 1000, pseudoElement: '' });
    else ev = new Event(type, { bubbles: true, cancelable: type !== 'change' });
    ev.isTrusted = true;
    dispatch(node, ev);
    return ev.defaultPrevented;
}

function windowEvent(type, onDocument) {
    if (type === 'DOMContentLoaded') { readyState = 'interactive'; dispatch(document, new Event('readystatechange')); }
    if (type === 'load') { readyState = 'complete'; dispatch(document, new Event('readystatechange')); }
    let ev;
    if (type === 'popstate') ev = new PopStateEvent(type, { state: histState });
    else if (type === 'load' || type === 'pageshow') ev = new Event(type);
    else ev = new Event(type, { bubbles: onDocument });
    ev.isTrusted = true;
    if (onDocument) dispatch(document, ev);
    else { ev.target = G; ev._path = [G]; ev.eventPhase = 2; invoke(G, ev, 2); }
}

function imageLoaded(img, ok) {
    const ev = new Event(ok ? 'load' : 'error');
    ev.isTrusted = true;
    dispatch(img, ev);
}

})();
