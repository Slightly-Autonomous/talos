# hardware

The Talos custom-hardware workstream. It follows `TALOS_CUSTOM_HARDWARE_HANDOFF_v0_1` (milestones H0–H7). Root ADRs in `docs/decisions/` still win over anything here (ADR 0017 links the two).

> **Nothing in this folder is a validated design.** The schematics pass ERC and the autorouted layouts pass DRC, but no electrical engineer has reviewed them, and nothing has been built or measured. **Do not order boards** until the review items in [layout/README.md](layout/README.md) are closed (HW-0005). Every value is `provisional: true` until a bench measurement says otherwise.

## Boards

| ID | What | Bespoke? | State |
|---|---|---|---|
| TL-HUB-REF | Hub: Pi 5 / mini-PC + USB 802.15.4 RCP | No, bought | `bom/devkit_rig_v0.csv` |
| TL-SN-001 | Four-slot Thread Sensor Node (ESP32-H2-MINI-1) | Yes | H3 schematic + H4 placed and routed 4-layer layout, 0 DRC errors, not yet reviewed ([layout](layout/README.md)) |
| TL-MOD-001 | Magnetic contact card (TMAG5273 3-axis Hall, any orientation) | Yes | H3 schematic + H4 routed 2-layer layout |
| TL-MOD-002 | PIR card (Panasonic EKMB 1 µA) | Yes | H3 schematic + H4 routed 2-layer layout |
| TL-MOD-003 | Vibration/impact card (ST LIS2DW12) | Yes | H3 schematic + H4 routed 2-layer layout |
| TL-MOD-004 | Temperature/humidity card (Sensirion SHT4x) | Yes | H3 schematic + H4 routed 2-layer layout |
| TL-TEST-001 | Slot test cartridge | Yes | H3 schematic + H4 routed 2-layer layout |
| TL-DBG-001 | One-slot USB debug adapter: one card on a PC (MCP2221A bridge, PPK2 meter headers) | Yes, bench tool | Schematic + routed 2-layer layout, 0 DRC errors ([HW-0007](architecture/decisions/0007-slot-socket-and-debug-adapter.md)) |
| TL-VN-001 | Vision Node (separate board, CM4/CM5/video SoC) | Yes | H5 study not started; manifest only |

The camera **slot** module (`smart_camera_s3_v1`) is superseded by TL-VN-001 ([ADR HW-0001](architecture/decisions/0001-hardware-family.md)).

## Where things are

| Path | Contents |
|---|---|
| `architecture/` | [hardware-overview.md](architecture/hardware-overview.md) and hardware ADRs HW-0001..0006 |
| `requirements/` | Requirements (`hardware-requirements.yaml`), decision/risk register, datasheet register, component selection, interface test matrix |
| `interfaces/` | [sensor_edge_v1](interfaces/sensor_edge_v1.md) slot connector contract |
| `pins/` | Complete ESP32-H2-MINI-1 pad ledger (CSV), the single source for the firmware `BoardSupport` table |
| `budgets/` | Per-slot and board resource budget (CSV) |
| `power/` | Power-tree study and operating-mode matrix (H2) |
| `mechanical/` | Connector shortlist and mechanical notes |
| `kicad/` | KiCad 10 projects, all **generated** by `scripts/gen_schematics.py`; project-local symbols in `kicad/libraries/` |
| `scripts/` | Schematic generator, ERC/netlist checks, power-budget calculator |
| `results/` | Captured tool output (ERC/DRC summary, netlist comparison, power report), dated and versioned |
| `layout/` | H4 layout method, board summary and the pre-order review list |
| `fab/` | Prototype fabrication packages (Gerbers, drill, CPL, BOM, renders), for quotes and DFM only |
| `templates/` | Component evaluation template |
| `bom/` | Dev-kit rig BOM (bring-up path, ADR 0016) |

Board and module manifests live in `contracts/manifests/` (one schema, validated by `core/manifest`). `sensor_node_proto_a.yaml` must agree with `pins/esp32h2_mini1_pin_ledger.csv` and `firmware/esp_idf/main/board_tl_sn_001.h`.

## Reproduce

```bash
# KiCad 10.0.x: kicad-cli on PATH, or set KICAD_CLI.
python3 hardware/scripts/gen_schematics.py          # regenerate all projects
python3 hardware/scripts/check_kicad.py --pdf      # ERC + netlist round-trip + PDFs -> hardware/results/
python3 hardware/scripts/power_budget.py             # H2 calculations -> hardware/results/power-budget.md
bazel test //core/manifest:manifest_test //codec:yaml_loader_test   # manifest validator (HW-MF tests)
```

## Hard rules
- Do not order parts or boards, request binding quotes, create factory keys or burn eFuses without explicit owner approval.
- Never hand-edit a generated `.kicad_sch`. Change the generator, regenerate, and re-run the checks.
- A green ERC is not "validated". See `requirements/design-risks.md` for what still needs a reviewer or a bench.

Vendor drawings marked proprietary (e.g. Samtec's HSEC8 prints) live in `hardware/vendor-docs/`, which git ignores. Re-download them from the vendor site; the register in `requirements/datasheet-register.md` names each one.
