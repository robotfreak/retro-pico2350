# Backplane-Spec — „Pico 2 Lochraster + Steckplane"

**Stand:** 06.10.2026 · **Status:** WIP (Steckverbinder-Belegung nicht final)
**Vorbild:** Neotron-Pico-Expansions-Standard (bewiesen aus README Neotron-Compute/Neotron-Pico) — Slot-Adressierung über EEPROM-Adresspins, MCP23S17 für CS/IRQ, 2×10-Connector

---

## Architektur-Übersicht

```
        ┌─────────────────────────── Backplane ───────────────────────────┐
        │  Slot-ID-Verdrahtung (A0/A1/A2 je Slot fix an VCC/GND)           │
        │  I2C-Bus (2 Adern + Pull-ups, Root auf Backplane)               │
        │  SPI-Bus (3 Adern) + je-Slot CS + je-Slot IRQ (über Expander)   │
        │  ~RESET-Leitung, 5V, 3V3, GND                                   │
        └───────┬─────────────────┬──────────────────────┬────────────────┘
                │Slot0            │Slot1                 │SlotN
         ┌──────▼──────┐   ┌──────▼──────┐        ┌──────▼──────┐
         │     BMC     │   │  (SD-Board  │        │  Erweiter.  │
         │ WiFi-Micro  │   │  oder frei) │        │ MIDI/Sound/…)│
         │ (UART→Main) │   │ EEPROM 0x51 │        │ EEPROM 0x5X │
         └─────────────┘   └─────────────┘        └─────────────┘
         ║ UART0 GP0/GP1 zur Main-CPU (AT-Protokoll)
┌────────┴──────────────── Main CPU: Pico 2 (RP2350, non-W) ───────────────┐
│  HSTX/HDMI GP12–19 · PS/2 GP2/3 (+Maus GP4/5) · SD SPI1 GP8–11 · LED GP25│
│  UART0 GP0/1 (=BMC) · I2C1 GP6/7 (=Backplane) · UART1 GP20/21 (=MIDI)   │
│  I2S-Kandidaten: GP26/27/28 (Sound-Board, später)                        │
└───────────────────────────────────────────────────────────────────────┘
```

## Pin-Zusicherungen Main-CPU (FIRMWARE-STAND bewiesen: c421028)

| Funktion | GPIO | funcsel/Anmerkung |
|---|---|---|
| PS/2 Tastatur DATA | GP2 | SIO, IRQ EDGELOW (bewiesen im Code) |
| PS/2 Tastatur CLOCK | GP3 | SIO, IRQ EDGELOW |
| PS/2 Maus DATA (opt.) | GP4 | reserviert (Treiberslot später) |
| PS/2 Maus CLOCK (opt.) | GP5 | reserviert |
| **I2C1 SDA** (Backplane-Bus) | **GP6** | funktion 3 = i2c1 — FREI bewiesen |
| **I2C1 SCL** (Backplane-Bus) | **GP7** | funktion 3 = i2c1 — FREI bewiesen |
| SD MISO | GP8 | spi1_rx (funcsel 1, io_bank0 bewiesen) |
| SD CS | GP9 | spi1_ss_n — **DIREKTPIN, NICHT über Expander!** |
| SD SCK | GP10 | spi1_sclk |
| SD MOSI | GP11 | spi1_tx |
| HDMI D0+/− | GP12/13 | HSTX-fix (config-0-Reihenfolge) |
| HDMI CLK+/− | GP14/15 | HSTX-fix |
| HDMI D2+/− | GP16/17 | HSTX-fix |
| HDMI D1+/− | GP18/19 | HSTX-fix |
| **UART0 TX/RX** (BMC-Link) | **GP0/GP1** | funktion 2 = uart0 — FREI, **AT-Protokoll** |
| **UART1 TX/RX** (MIDI) | **GP20/GP21** | funktion 2 = uart1 — FREI bewiesen; MIDI-Out meist nur TX |
| I2S (Sound, später) | GP26/27/28 | Kandidaten — Funcsel beim Sound-Board-Design beweisen |
| LED | GP25 | onboard, im Code (Kein CYW43!) |
| frei insgesamt | GP22, GP23, GP24, GP26–29 | Reserven |

## Steckverbinder-Belegung (Empfehlung 2×10, Neotron-kompatibel!)

```
 SPI_COPI(MOSI)  1    2  GND
 SPI_CIPO(MISO)  3    4  GND
 SPI_CLK         5    6  GND
 ~SPI_CS(Slot)   7    8  ~IRQ(Slot)
 I2C_SDA         9   10  I2C_SCL
 EEPROM_ADDR0   11   12  EEPROM_ADDR1
 EEPROM_ADDR2   13   14  ~RESET
 5V             15   16  5V
 3V3            17   18  3V3
 GND            19   20  GND
```

## Zusatzsteckverbinder — UART-Leitungen (Wahl: 2×8 = 16 Pins)

**Zweck:** die UARTs des RP2350 für die Backplane-Bausteine: BMC-AT-Link,
MIDI-Stream — **plus Reserve** (HW-Flow-Control, dritter UART-Bedarf, oder
eine zweite I2C-Route). Belegungsvorschlag:

```
 UART0_TX (BMC)   1    2  GND
 UART0_RX (BMC)   3    4  GND
 UART1_TX (MIDI)  5    6  GND
 UART1_RX (MIDI)  7    8  GND
 3V3              9   10  GND
 RESERVE (GND)   11   12  RESERVE (Signal)
 RESERVE (Signal) 13  14  RESERVE (GND)
 5V              15   16  GND
```

**Reserve-Empfehlung:** Pin 11–14 als bewusst freie Leitungen mit GND-
Nachbarn. Mögliche spätere Nutzungen (je Bedarf entschieden, nicht jetzt
verdrahtet): CTS/RTS für den BMC-UART (Hardware-Flow), ein 3. UART-Kanal
(Zweit-BMC/Zweit-MIDI), oder eine getrennte I2C-Route (falls der
Backplane-I2C mal Buslast kriegt). **5V auf Pin 15** als Reserve-Versorgung
(z. B. für Optokoppler-Schaltungen am Audio-Board, die 5V-LED-Strom brauchen —
3V3 bleibt die Signalebene).


**Anmerkung zu SPI auf dem Backplane-Bus:** Beim Hauptboard läuft der SD am
**SPI1-Pin-Gruppe GP8–11**. Der Backplane-SPI = die GLEICHE SPI1-Bank (MISO/
MOSI/SCLK sind Bus-Leitungen) + je-Slot-CS via Expander. **Der SD-CS (GP9)
bleibt DIREKT-Pin** — der Expander kriegt nur die Slot-CS-Leitungen (langsam
und optional). Für hohe SD-Durchsätze: SD NICHT auf den Backplane-SPI legen,
sondern direkten SPI1-Pfad verwenden (wie heute bewiesen).

## Slot-Adressierung (EEPROM-Adresspins — Neotron-bewiesen)

- Je Slot 3 Leitungen (EEPROM_ADDR0/1/2) — auf der **Backplane** fix mit
  VCC/GND verdrahtet (je Slot anders!).
- Board-EEPROM (AT24C256 o. ä.): A0/A1/A2 über Steckverbinder an die
  Slot-Leitungen → **I2C-Adresse = Slot-Nummer** (0x50 = Slot 0 … 0x57 = Slot 7)
- A0 = LSB; 7-Bit-Adresse: `0x50 | slot` (A2 = Bit 2)
- **Slot 0 = BMC** (WiFi-Micro) — braucht kein EEPROM (oder reserviert)
- **Slot 1 = SD** (beim Neotron) — bei uns: SD direkt am SPI1 des Pico;
  Slots 2–7 für Erweiterungen. **Achtung:** Bei uns die Slot-Nummer
  anders wählen (SD nicht als Slot!) — Empfehlung: Slot 0 = BMC,
  Slots 1–7 frei (SD ist nicht als Slot gefasst).

## EEPROM-Inhalt (NEID-kompatibel — Empfehlung)

```
Byte 0–1:  NEID-Board-Typ (u16 LE) — „wer bin ich"
Byte 2:    Revision (u8)
Byte 3–7:  Seriennummer (5 B, werkseitig/der User)
Byte 8–15: reserviert (0xFF)
Byte 16+:  board-spezifische Beschreibung (TBD je Board)
```

Die Firmware scannt beim Boot Adresse 0x50–0x57, erkennt Steckplatte/BMC und
aktiviert die zugehörigen Treiber. Kompatibilität zum NEID-Format wäre
Bonus (mit dem Neotron-OS-Stamm austauschbar).

## BMC-Link (UART0, GP0/GP1) — Protokoll-Skizze

**BMC-Hardware-Entscheidung (06.10.2026, Peter): Pico 2 W** — kein ESP32,
keine zweite Architektur im Haus, Board vorhanden. Der BMC sitzt als
**Slot 0**-Board in der Backplane, nutzt sein CYW43-WiFi (dort OHNE
HSTX-Video = der bewiesene Konflikt-Fall existiert nicht, kein Video am BMC!).

**AT-Befehlssatz (80er-Modem-Stil, bewährt bei Retro-WiFi-Modems):**

| Befehl | Wirkung |
|---|---|
| `AT` | Handshake → `OK` |
| `ATI` | Firmware-Version des BMC → `OK` |
| `ATH` | Verbindung schließen → `NO CARRIER` |
| `ATDT<host>:<port>` | TCP/Telnet-Verbinden → `CONNECT` oder `ERROR` |
| `AT+CWLAP` | WLAN-Liste (optional) |
| `AT+CWJAP="<ssid>","<pwd>"` | WLAN-Login ([REDACTED]-Handhabung!) |
| Daten-Modus | nach `CONNECT`: transparente Zeichen-Brücke UART↔TCP |
| `+++` (1 s still) | zurück in den Befehlsmodus |

Bewährter Standard-Flow: Befehlsmodus → `ATDT realitycheckbbs.org:23` →
`CONNECT` → BBS-Banner am HDMI (Terminal-Brücke Hauptboard). Der BMC
(Firmware: W5500-Variante kein WLAN; ESP32-Variante mit WLAN) implementiert
nur die Brücke — die eigentliche „Anwendung" (BBS-Terminal, Telnet-BASIC)
liegt am Main-CPU.

## Offene Punkte (WIP)

- [ ] Slot-Reihenfolge fixieren (SD = Slot? oder außerhalb) — Empfehlung: außerhalb
- [ ] ~~BMC-Hardware-Entscheidung~~ ✅ **Pico 2 W** (06.10.) — WiFi-Modem-Firmware (AT-Bridge UART↔lwIP-TCP, Basis: bbs-client-Code `f7d9c33` als Startpunkt — WLAN-Connect-Teil bewiesen!)
- [ ] MIDI-Board-Spec (UART1, GP20/21) — später
- [ ] Sound-Board-Spec (I2S, GP26–28-Kandidaten + EEPROM) — später
- [ ] Expander-Wahl: MCP23S17 (SPI) vs MCP23008 (I2C) — SPI-Variante ist der
      Neotron-Standard (bewiesen) und braucht nicht den I2C-Bus
- [ ] EEPROM-Schreibschutz (WP-Pin): Backplane-Zugriff fürs Beschreiben der
      Kennung ermöglichen oder fix read-only?
- [ ] 32blit-HSTX-Projekt (separat, siehe Diskussion 06.10.)
- [x] **Wii-Controller-Ports** (Ergänzung 06.10.): **2× I2C-Steckverbinder** für
      Nintendo Wii-kompatible Game-Controller — ✅ **Entscheidung: eigener
      zweiter I2C-Bus (I2C0, SDA=GP28/SCL=GP29)** wegen 0x52-Kollision;
      Details in der Wii-Sektion

## Wii-Controller-Anschlüsse (2×) — Ergänzung vom 06.10.

**Ziel:** Nintendo Wii-kompatible Controller (Nunchuk, Wii Classic Controller,
NES/SNES-Classic-Pads — **alle I2C-Adresse 0x52**, gleiche Button-Maps) an
der Backplane.

**Der Hardware-Pfad (bewährt, 3 Bauteile je Port):**

```
 Wii-Stecker (Proprietary ~Mini-DIN) ── Adapter-Breakout (WiiClassic/Nunchuk
    Breakout mit I2C-Pins rausgeführt, z. B. Adafruit 2634/2635) ──
    Backplane-I2C-Bus (3V3, 400 kHz)
 ⚠ Achtung: Wii-Controller laufen INTERN mit **3,3 V** — kein 5V-Pin an den
    Controller! Der Adapter-Breakout löst die Pegel-/Spannungsfrage.
```

**⚠ Die konkrete Kollision: Wii-Controller = Adresse 0x52, Slot-2-EEPROM = 0x52!**
Der Wii-Controller darf **nicht** auf den Backplane-Slot-I2C-Bus.
Lösungsoptionen (noch offene Entscheidung):
  1. **Wii-Controller an einen GETRENNTEN zweiten I2C-Bus** (eigene
     SDA/SCL-Leitungen am 2×8-Zusatzverbinder — die „Reserve-Pins"!)
  2. Wii-Controller hinter einen I2C-Multiplexer (TCA9548A oder TCA9544)
  3. Slot-Adressierung der EEPROMs umziehen (z. B. Slot-EEPROMs auf
     0x60–0x67 — die AT24C256 kann nur A0/A1/A2 = 0x50–0x57, also bräuchte
     man eine EEPROM-Adressverschiebe-Schaltung — **nicht empfohlen**)
- **Empfehlung: Option 1** — Wii-Ports an eigenes I2C-Bus-Pärchen (#2-I2C-Bus
  am Main-CPU = **I2C0: SDA=GP28 (phys. 34), SCL=GP29 (phys. 35)** — funcsel
  aus rp2350a_interface_pins.json offiziell bewiesen, beide Pins frei!
  Pull-ups 2,2–4,7 kΩ an 3V3 an JEDEM Wii-Adapter-Breakout (die Wii-Internes
  hat keine!). Baudrate 400 kHz.

**Wii-Protokoll-Notizen (bewiesen aus Community-Doku):**
- I2C-Adresse 0x52 (7-Bit; „0xA4/0xA5" = Schreib/Lese-Byte-Darstellung)
- Init: `I2C-W 0x52, 0x40, 0x00` (unkomprimierter Modus) —
  Alternative: `0xFB, 0x00`-Handshake-Varianten je Controller-Stamm
- Lesen: Register 0x00 schreiben, dann 6 Bytes lesen (Buttons + Sticks)
- 400 kHz „Fast-Mode"-Bus; zwischen Transfers kurze Pause (~1 ms)
- Nunchuk: 6 Bytes = X/Y-Button-Z + Analog-Stick; Classic: beide
  Sticks + 12 Tasten in 6 Bytes
- Wii-Classic = **perfekter Retro-Gamepad**: D-Pad + A/B/X/Y + L/R/Triggers,
  2 Analog-Sticks — für 32blit-/Games-Workshop-Software ideal

## Main-CPU-Board-Spec (verweist auf Hauptboard-Doku)

Die Main-CPU-Board-Spec lebt **separat** → `MAINBOARD_SPEC_pico2.md`
(neueste Erkenntnisse werden dort notiert: Pin-Zuordnung,
Firmware-Stand, Test-Kette).