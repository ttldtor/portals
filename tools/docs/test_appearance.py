"""Test generated documentation appearance behavior without a renderer.

Run with unittest discovery in tools/docs. Node's built-in VM supplies a small
DOM/storage fixture; no browser, network, third-party modules or output files.
"""

from pathlib import Path
import shutil
import subprocess
import unittest


NODE = shutil.which("node")
SCRIPT = Path(__file__).resolve().parents[2] / "docs/appearance.js"


@unittest.skipUnless(NODE, "Node.js is required for documentation JavaScript tests")
class AppearanceTests(unittest.TestCase):
    def test_disclosures_track_lazy_nodes_and_space_activation(self):
        fixture = r'''
const { readFileSync } = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
let ready, changed, observed;
const nodes = [];
function node(label) {
  let opened = false;
  const attributes = {};
  const handlers = [];
  const arrow = { classList: { contains: value => value === "opened" && opened } };
  const toggle = {
    querySelector: () => arrow,
    setAttribute: (name, value) => attributes[name] = value,
    addEventListener: (_, handler) => handlers.push(handler),
    click: () => { opened = !opened; }
  };
  nodes.push({ querySelector: selector => selector === ":scope > a" ? toggle : { textContent: label } });
  return {attributes, handlers, toggle};
}
const initial = node("Start here");
const tree = { querySelectorAll: () => nodes };
const document = {
  documentElement: { classList: { toggle() {} } },
  querySelector: selector => selector === "#nav-tree" ? tree : null,
  addEventListener: (_, fn) => ready = fn
};
class MutationObserver {
  constructor(fn) { changed = fn; }
  observe(target, options) { observed = {target,options}; }
}
const window = {matchMedia: () => ({matches:false,addEventListener(){}}), localStorage:{getItem(){return null;}}};
vm.runInNewContext(readFileSync(process.argv[1], "utf8"), {document,window,MutationObserver});
ready();
assert.equal(initial.attributes.role, "button");
assert.equal(initial.attributes["aria-label"], "Start here");
assert.equal(initial.attributes["aria-expanded"], "false");
let prevented = false;
initial.handlers[0]({key:" ",repeat:false,preventDefault(){prevented=true;}});
changed();
assert(prevented);
assert.equal(initial.attributes["aria-expanded"], "true");
initial.handlers[0]({key:" ",repeat:true,preventDefault(){}});
changed();
assert.equal(initial.attributes["aria-expanded"], "true");
assert.equal(initial.handlers.length, 1);
const lazy = node("API reference");
changed();
assert.equal(lazy.attributes["aria-label"], "API reference");
assert.equal(lazy.handlers.length, 1);
assert.equal(observed.target, tree);
assert.equal(observed.options.attributeFilter.join(","), "class");
'''
        result = subprocess.run([NODE, "-e", fixture, str(SCRIPT)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_search_labels_preserve_existing_controls_and_handlers(self):
        fixture = r'''
const { readFileSync } = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
for (const lang of ["en", "ru"]) {
  let ready;
  let closed = false;
  const close = {
    attributes: {},
    onclick() { closed = true; },
    setAttribute(name, value) { this.attributes[name] = value; }
  };
  const category = { attributes: {}, setAttribute(name, value) { this.attributes[name] = value; } };
  const controls = {"#MSearchSelect": category, "#MSearchClose": close};
  const originalClose = close.onclick;
  const document = {
    documentElement: { lang, classList: { toggle() {} } },
    querySelector: selector => controls[selector] || null,
    addEventListener: (_, fn) => ready = fn
  };
  const window = { matchMedia: () => ({matches:false, addEventListener(){}}),
                   localStorage: {getItem(){return null;}} };
  vm.runInNewContext(readFileSync(process.argv[1], "utf8"), {document, window});
  ready();
  assert.equal(category.attributes["aria-label"], lang === "ru" ? "Категория поиска" : "Search category");
  assert.equal(close.attributes["aria-label"], lang === "ru" ? "Закрыть результаты поиска" : "Close search results");
  assert.equal(category.title, category.attributes["aria-label"]);
  assert.equal(close.title, close.attributes["aria-label"]);
  assert.equal(close.onclick, originalClose);
  close.onclick();
  assert(closed);
}
'''
        result = subprocess.run([NODE, "-e", fixture, str(SCRIPT)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_button_cycle_storage_failure_labels_and_system_preference(self):
        fixture = r'''
const { readFileSync } = require("node:fs");
const vm = require("node:vm");
const assert = require("node:assert/strict");
const source = readFileSync(process.argv[1], "utf8");
function page(saved, unavailable = false) {
  const classes = new Set();
  const listeners = {};
  const writes = [];
  const button = {
    hidden: true,
    attributes: {},
    dataset: { label: "Color scheme", nextLabel: "Next", system: "System", light: "Light", dark: "Dark", turbo: "Turbo Vision", turbo2: "Turbo2" },
    addEventListener: (name, fn) => listeners[name] = fn,
    setAttribute(name, value) { this.attributes[name] = value; }
  };
  const media = { matches: true, addEventListener: (_, fn) => listeners.system = fn };
  const storage = {
    getItem() { if (unavailable) throw Error("denied"); return saved; },
    setItem(key, value) { if (unavailable) throw Error("denied"); writes.push([key, value]); }
  };
  const document = {
    documentElement: { classList: { toggle: (key, value) => value ? classes.add(key) : classes.delete(key) } },
    querySelector: selector => selector === ".portals-appearance" ? button : null,
    addEventListener: (_, fn) => listeners.ready = fn
  };
  vm.runInNewContext(source, { document, window: { localStorage: storage, matchMedia: () => media } });
  listeners.ready();
  return { classes, listeners, button, media, writes };
}
const system = page(null);
assert(system.classes.has("dark-mode"));
assert(!system.button.hidden);
assert.equal(system.button.dataset.scheme, "system");
assert.equal(system.button.attributes["aria-label"], "Color scheme: System. Next: Light.");
assert(system.button.innerHTML.includes('aria-hidden="true"'));
system.media.matches = false;
system.listeners.system();
assert(system.classes.has("light-mode"));
const light = page("light");
assert.equal(light.button.dataset.scheme, "light");
assert(light.classes.has("light-mode"));
light.listeners.click();
assert(light.classes.has("dark-mode"));
assert(!light.classes.has("light-mode"));
assert.equal(light.button.dataset.scheme, "dark");
assert.equal(light.button.title, "Color scheme: Dark. Next: Turbo Vision.");
assert.deepEqual(light.writes, [["portals-color-scheme", "dark"]]);
light.media.matches = false;
light.listeners.system();
assert(light.classes.has("dark-mode"));
light.listeners.click();
assert.equal(light.button.dataset.scheme, "turbo");
assert(light.classes.has("turbo-mode"));
assert(light.classes.has("light-mode"));
assert(!light.classes.has("dark-mode"));
assert.equal(light.button.title, "Color scheme: Turbo Vision. Next: Turbo2.");
light.media.matches = true;
light.listeners.system();
assert(light.classes.has("turbo-mode"));
assert(!light.classes.has("dark-mode"));
light.media.matches = false;
light.listeners.click();
assert.equal(light.button.dataset.scheme, "turbo2");
assert(light.classes.has("turbo-mode"));
assert(light.classes.has("turbo2-mode"));
assert(!light.classes.has("dark-mode"));
assert.equal(light.button.title, "Color scheme: Turbo2. Next: System.");
light.listeners.click();
assert.equal(light.button.dataset.scheme, "system");
assert(!light.classes.has("turbo-mode"));
assert(!light.classes.has("turbo2-mode"));
assert(light.classes.has("light-mode"));
light.listeners.click();
assert.equal(light.button.dataset.scheme, "light");
const blocked = page(null, true);
blocked.listeners.click();
assert(blocked.classes.has("light-mode"));
assert.equal(blocked.button.dataset.scheme, "light");
assert.equal(page("invalid").button.dataset.scheme, "system");
const restored = page("turbo");
assert(restored.classes.has("turbo-mode"));
assert.equal(restored.button.dataset.scheme, "turbo");
assert(restored.button.innerHTML.includes('<path d="M2 3h16'));
const second = page("turbo2");
assert(second.classes.has("turbo-mode"));
assert(second.classes.has("turbo2-mode"));
assert.equal(second.button.dataset.scheme, "turbo2");
console.log("appearance scenarios passed");
'''
        result = subprocess.run([NODE, "-e", fixture, str(SCRIPT)], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
