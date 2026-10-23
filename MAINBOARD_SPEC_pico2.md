# Main-CPU-Board-Spec — „Pico 2 HSTX Console"

**Stand:** 06.10.2026 · **Status:** aktiv (Lochraster-Board gelötet, Test läuft)
**Hardware:** Raspberry Pi Pico 2 (RP2350, **non-W** — bewusste Entscheidung: kein CYW43 = kein HSTX-Konflikt, Beweis siehe Backplane-Spec)
**Rolle:** Haupt-CPU — Video (HDMI/HSTX), Tastatur (PS/2), SD-Karte, BASIC-Konsole; alle weiteren Funktionen über Backplane-Boards

---

## Identität & Stammbaum

- **Firmware-Stamm A (Workshop):** tb-c (PicoLibSDK) — ATEXT 80×30, BASIC (tbtest 22/22), FAT16 (20/20), Workshop-Beispiele 8 Stück + Anleitung (tbanl 11/11)
- **Firmware-Stamm B (Experimentier):** retro-pico2350 (pico-sdk, von Claude Code gebaut) — PIO-PS/2, no-OS-FatFS, WiFi-Versuche (dokumentierter HSTX-Konflikt → führte zur Arch-Entscheidung)
- Beide Stämme werden weitergeführt — A = Workshop-Board, B = Labor

## Pin-Zuordnung (FINAL, Firmware-Stand c421028)

| Funktion | GPIO | Beweis/Anmerkung |
|---|---|---|
| PS/2 Tastatur DATA | GP2 | SIO-IRQ, bewiesen am Breadboard (MOSI-Port-Falle!) |
| PS/2 Tastatur CLOCK | GP3 | SIO-IRQ, bewiesen |
| PS/2 Maus DATA (Optional, Reserve) | GP4 | Treiber später |
| PS/2 Maus CLOCK (Optional, Reserve) | GP5 | Treiber später |
| **I2C1 SDA = Backplane-Bus** | GP6 | funcsel 3 (i2c1) — frei bewiesen |
| **I2C1 SCL = Backplane-Bus** | GP7 | funcsel 3 — frei bewiesen |
| SD MISO | GP8 | spi1_rx = funcsel 1 bewiesen (native SPI1-Familie) |
| SD CS | GP9 | spi1_ss_n — **DIREKT-Pin, nie über Expander** |
| SD SCK | GP10 | spi1_sclk |
| SD MOSI | GP11 | spi1_tx |
| HDMI D0+/− | GP12/13 | HSTX config-0-Reihenfolge (bewiesen: 12/13=D0, 14/15=CLK) |
| HDMI CLK+/− | GP14/15 | HSTX |
| HDMI D2+/− | GP16/17 | HSTX |
| HDMI D1+/− | GP18/19 | HSTX |
| **UART0 TX/RX = BMC-Link (AT)** | GP0/GP1 | funcsel 2 — frei bewiesen; AT-Protokoll-Skizze in Backplane-Spec |
| **UART1 TX/RX = MIDI** | GP20/GP21 | funcsel 2 — frei bewiesen |
| **I2C0 SDA = Wii-Controller-Bus** | GP28 | phys. 34, funcsel bewiesen (rp2350a_interface_pins.json) — eigener Bus wegen 0x52-Kollision |
| **I2C0 SCL = Wii-Controller-Bus** | GP29 | phys. 35, funcsel bewiesen — beide Pins frei |
| LED (onboard) | GP25 | non-W = normale GPIO (im Code umgemappt) |
| I2S-Kandidaten (Sound) | GP26/27 | funcsel beim Sound-Board-Design beweisen (GP28 → Wii-I2C) |
| Frei/Reserven | GP22/23/24, GP26–29 | — |

**Pegel-Hinweis:** alles 3V3; PS/2 braucht BSS138-Shifter (5V-Tastatur, bewiesen).

## Hardware-Aufbau (Lochraster, gelötet)

- Pico-2-Fassung 2×20 auf Lochraster-Board (bewiesener Lötplan-Reihenfolge: Fassung → GND-Verband → HDMI → PS/2 → SD)
- **Backplane-Steckverbinder** an Bord (Board steckt auf die Backplane — siehe Backplane-Spec)
- Reset-Taster an RUN+GND (bewiesener Plan)
- LED = onboard GP25 (keine externe LED nötig)

## Firmware-Stand & Builds

| Artefakt | Stand |
|---|---|
| `TINYBASIC_SPI1_pico2.uf2` | **aktuell**: SPI1-Pins GP8–11, PS/2 GP2/3, LED GP25, Banner „Pico 2 HSTX" |
| `TINYBASIC_80x30_workshop_feather.uf2` | Feather-Schaustück (historisch, INPUT-Fix drin) |
| `TINYBASIC_SD_PICO2W_MOSI3.uf2` | Pico-2W-Breadboard-Stand (historisch) |
| Commit tb-c | `c421028` (Pin-Remap + Banner) |
| Host-Tests | tbtest 22/22, tbex 10/10, tbanl 11/11, fat16test 20/20 — alle grün |

## Test-Kette (bewiesen / offen)

| Schritt | Stand |
|---|---|
| Boot + Banner + Prompt | ✅ Feather, ⏳ Lochraster (läuft gerade) |
| HDMI 80×30 | ✅ |
| SD SPI1 (GP8–11): SD: OK + SAVE/LOAD | ✅ Breadboard-Logik, ⏳ Lochraster |
| PS/2 GP2/3: Tastatureingabe | ✅ (MOSI-Port-Falle dokumentiert), ⏳ Lochraster |
| LED GP25 | ⏳ |
| UART0/GP0+1: AT-Antwort | Plan (Backplane) |
| I2C1-Boot-Scan (Slot-EEPROMs) | Plan (Backplane) |
| UART1-MIDI | über Sound-Board |

## Bekannte Fallen (aus der Beweis-Kette)

1. **GPIO_Init (FUNCSEL) vor jedem Output** — Zentralfalle des PicoLibSDK
2. **Build-Cache-Falle:** Header-Änderungen → `touch src/main.cpp` vor `bash c.sh pico2` (UF2-Hash vergleichen als Beweis)
3. **USB-CDC-Drop ohne Host:** UsbPrint verfällt ohne gemounteten Host → immer zyklisch ausgeben. ttyACM0-Verschwinden kann PC-USB-Stau sein (PC-Reboot half zweimal)
4. **PS/2 5V→3V3:** nie direkt, immer BSS138-Shifter
5. **8.3-Dateinamen** auf FAT16: Workshop-Dateinamen ≤8 Zeichen
6. **Scancode-Set-2** bewiesene Tabelle: 0=0x45, 1=0x16 … ;=0x4C, Enter=0x5A (Details tb-c/README)

## Verweise

- Backplane-Spec: `BACKPLANE_SPEC_pico2.md` (Slot-System, Connectors, BMC)
- Sound-Board-Spec: `SOUND_BOARD_SPEC.md` (MIDI, Synth, FV-1)
- Firmware-Repo tb-c: `/home/pi/neotron-hello/tb-c/` (GitHub, gepusht)
- Beispiele: `/home/pi/neotron-hello/beispiele/` (8 .bas, alle host-getestet)
- Teilnehmer-Anleitung: `40-Ressourcen/Workshop-Basic-80er-Anleitung.md` (Vault)
- Claude-Repository: `/home/pi/retro-pico2350/` (Stamm B, Experimentier)