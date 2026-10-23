# Stromversorgungs-Spec — Backplane-System „Pico 2 Konsolen-Familie"

**Stand:** 06.10.2026 · **Status:** PLAN (vor dem Löten der Backplane fixieren!)
**Gilt für:** Main-CPU-Board (Pico 2), BMC (Pico 2 W), Erweiterungs-Boards (MIDI/Sound/etc.)

---

## Anforderungen (Peters drei Punkte)

1. **USB am PC:** ein am PC hängender Pico darf das **gesamte Backplane-System
   nicht** mit Strom versorgen
2. **Zentrale 5V:** die Picos werden vom Backplane-Netzteil gespeist — **ohne
   Rückwärts-Strom in den USB-Bus des PCs**
3. **12V-Zentralversorgung:** ein 12V-Netzteil, darunter DC-DC-Wandler
   für 5V (Backplane-Stromschiene) und 3V3 (lokale Logik)

## Grundprinzip: Power-Path mit ORing-Dioden

**Der Schlüssel:** Der Pico hat **zwei 5V-Eingänge mit unterschiedlicher Rolle**
(bewiesen aus dem Pico-Datasheet):

| Pin | Rolle | Richtung |
|---|---|---|
| **VBUS** | USB-5V-Eingang | hat intern eine Diode **VBUS→VSYS** (Pico speist sich selbst davon) |
| **VSYS** | System-Eingang (1,8–5,5 V) | der reguläre Versorgungspfad für den 3V3-Regler |

**Der Trick:** Der VBUS-Pin ist ein **reiner Eingang** — es gibt keine interne
Rückwärts-Route VSYS→VBUS. Wer den Backplane-5V **auf VSYS** legt (nicht auf
VBUS!), kann vom Pico aus **nie** in den PC-USB zurücktreiben.

###Das Schaltungsschema je Board:

```
                   Schottky (SS34 o.ä.)
  USB-PC (5V) ──VBUS-Pin──►|────┐
                               ├──► VSYS (Pico-interne 3V3-Erzeugung)
  Backplane-5V-Schiene ────────┘
```

**Das ORing-Verhalten (bewährte Praxis):**
- **Backplane-5V aktiv:** kommt direkt auf VSYS. Der DCDC steht auf
  **5,1 V** (leicht über USB-5,0 V) → die VBbus-Schottky sperrt (Anode
  5,0 V < Kathode 5,1 V) → **der PC liefert NULL Strom**, hängt nur als
  USB-Device (Konsole/MSD-Flash) an der Datenline
- **Backplane-5V AUS, USB allein dran:** die Schottky öffnet, der Pico
  versorgt **sich selbst** (lokale 3V3) — er kann die Backplane-Schiene
  NICHT rückwärts speisen (keine Verbindung VSYS→Backplane-Schiene!)
- **Beide Quellen weg:** System aus. Kein Pfad bleibt offen.

**Der zentrale Trick:** Der Backplane-5V geht NUR über eine Einbahn-Strecke in
VSYS. Ein Board steckt in einem freien USB-Port beim Programmieren —
das Board ist dann isoliert und unabhängig.

## Bord-Design (je Board identisch)

- **VBUS-Schottky:** SS34 (3A/40V, 0,3 V@2A) oder SS14 (1A, klein) —
  je Pico einen, direkt am VBUS-Pin
- **Backplane-5V → VSYS DIREKT** (keine Diode nötig — VSYS ist ein Eingang
  mit interner Diode VBUS→VSYS; ein Rückwärtsdrücken VSYS→VBUS gibt es
  hardwareseitig nicht)
- **USB-Stecker am Board (optional):** Pico-USB bleibt zugänglich (BOOTSEL-
  Flash + Serial-Konsole). Wenn kein PC dran: Backplane speist alles.

## Zentrale Versorgung (12V → 5V + 3V3)

```
 12V-Netzteil ──► DC-DC-Buck 5,1V (5A) ──► [5V-Schiene Backplane]
                     │ Ideal-Diode (LM66100 o.ä.)
                     ▼
                 Backplane-5V-Schiene (an alle Boards → VSYS)

 12V-Netzteil ──► DC-DC-Buck 3,3V (1–2 A) ──► [3V3-Schiene Backplane]
                     │ Ideal-Diode
                     ▼
                 Backplane-3V3-Schiene (für Sensoren/Displays an Boards)
```

**Komponenten-Empfehlungen (bewährt & günstig):**

| Block | Baustein | Warum |
|---|---|---|
| 12V→5V | **MP1584EN** (3A-Buck, ~2€) oder **LM2596** (robuster Klassiker) | genügt für 2–4 Picos + Erweiterungen |
| 12V→3V3 | **MP1584EN** (2. Instanz, auf 3,3V gestellt) | separat: die 3V3-Schiene NICHT vom Pico-3V3-Out nutzen (ein Pico kann nur ~300 mA und ist nicht der Master!) |
| Rückwärts-Schutz 5V-Schiene | Ideal-Diode **LM66100** (~0,5 €, 1A) oder Schottky SS34 | schützt den Buck gegen Rückgespeiste aus VSYS der Boards |
| 5V→3V3 am Board (klein) | **AMS1117-3.3** (Board-eigen) | nur wenn einBoard keine Backplane-3V3-Schiene bekommt |

**Warum 5,1 V statt 5,0 V:** Damit die ORing-Schottky am VBUS-Pin sicher
sperrt, wenn 12V-Netzteil aktiv ist (Backplane-Schiene = dominant). Mit exakt
5,0 V wäre die Dioden-Kennlinie grenzwertig (USB-Netzteile driften ±5%).

## 3V3-Zweig — zwei getrennte Welten

- **Pico-interne 3V3** (aus VSYS erzeugt): die Picos erzeugen ihre 3V3 selbst
  (bewiesener Pfad: VSYS → Regler → 3V3-Out). Sie speisen kleine lokale
  Lasten (Pull-ups, LEDs) — **nicht** die Backplane!
- **Backplane-3V3-Schiene** (eigener DC-DC-Wandler): für Erweiterungs-Boards
  mit 3V3-Peripherie (Wii-Adapter, Sound-Board-Logik). **NIEMALS** eine
  3V3-Schiene mit einem Pico-3V3-Out verbinden (Einbahn-Regel — siehe oben)
- Die I2C-Pull-ups der Backplane (Slot-EEPROMs, MCP23S17) hängen an die
  **Backplane-3V3-Schiene** (nicht an irgendeinen Pico!) — sonst hängt die
  Bus-Pegel-Stabilität an einem einzelnen Board-Regler

## Strom-Budget (grobe Größenordnung)

| Verbraucher | Strom (5V) |
|---|---|
| Pico 2 (non-W), idle+BASIC | ~80–150 mA |
| Pico 2 W (BMC, WiFi aktiv) | ~100–250 mA (Burst beim WiFi-TX bis 300 mA!) |
| Erweiterungs-Board (MIDI/Sound, ohne Amp) | ~50–150 mA |
| **Reserve (Wachstum)** | +1 A |

→ **MP1584EN @ 3A reicht** für 2–3 Boards + Reserve. Bei +Amp/Speaker:
LM2596 (3A) ist die robuste Wahl, oder 2× MP1584 parallel je 2 A.

## Zusammenfassung der Einbahn-Regeln (Die 4 Regeln)

1. **Backplane-5V → nur auf VSYS-Pins der Picos** (Nie auf VBUS!) — dann
   ist Rückwärts-Speisung in den PC hardwareseitig unmöglich
2. **VBUS jedes Boardes mit Schottky** zum ORing (PC-USB kann nur das
   eine Board speisen, nie die Schiene)
3. **Backplane-3V3 nie mit Pico-3V3-Out verbinden** — zwei getrennte
   3V3-Welten, die Backplane-Schiene hat ihren eigenen Wandler
4. **Ideal-Diode zwischen DC-DC-Wandlern und den Schienen** — schützt die
   Wandler gegen rückgespeiste Boards

## Offene Punkte

- [ ] 12V-Steckdose am Backplane-Board (DC-Hohlstecker 5,5/2,1 mm bewährt?)
- [ ] Gesamtleistung: 12V-Netzteil 2A (24W) reicht für den Workshop-Start
- [ ] Verteilung auf der Backplane: Stern-Verteilung (je Slot ein eigener
      5V/3V3-Verbindungspfad, keine Daisy-Chain) — Spannungsabfall + Störfestigkeit
- [ ] Elko-Verteilung: 100µF je Schiene am Backplane-Root + je Slot 10µF