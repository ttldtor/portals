"""Exercise native scroll synchronization and fallback with Node's built-in VM.

Run with unittest discovery in tools/docs; no browser or third-party packages.
Rendered clipping and theme appearance require the browser checks as well.
"""
from pathlib import Path
import shutil
import subprocess
import unittest


NODE = shutil.which("node")
SCRIPT = Path(__file__).resolve().parents[2] / "docs/turbo-scrollbars.js"


@unittest.skipUnless(NODE, "Node.js is required for documentation JavaScript tests")
class TurboScrollbarTests(unittest.TestCase):
    def test_interactions_and_native_fallback(self):
        fixture = r'''
const {readFileSync} = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
class Element {
  constructor() {
    this.children = []; this.dataset = {}; this.style = {setProperty(k,v){this[k]=v;}, removeProperty(k){delete this[k];}}; this.attributes = {};
    this.listeners = {}; this.id = ""; this.lang = "en"; this.isConnected = true;
    this.clientLeft = this.clientTop = 0; this.clientWidth = 300; this.clientHeight = 400;
    this.scrollWidth = 300; this.scrollHeight = 1600; this.scrollTop = this.scrollLeft = 0;
    const classes = new Set();
    this.classList = {contains: v => classes.has(v) || this.className === v,
      add: (...v) => v.forEach(x => classes.add(x)), remove: (...v) => v.forEach(x => classes.delete(x))};
  }
  append(child) { this.children.push(child); child.parentElement = this; }
  remove() { this.parentElement.children = this.parentElement.children.filter(c => c !== this); }
  setAttribute(k, v) { this.attributes[k] = v; }
  removeAttribute(k) { if (k === "id") this.id = ""; delete this.attributes[k]; }
  addEventListener(k, fn) { this.listeners[k] = fn; }
  closest() { return null; }
  matches() { return false; }
  getClientRects() { return [this.getBoundingClientRect()]; }
  get offsetHeight() { return parseFloat(this.style.height) || 16; }
  get offsetWidth() { return parseFloat(this.style.width) || 16; }
  getBoundingClientRect() {
    if (this.className === "tv-scrollbar-track") {
      const p = this.parentElement.getBoundingClientRect();
      return {left: p.left, top: p.top + 16, width: p.width, height: p.height - 32};
    }
    const left = parseFloat(this.style.left) || 0, top = parseFloat(this.style.top) || 0;
    const width = parseFloat(this.style.width) || this.clientWidth;
    const height = parseFloat(this.style.height) || this.clientHeight;
    return {left, top, width, height, right: left + width, bottom: top + height};
  }
  focus() { this.focused = true; }
  setPointerCapture(id) { this.captured = id; }
}
const root = new Element(), body = new Element(), target = new Element();
body.append(target);
const document = {documentElement: root, body, readyState: "complete", fonts: null,
  querySelectorAll: () => [target], createElement: () => new Element(), addEventListener() {}};
const desktop = {matches: true, addEventListener(_, fn) {this.changed = fn;}},
      forced = {matches: false, addEventListener(_, fn) {this.changed = fn;}};
let pending = [];
const window = {innerWidth: 1000, innerHeight: 800, PointerEvent: function(){},
  matchMedia: q => q.includes("forced-colors") ? forced : desktop,
  requestAnimationFrame: fn => {pending.push(fn); return pending.length;}, addEventListener(){}};
class ResizeObserver { observe(){} unobserve(){} }
const mutations = [];
class MutationObserver { constructor(fn) {mutations.push(fn);} observe(){} }
window.ResizeObserver = ResizeObserver; window.MutationObserver = MutationObserver;
const getComputedStyle = element => ({overflowX: "auto", overflowY: "auto", paddingRight: "8px", paddingBottom: "8px",
  getPropertyValue: () => "1"});
function flush() { const batch = pending; pending = []; batch.forEach(fn => fn()); }
vm.runInNewContext(readFileSync(process.argv[1], "utf8"),
  {document, window, ResizeObserver, MutationObserver, getComputedStyle});
flush();
assert.equal(body.children.length, 1, "Normal themes must retain native scrolling");
root.classList.add("turbo-mode"); mutations[1](); flush();
assert(target.classList.contains("tv-scrollbars-enhanced"));
assert(target.id.startsWith("tv-scroll-region-"));
const box = target.children[0], track = box.children[1], thumb = track.children[0];
assert.equal(box.parentElement, target, "Wheel bubbling retains the native scroll ancestor");
assert.equal(thumb.attributes["aria-controls"], target.id);
assert.equal(thumb.attributes["aria-valuemax"], "1200");
function wheel(properties = {}) {
  let consumed = false;
  box.listeners.wheel({deltaX: 0, deltaY: 1, deltaMode: 0, ctrlKey: false, shiftKey: false,
    preventDefault(){consumed = true;}, ...properties}); flush();
  return consumed;
}
assert(wheel({deltaY: 100})); assert.equal(target.scrollTop, 100);
assert(wheel({deltaY: 2, deltaMode: 1})); assert.equal(target.scrollTop, 180);
assert(wheel({deltaY: 1, deltaMode: 2})); assert.equal(target.scrollTop, 580);
assert(!wheel({ctrlKey: true})); assert.equal(target.scrollTop, 580, "Ctrl-wheel retains browser zoom");
let prevented = false;
thumb.listeners.keydown({key: "End", preventDefault(){prevented = true;}}); flush();
assert(prevented); assert.equal(target.scrollTop, 1200);
assert.equal(thumb.attributes["aria-valuenow"], "1200");
assert(!wheel(), "An unconsumed wheel at the bound preserves default chaining");
thumb.listeners.keydown({key: "PageUp", preventDefault(){}}); flush();
assert.equal(target.scrollTop, 800);
box.children[0].listeners.click(); flush(); assert.equal(target.scrollTop, 760);
thumb.listeners.pointerdown({button: 0, pointerId: 7, clientY: 0, preventDefault(){}});
thumb.listeners.pointermove({pointerId: 7, clientY: 10000}); flush();
assert.equal(target.scrollTop, 1200, "Dragging is bounded by native range");
thumb.listeners.pointercancel();
thumb.listeners.pointermove({pointerId: 7, clientY: 0}); flush();
assert.equal(target.scrollTop, 1200, "Cancelled capture must stop changing scroll");
assert.equal(target.listeners.wheel, undefined, "Content wheel events are not intercepted");
forced.matches = true; forced.changed(); flush();
assert.equal(target.children.length, 0);
assert(!target.classList.contains("tv-scrollbars-enhanced")); assert.equal(target.id, "");
forced.matches = false; forced.changed(); flush(); assert.equal(target.children.length, 1);
desktop.matches = false; desktop.changed(); flush(); assert.equal(target.children.length, 0);
assert.equal(target.scrollTop, 1200, "Fallback preserves native scroll position");
desktop.matches = true; desktop.changed(); flush();
target.scrollWidth = 800; mutations[1](); flush();
assert.equal(target.children.length, 2, "Both overflowing axes receive controls");
target.scrollHeight = target.clientHeight; mutations[1](); flush();
assert.equal(target.children.length, 1, "A disappeared axis removes its rail");
assert(!target.classList.contains("tv-scrollbars-y"));
assert(target.classList.contains("tv-scrollbars-x"));
target.scrollWidth = target.clientWidth; mutations[1](); flush();
assert.equal(target.children.length, 0, "No overflow restores the native element completely");
assert(!target.classList.contains("tv-scrollbars-enhanced"));
assert.equal(target.style["--tv-original-padding-right"], undefined);
target.clientHeight = 48; target.scrollHeight = 1600; mutations[1](); flush();
assert.equal(target.children.length, 0, "Short scrolling regions retain native controls");
assert(!target.classList.contains("tv-scrollbars-enhanced"));
target.clientHeight = 400; mutations[1](); flush();
assert.equal(target.children.length, 1);
target.clientHeight = 48; target.scrollWidth = 800; mutations[1](); flush();
assert.equal(target.children.length, 0, "A short axis cannot disappear behind the other axis enhancement");
assert(!target.classList.contains("tv-scrollbars-enhanced"));
'''
        result = subprocess.run([NODE, "-e", fixture, str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
