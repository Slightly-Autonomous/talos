# `sensor_edge_v1`: Sensor Node slot contract (Rev A, proposed)

Twenty contacts, 0.80 mm pitch, 2 × 10. The card is a 1.60 mm PCB with gold fingers in a coplanar edge-mount socket ([HW-0002](../architecture/decisions/0002-sensor-module-interface.md)). Contact numbers follow KiCad `Conn_02x10_Odd_Even` (odd = row A, even = row B). The physical finger-to-socket mapping is fixed when the socket footprint is drawn (H4).

| # | Net (main side, slot *n*) | Dir | Rev A main board | Card obligation |
|---|---|---|---|---|
| 1 | GND | – | ground | ground |
| 2 | GND | – | ground | ground |
| 3 | V3V3_AON_n | main→card | TPS22917, default ON, ≤ 20 mA *(est)* | ID EEPROM and anything on the I²C branch |
| 4 | V3V3_SW_n | main→card | TPS22917, default OFF, QOD | higher-current sensing only, no I²C devices unless buffered |
| 5 | V5_SW (reserved) | main→card | **not connected** | leave unconnected |
| 6 | GND | – | ground | ground |
| 7 | SLOTn_SDA | ↔ | TCA9546A channel *n*, 4.7 k pull-up to AON_n | open-drain |
| 8 | SLOTn_SCL | main→card | TCA9546A channel *n*, 4.7 k pull-up to AON_n | open-drain |
| 9 | SLOT_WAKEn | card→main | GPIO10/11/12/13, 1 MΩ pull-down, TVS | push-pull, idle low preferred |
| 10 | SLOTn_ALERT_N | card→main | EXP_B input, 100 k pull-up to AON_n | open-drain, active low |
| 11 | SPI_SCK | main→card | shared GPIO4 | high-Z input |
| 12 | SPI_MOSI | main→card | shared GPIO5 | high-Z input |
| 13 | SPI_MISO | card→main | shared GPIO0 | **must tri-state when CS_N high** |
| 14 | SLOTn_CS_N | main→card | EXP_A output, 100 k pull-up to AON_n | active low |
| 15 | SLOTn_UART_TX | main→card | TS3A5017 1S*n* (UART1 TX GPIO22) | card RX |
| 16 | SLOTn_UART_RX | card→main | TS3A5017 2S*n* (UART1 RX GPIO25) | card TX |
| 17 | SLOTn_RESET_N | main→card | EXP_A output, 100 k pull-up to AON_n | optional |
| 18 | SLOTn_PRESENT_N | card→main | EXP_B input, 100 k pull-up to PRESENT_PROBE | **tie to GND on every card** |
| 19 | ANALOG_RSVD | – | **not connected** (HD-003) | leave unconnected |
| 20 | GND | – | ground | ground |

## Rules
- **S-1:** Every card has an ID EEPROM at 0x50 on AON. Card I²C addresses must not use the reserved root addresses 0x20, 0x21, 0x6A or 0x70.
- **S-2:** Devices on the branch I²C must be powered from AON, because the branch pull-ups go to AON. A device on SW needs an isolating buffer with I_off support.
- **S-3:** Firmware holds SPI_SCK/MOSI low and keeps the UART mux disabled (`UART_MUX_EN_N` high) whenever the addressed slot's rails are off.
- **S-4:** One card UART is active at a time (one UART1, one mux). The board manifest declares `uart_channels: 1`, and the validator rejects two UART cards that need concurrent operation.
- **S-5:** No hot-swap. Cards are inserted with the node off.
- **S-6:** Current per contact is ≤ 1 A design limit. The 2.3 A rating is for 2 pins powered *(src)*, so derate for plating and temperature at review.
