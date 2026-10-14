# Glossary {#glossary}

| Term | Meaning in Portals | Read next |
|---|---|---|
| Description / recipe | Owning single-use value describing controls to construct; not a mounted widget | [Declarative UI](declarative-ui.md) |
| Scene | The `ui::Scene` facade for recipe, setup, running and cleanup | [Declarative UI](declarative-ui.md) |
| ManagedScene | Scene-owned registrations, properties and models over the existing Application/tree | [Managed controls](managed-controls.md) |
| Ref | Observing typed handle; copying it does not keep a scene or widget alive | [Text properties](text-property.md) |
| Node | Checked tree identity with slot/generation; observes a mounted widget | [Widget tree](widget-tree.md) |
| Property | UI-owned typed value publishing immutable snapshots | [Properties](property.md) |
| ManagedState | Weak handle to a scene-owned Property | [Managed controls](managed-controls.md) |
| Snapshot | Owning immutable value/revision retained independently of later updates | [Properties](property.md) |
| Binding | Explicit connection between model/state and a view; ordinary text assignment copies a value | [Bindings guide](bindings-guide.md) |
| PSS | Portals stylesheet syntax; resembles CSS but has its own small grammar | [Styling guide](styling-guide.md) |
| Theme | A palette expressed as ordinary style rules, installed in the THEME tier | [Themes guide](themes-guide.md) |
| Theme token | Named palette value; typed theme parameters differ from unsupported PSS string variables | [Themes guide](themes-guide.md) |
| LiveTheme | Typed parameter controller which recomputes ordinary sheets using the existing resolver | [Compile-time styles](compile-time-styles.md) |
| Layer / tier | Independently installed sheet / its precedence class | [Style layers](style-layers.md) |
| Selector | Predicate over explicit type/id/classes/state metadata | [Styles](styles.md) |
| Invalidation | Request to recompute layout or paint; layout includes paint | [Properties](property.md) |
| UI owner thread | Thread on which scene/tree mutation and lifecycle operations are admitted | [Managed controls](managed-controls.md) |
| Diagnostic / final report | Operation record / report delivered after attempted cleanup | [Declarative UI](declarative-ui.md) |
| `$name` | **TBD — M5/U65/U128:** code-provided or observed parameter; not an automatic alias for a theme parameter | [Themes](themes-guide.md) |
| `theme.*` | **TBD — M5/U65/U128:** namespace of semantic PSS theme parameters | [Themes](themes-guide.md) |
| `this.text` / `len` | **TBD — M5/U128:** localized control text / its terminal-cell width under WidthPolicy, not bytes | [Styling](styling-guide.md) |
| Checked parameter assignment | **TBD — M5/U65/U128:** validate a candidate before atomic publication; optional policy must be explicit | [Themes](themes-guide.md) |
| TBD | Planned behavior or unfinished guide section; use the milestone/task table, not guessed syntax | [Guide contents](guides.md) |
| Milestone | Planned group/order of work, not a promised calendar release date | [Guide contents](guides.md) |

These definitions describe Portals contracts rather than a generic GUI framework.
