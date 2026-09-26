# Void Linux native rebuild: Wayfinder entry point

Canonical map: [Rebuild Leo for Void Linux with complete desktop feature parity](https://github.com/soubarnak/leo/issues/1).

Published 2026-09-26 after GitHub access was restored. Issues were disabled on the
repository and were enabled for the chosen tracker. The map has five native
child issues and four native blocking edges. This file is a local entry point;
questions and resolutions live in the GitHub tickets.

## Confirmed scope

- Native desktop UI, no Electron or embedded webview.
- Void Linux x86_64/glibc on Wayland/niri first.
- All current desktop features, existing library files and the writing experience
  are required; native controls and Linux shortcuts may differ.
- This phase is research and implementation planning. Implementation and release
  follow after the decisions are settled.

## Decision tickets

| Ticket | Type | Prerequisites |
| --- | --- | --- |
| [Compare native editor stacks against Leo's document semantics](https://github.com/soubarnak/leo/issues/2) | Research | None |
| [Establish the Void packaging and desktop integration contract](https://github.com/soubarnak/leo/issues/3) | Research | None |
| [Settle library compatibility, durability and Pocket coexistence](https://github.com/soubarnak/leo/issues/4) | Human discussion | None |
| [Validate native editing and choose the stack](https://github.com/soubarnak/leo/issues/5) | Prototype and human feedback | Both research tickets and library compatibility |
| [Agree on the parity acceptance matrix and implementation sequence](https://github.com/soubarnak/leo/issues/6) | Human discussion | Native editing validation and stack choice |

Query GitHub for current ticket state and assignees. Do not infer the live frontier
from this static table.

## Research assets

- [Existing features and data semantics](../research/void-native-parity.md)
- [Native toolkit comparison](../research/void-native-stack.md)
- [Void packaging and desktop integration](../research/void-native-integration.md)

The research assets are published on `research/void-native`. Resolution comments
link immutable commits. The final toolkit decision requires the native editor
proof and human feedback; a documented toolkit capability does not prove parity.

## Resumption

Use Wayfinder with the canonical map URL. Claim the first open, unblocked,
unassigned child before work. Follow native dependencies; do not begin the
prototype until its prerequisites are settled. A charting session resolves
research only, leaving human decisions for subsequent sessions.

The earlier shell/DNS and browser denials are historical access failures.
Current evidence and remaining technical uncertainties belong in the research
reports and ticket resolutions, not a duplicated decision record here.
