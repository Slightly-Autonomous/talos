# Manufacturing release package (spec §46-50)

A hardware release is a tagged git commit that contains or produces:

| Item | Source | Check |
|---|---|---|
| KiCad schematic + PCB | `hardware/mainboard/` (future) | ERC/DRC clean in CI |
| Gerbers, drill, stackup | KiCad export | regenerated from tag |
| BOM with MPNs + alternates | `hardware/bom/*.csv` | every line has a datasheet link and stock check |
| Placement (CPL) + assembly drawing | KiCad export | top-side only (spec D06) |
| Board manifest | `contracts/manifests/boards/*.yaml` | matches schematic net names; `provisional: false` only for verified values |
| Firmware factory image + test image | `firmware/esp_idf` build | hashes recorded in the release |
| Factory test procedure | `docs/manufacturing/factory_test.md` (future) | pogo fixture pinout matches test pads |
| Identity provisioning spec | spec §37, §48 | no owner Wi-Fi/Thread secrets; no signing roots at the assembler |

## Factory flow (target)
1. Assemble (turnkey PCBA). Flash the **test image** through pogo pins.
2. Test rails, the four slot connectors (a loopback module per slot), the radio, the charge path, and current in sleep.
3. Flash the **release image**. Write the unique device identity and claim secret through a controlled provisioning step.
4. Print the QR label (non-secret id + claim data). Record per-serial test results.
5. The unit ships **unclaimed**, ready to pair and receive OTA (ADR 0013/0014).

## Vendor RFQ fields
Quantity tiers, turnkey parts sourcing, functional test and fixture cost, programming service, per-serial test reports, lead time, alternates policy and box-build option. Quote JLCPCB, PCBWay and Seeed like-for-like (spec §46).
