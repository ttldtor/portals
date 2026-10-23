/* Copyright (c) 2026 ttldtor.
 * SPDX-License-Identifier: BSL-1.0
 * Generated HTML loads this before rendering. It selects the local CSS scheme
 * and connects the appearance button; unavailable browser storage is optional.
 */
(() => {
  const storageKey = "portals-color-scheme";
  const schemes = ["system", "light", "dark", "turbo", "turbo2"];
  const icons = {
    system: '<circle cx="10" cy="10" r="7"/><path d="M10 3a7 7 0 0 0 0 14Z" fill="currentColor"/>',
    light: '<circle cx="10" cy="10" r="4"/><path d="M10 0v3m0 14v3M0 10h3m14 0h3M3 3l2 2m10 10 2 2M3 17l2-2M15 5l2-2"/>',
    dark: '<path d="M15 15A7 7 0 0 1 5 5a7 7 0 1 0 10 10Z"/>',
    turbo: '<path d="M2 3h16v12H2zM6 18h8M10 15v3M5 7l3 2-3 2M10 11h4"/>',
    turbo2: '<path d="M1 3h18v14H1zM1 7h18M5 3v4M4 11h5v3H4zM12 11h4m-4 3h4"/>'
  };
  const system = window.matchMedia("(prefers-color-scheme: dark)");
  let selected = "system";

  try {
    const saved = window.localStorage.getItem(storageKey);
    if (schemes.includes(saved)) selected = saved;
  } catch {
    // Local files and privacy settings may disable persistent storage.
  }

  const apply = () => {
    const dark = selected === "dark" || (selected === "system" && system.matches);
    document.documentElement.classList.toggle("dark-mode", dark);
    document.documentElement.classList.toggle("light-mode", !dark);
    document.documentElement.classList.toggle("turbo-mode", selected === "turbo" || selected === "turbo2");
    document.documentElement.classList.toggle("turbo2-mode", selected === "turbo2");
  };

  apply();
  system.addEventListener("change", apply);
  document.addEventListener("DOMContentLoaded", () => {
    // Doxygen resizes the tree itself. Keep the header and fixed footer aligned
    // with its content width without changing the stock schemes' geometry.
    const sideNav = document.querySelector("#side-nav");
    if (sideNav && typeof ResizeObserver !== "undefined") {
      new ResizeObserver(entries => {
        const width = entries[0].contentRect.width;
        if (width > 0) {
          document.documentElement.style.setProperty("--turbo-sidebar-width", `${width}px`);
        }
      }).observe(sideNav);
    }
    // Diagrams may use the central panel's spare width without scaling text.
    const content = document.querySelector("#doc-content");
    const diagramPanelGutter = 32;
    if (content && typeof ResizeObserver !== "undefined") {
      new ResizeObserver(entries => {
        const width = Math.max(1, entries[0].contentRect.width - diagramPanelGutter);
        document.documentElement.style.setProperty("--turbo-diagram-available-width", `${width}px`);
      }).observe(content);
    }
    // Doxygen creates disclosure links lazily. Keep its expansion mechanism,
    // adding the accessible state and Space activation to each current link.
    const tree = document.querySelector("#nav-tree");
    if (tree) {
      const bound = new WeakSet();
      const syncDisclosures = () => {
        tree.querySelectorAll(".item").forEach(item => {
          const toggle = item.querySelector(":scope > a");
          const arrow = toggle?.querySelector(".arrowhead");
          const label = item.querySelector(":scope > .label > a")?.textContent.trim();
          if (!arrow || !label) return;
          toggle.setAttribute("role", "button");
          toggle.setAttribute("aria-label", label);
          toggle.setAttribute("aria-expanded", String(arrow.classList.contains("opened")));
          if (!bound.has(toggle)) {
            toggle.addEventListener("keydown", event => {
              if (event.key === " ") {
                event.preventDefault();
                if (!event.repeat) toggle.click();
              }
            });
            bound.add(toggle);
          }
        });
      };
      syncDisclosures();
      new MutationObserver(syncDisclosures).observe(tree, {
        subtree: true, childList: true, attributes: true, attributeFilter: ["class"]
      });
    }

    if (tree) {
      // Generated overload labels carry the original signature, including defaults.
      const overloadTooltip = document.createElement("div");
      overloadTooltip.className = "portals-overload-tooltip";
      overloadTooltip.id = "portals-overload-tooltip";
      overloadTooltip.role = "tooltip";
      overloadTooltip.hidden = true;
      document.body.appendChild(overloadTooltip);
      let tooltipAnchor = null;
      let focusedLabel = null;
      let hoveredLabel = null;
      let dismissed = false;
      const hideOverload = () => {
        tooltipAnchor?.removeAttribute("aria-describedby");
        tooltipAnchor = null;
        overloadTooltip.hidden = true;
      };
      const overloadLabel = target => target?.closest?.("[data-overload-signature]")
        || target?.querySelector?.("[data-overload-signature]");
      const showOverload = () => {
        const label = hoveredLabel || focusedLabel;
        const anchor = label?.closest("a");
        hideOverload();
        if (!anchor || dismissed) return;
        tooltipAnchor = anchor;
        overloadTooltip.textContent = label.dataset.overloadSignature;
        anchor.setAttribute("aria-describedby", overloadTooltip.id);
        const margin = 8;
        // Measure from a stable origin, independent of the previous link's indent.
        overloadTooltip.style.left = `${margin}px`;
        overloadTooltip.style.top = `${margin}px`;
        overloadTooltip.hidden = false;
        const bounds = anchor.getBoundingClientRect();
        const size = overloadTooltip.getBoundingClientRect();
        overloadTooltip.style.left = `${Math.max(margin, Math.min(bounds.left, window.innerWidth - size.width - margin))}px`;
        overloadTooltip.style.top = `${Math.max(margin, Math.min(bounds.bottom + margin, window.innerHeight - size.height - margin))}px`;
      };
      const prepareOverloadLabels = () => {
        document.querySelectorAll("#nav-tree [data-overload-signature]").forEach(label => {
          const anchor = label.closest("a");
          anchor.setAttribute("aria-label", label.dataset.overloadSignature);
          anchor.title = label.dataset.overloadSignature;
        });
      };
      prepareOverloadLabels();
      new MutationObserver(prepareOverloadLabels).observe(tree, {subtree: true, childList: true});
      tree.addEventListener("focusin", event => {
        focusedLabel = overloadLabel(event.target);
        dismissed = false;
        showOverload();
      });
      tree.addEventListener("focusout", event => {
        focusedLabel = overloadLabel(event.relatedTarget);
        showOverload();
      });
      tree.addEventListener("mouseover", event => {
        const label = overloadLabel(event.target);
        if (label !== hoveredLabel) dismissed = false;
        hoveredLabel = label;
        showOverload();
      });
      tree.addEventListener("mouseout", event => {
        hoveredLabel = overloadLabel(event.relatedTarget);
        showOverload();
      });
      const dismissOverload = () => {
        dismissed = true;
        hideOverload();
      };
      tree.addEventListener("scroll", dismissOverload, true);
      document.addEventListener("keydown", event => {
        if (event.key === "Escape") dismissOverload();
      });
      window.addEventListener("resize", dismissOverload);
    }

    // These retain Doxygen's filter selector and close/reset handlers.
    const russian = document.documentElement.lang === "ru";
    const searchControls = [
      ["#MSearchSelect", russian ? "Категория поиска" : "Search category"],
      ["#MSearchClose", russian ? "Закрыть результаты поиска" : "Close search results"]
    ];
    searchControls.forEach(([selector, label]) => {
      const control = document.querySelector(selector);
      if (!control) return;
      control.setAttribute("aria-label", label);
      control.title = label;
    });

    const button = document.querySelector(".portals-appearance");
    if (!button) return;
    const updateButton = () => {
      const next = schemes[(schemes.indexOf(selected) + 1) % schemes.length];
      const label = `${button.dataset.label}: ${button.dataset[selected]}. ${button.dataset.nextLabel}: ${button.dataset[next]}.`;
      button.title = label;
      button.setAttribute("aria-label", label);
      button.dataset.scheme = selected;
      button.innerHTML = '<svg viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.5" aria-hidden="true" focusable="false">'
        + icons[selected] + '</svg>';
      button.hidden = false;
    };
    updateButton();
    button.addEventListener("click", () => {
      selected = schemes[(schemes.indexOf(selected) + 1) % schemes.length];
      apply();
      updateButton();
      try {
        window.localStorage.setItem(storageKey, selected);
      } catch {
        // The selection still applies to the current page without persistence.
      }
    });
  });
})();
