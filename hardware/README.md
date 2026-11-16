# hardware

Nothing in this folder is a validated design.

- `bom/`: provisional bill of materials for the first dev-kit rig and the future mainboard. **Candidate parts only.**
- KiCad sources, connector spec, enclosures and fixtures land here after the dev-kit bring-up (docs/hardware/bringup.md) answers the open P0 questions in `docs/decisions/open-decisions.md`.

The board manifest (`contracts/manifests/boards/`) and the firmware `BoardSupport` table (`firmware/esp_idf/main/board_proto_a.h`) must describe the same wiring. Both stay `provisional` until a schematic exists.
