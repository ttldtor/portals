/* Copyright (c) 2026 ttldtor.
 * SPDX-License-Identifier: BSL-1.0
 * Progressive desktop rails. The scrolling element remains the native owner.
 */
(() => {
  "use strict";
  const selector = "#doc-content, #nav-tree, .toc, div.fragment, pre, div.memproto, .diagram-variants";
  const thickness = 16;
  const minimumThumb = 24;
  const lineStep = 40;
  const root = document.documentElement;
  const desktop = window.matchMedia("(min-width: 768px) and (pointer: fine)");
  const forcedColors = window.matchMedia("(forced-colors: active)");
  const entries = new Map();
  let frame = 0;
  let discover = true;
  let sequence = 0;
  let observer;
  let resizeObserver;

  function schedule(scan = false) {
    discover ||= scan;
    if (!frame) frame = window.requestAnimationFrame(refresh);
  }

  function enabled() {
    return root.classList.contains("turbo-mode") && desktop.matches && !forcedColors.matches &&
      getComputedStyle(root).getPropertyValue("--tv-scrollbars-ready").trim() === "1";
  }

  function maximum(target, axis) {
    return Math.max(0, axis === "y" ? target.scrollHeight - target.clientHeight : target.scrollWidth - target.clientWidth);
  }

  function position(target, axis) { return axis === "y" ? target.scrollTop : target.scrollLeft; }
  function move(target, axis, value) {
    target[axis === "y" ? "scrollTop" : "scrollLeft"] = Math.max(0, Math.min(maximum(target, axis), value));
    schedule();
  }

  function rail(entry, axis) {
    const target = entry.target;
    const russian = (root.lang || "en").toLowerCase().startsWith("ru");
    const vertical = axis === "y";
    const box = document.createElement("div");
    box.className = "tv-scrollbar";
    box.dataset.axis = axis;
    /* Fixed descendants do not consistently inherit native wheel routing. */
    box.addEventListener("wheel", event => {
      if (event.ctrlKey) return;
      const oldX = target.scrollLeft;
      const oldY = target.scrollTop;
      let deltaX = event.deltaX;
      let deltaY = event.deltaY;
      if (event.shiftKey && !deltaX) { deltaX = deltaY; deltaY = 0; }
      if (axis === "x" && !maximum(target, "y") && !deltaX) { deltaX = deltaY; deltaY = 0; }
      const unitsX = event.deltaMode === 1 ? lineStep : event.deltaMode === 2 ? target.clientWidth : 1;
      const unitsY = event.deltaMode === 1 ? lineStep : event.deltaMode === 2 ? target.clientHeight : 1;
      move(target, "x", oldX + deltaX * unitsX);
      move(target, "y", oldY + deltaY * unitsY);
      if (target.scrollLeft !== oldX || target.scrollTop !== oldY) event.preventDefault();
    }, {passive: false});
    const track = document.createElement("div");
    track.className = "tv-scrollbar-track";
    const thumb = document.createElement("div");
    thumb.className = "tv-scrollbar-thumb";
    thumb.tabIndex = 0;
    thumb.setAttribute("role", "scrollbar");
    thumb.setAttribute("aria-controls", target.id);
    thumb.setAttribute("aria-orientation", vertical ? "vertical" : "horizontal");
    thumb.setAttribute("aria-valuemin", "0");
    thumb.setAttribute("aria-label", russian ? (vertical ? "Прокрутка по вертикали" : "Прокрутка по горизонтали") :
      (vertical ? "Vertical scrolling" : "Horizontal scrolling"));
    for (const direction of [-1, 1]) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "tv-scrollbar-arrow";
      button.dataset.direction = String(direction);
      button.setAttribute("aria-controls", target.id);
      button.setAttribute("aria-label", russian ?
        (vertical ? (direction < 0 ? "Прокрутить вверх" : "Прокрутить вниз") : (direction < 0 ? "Прокрутить влево" : "Прокрутить вправо")) :
        (vertical ? (direction < 0 ? "Scroll up" : "Scroll down") : (direction < 0 ? "Scroll left" : "Scroll right")));
      button.addEventListener("click", () => move(target, axis, position(target, axis) + direction * lineStep));
      box.append(button);
      if (direction < 0) box.append(track);
    }
    track.append(thumb);
    track.addEventListener("pointerdown", event => {
      if (event.target !== track || event.button !== 0) return;
      const bounds = thumb.getBoundingClientRect();
      const before = vertical ? event.clientY < bounds.top : event.clientX < bounds.left;
      move(target, axis, position(target, axis) + (before ? -1 : 1) * (vertical ? target.clientHeight : target.clientWidth));
      thumb.focus({preventScroll: true});
    });
    let drag = null;
    thumb.addEventListener("pointerdown", event => {
      if (event.button !== 0) return;
      const bounds = thumb.getBoundingClientRect();
      drag = {id: event.pointerId, offset: vertical ? event.clientY - bounds.top : event.clientX - bounds.left};
      thumb.setPointerCapture(event.pointerId);
      event.preventDefault();
      thumb.focus({preventScroll: true});
    });
    thumb.addEventListener("pointermove", event => {
      if (!drag || drag.id !== event.pointerId) return;
      const bounds = track.getBoundingClientRect();
      const extent = vertical ? bounds.height : bounds.width;
      const size = vertical ? thumb.offsetHeight : thumb.offsetWidth;
      const coordinate = vertical ? event.clientY - bounds.top : event.clientX - bounds.left;
      if (extent > size) move(target, axis, (coordinate - drag.offset) / (extent - size) * maximum(target, axis));
    });
    const endDrag = () => { drag = null; };
    thumb.addEventListener("pointerup", endDrag);
    thumb.addEventListener("pointercancel", endDrag);
    thumb.addEventListener("lostpointercapture", endDrag);
    thumb.addEventListener("keydown", event => {
      const page = vertical ? target.clientHeight : target.clientWidth;
      const steps = {ArrowUp: -lineStep, ArrowLeft: -lineStep, ArrowDown: lineStep, ArrowRight: lineStep,
        PageUp: -page, PageDown: page};
      if (!(event.key in steps) && event.key !== "Home" && event.key !== "End") return;
      event.preventDefault();
      move(target, axis, event.key === "Home" ? 0 : event.key === "End" ? maximum(target, axis) : position(target, axis) + steps[event.key]);
    });
    target.append(box);
    return {box, track, thumb};
  }

  function remove(entry) {
    for (const item of Object.values(entry.rails)) item.box.remove();
    entry.target.classList.remove("tv-scrollbars-enhanced", "tv-scrollbars-x", "tv-scrollbars-y");
    entry.target.style.removeProperty("--tv-original-padding-right");
    entry.target.style.removeProperty("--tv-original-padding-bottom");
    if (entry.generatedId && entry.target.id === entry.generatedId) entry.target.removeAttribute("id");
    resizeObserver.unobserve(entry.target);
    entries.delete(entry.target);
  }

  function clippingBounds(target) {
    const bounds = {left: 0, top: 0, right: window.innerWidth, bottom: window.innerHeight};
    for (let parent = target.parentElement; parent && parent !== document.body; parent = parent.parentElement) {
      const style = getComputedStyle(parent);
      const rect = parent.getBoundingClientRect();
      if (/(auto|scroll|hidden|clip)/.test(style.overflowX)) {
        bounds.left = Math.max(bounds.left, rect.left + parent.clientLeft);
        bounds.right = Math.min(bounds.right, rect.left + parent.clientLeft + parent.clientWidth);
      }
      if (/(auto|scroll|hidden|clip)/.test(style.overflowY)) {
        bounds.top = Math.max(bounds.top, rect.top + parent.clientTop);
        bounds.bottom = Math.min(bounds.bottom, rect.top + parent.clientTop + parent.clientHeight);
      }
    }
    return bounds;
  }

  function update(entry) {
    const target = entry.target;
    const bounds = target.getBoundingClientRect();
    const clip = clippingBounds(target);
    const left = bounds.left + target.clientLeft;
    const top = bounds.top + target.clientTop;
    const right = left + target.clientWidth;
    const bottom = top + target.clientHeight;
    for (const [axis, item] of Object.entries(entry.rails)) {
      const vertical = axis === "y";
      const max = maximum(target, axis);
      const start = Math.max(vertical ? top : left, vertical ? clip.top : clip.left);
      const end = Math.min(vertical ? bottom : right, vertical ? clip.bottom : clip.right) -
        (entry.rails[vertical ? "x" : "y"] ? thickness : 0);
      const edge = vertical ? right : bottom;
      const edgeVisible = vertical ? edge <= clip.right && edge - thickness >= clip.left : edge <= clip.bottom && edge - thickness >= clip.top;
      const visible = max > 0 && edgeVisible && end - start > thickness * 3 && target.getClientRects().length > 0;
      item.box.hidden = !visible;
      if (!visible) continue;
      Object.assign(item.box.style, vertical ? {left: `${right - thickness}px`, top: `${start}px`, width: `${thickness}px`, height: `${end - start}px`} :
        {left: `${start}px`, top: `${bottom - thickness}px`, width: `${end - start}px`, height: `${thickness}px`});
      const trackLength = end - start - thickness * 2;
      const viewport = vertical ? target.clientHeight : target.clientWidth;
      const size = Math.min(trackLength, Math.max(minimumThumb, trackLength * viewport / (viewport + max)));
      const offset = Math.max(0, Math.min(max, position(target, axis))) / max * (trackLength - size);
      Object.assign(item.thumb.style, vertical ? {height: `${size}px`, top: `${offset}px`} : {width: `${size}px`, left: `${offset}px`});
      item.thumb.setAttribute("aria-valuemax", String(Math.round(max)));
      item.thumb.setAttribute("aria-valuenow", String(Math.round(position(target, axis))));
    }
  }

  function refresh() {
    frame = 0;
    if (!enabled()) {
      for (const entry of entries.values()) remove(entry);
      discover = true;
      return;
    }
    if (discover) {
      discover = false;
      for (const target of document.querySelectorAll(selector)) {
        if (target.matches("pre") && target.closest("div.fragment")) continue;
        const style = getComputedStyle(target);
        const axes = ["x", "y"].filter(axis => /auto|scroll/.test(axis === "y" ? style.overflowY : style.overflowX) && maximum(target, axis) > 1);
        const existing = entries.get(target);
        /* Firefox hides both native axes together. Keep native controls if any
         * overflowing axis cannot fit its arrows and a usable thumb track. */
        const usable = target.getClientRects().length > 0 && axes.every(axis => {
          const extent = axis === "y" ? target.clientHeight : target.clientWidth;
          const cross = axis === "y" ? target.clientWidth : target.clientHeight;
          return extent - (axes.length > 1 ? thickness : 0) > thickness * 3 && cross >= thickness;
        });
        if (!axes.length || !usable) {
          if (existing) remove(existing);
          continue;
        }
        if (existing) {
          for (const axis of Object.keys(existing.rails)) if (!axes.includes(axis)) {
            existing.rails[axis].box.remove();
            delete existing.rails[axis];
            target.classList.remove(`tv-scrollbars-${axis}`);
          }
          for (const axis of axes) if (!existing.rails[axis]) {
            existing.rails[axis] = rail(existing, axis);
            target.classList.add(`tv-scrollbars-${axis}`);
          }
          continue;
        }
        const entry = {target, rails: {}, generatedId: target.id ? null : `tv-scroll-region-${++sequence}`};
        if (entry.generatedId) target.id = entry.generatedId;
        try {
          for (const axis of axes) entry.rails[axis] = rail(entry, axis);
          target.style.setProperty("--tv-original-padding-right", style.paddingRight);
          target.style.setProperty("--tv-original-padding-bottom", style.paddingBottom);
          target.classList.add("tv-scrollbars-enhanced");
          for (const axis of axes) target.classList.add(`tv-scrollbars-${axis}`);
          entries.set(target, entry);
          resizeObserver.observe(target);
        } catch (_) { remove(entry); }
      }
    }
    for (const entry of entries.values()) {
      if (!entry.target.isConnected) remove(entry);
      else update(entry);
    }
  }

  function initialize() {
    if (!window.ResizeObserver || !window.PointerEvent || !window.MutationObserver) return;
    resizeObserver = new ResizeObserver(() => schedule(true));
    observer = new MutationObserver(records => {
      if (records.some(record => !record.target.closest?.(".tv-scrollbar") &&
          (record.type !== "childList" || [...record.addedNodes, ...record.removedNodes].some(node => !node.classList?.contains("tv-scrollbar"))))) schedule(true);
    });
    observer.observe(document.body, {subtree: true, childList: true, characterData: true,
      attributes: true, attributeFilter: ["class", "style", "hidden"]});
    new MutationObserver(() => schedule(true)).observe(root, {attributes: true, attributeFilter: ["class"]});
    document.addEventListener("scroll", () => schedule(), {capture: true, passive: true});
    document.addEventListener("load", () => schedule(true), {capture: true, passive: true});
    window.addEventListener("resize", () => schedule(true), {passive: true});
    desktop.addEventListener("change", () => schedule(true));
    forcedColors.addEventListener("change", () => schedule(true));
    document.fonts?.ready.then(() => schedule(true));
    schedule(true);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", initialize, {once: true});
  else initialize();
})();
