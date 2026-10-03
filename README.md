# Retro-Pico BASIC Computer (RP2350 / Pico 2 W)

Ein kleiner Retro-Heimcomputer auf Basis des Raspberry Pi Pico 2 W (RP2350):
DVI-Videoausgabe über die HSTX-Schnittstelle, PS/2-Tastatur (und optional
PS/2-Maus) als Eingabe, und ein eingebauter BASIC-Interpreter als
"Betriebssystem".

**Status:** Der DVI/HSTX-Treiber basiert 1:1 auf dem offiziellen, verifizierten
Raspberry-Pi-Beispiel (`pico-examples/hstx/dvi_out_hstx_encoder`) und sollte
zuverlässig funktionieren. Der PS/2-Tastaturtreiber folgt einem bewährten,
weit verbreiteten Muster (PIO-basierter Set-2-Scancode-Empfänger). Der
WLAN/Telnet-Code (`src/net/net.c`) folgt 1:1 dem offiziellen
`pico-examples/pico_w/wifi/tcp_client`-Muster (inkl. korrektem
`cyw43_arch_lwip_begin/end`-Locking) und die SD-Karten-Anbindung nutzt die
etablierte, RP2350-fähige Bibliothek `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` — in
beiden Fällen ist aber die projektspezifische Verdrahtung/Integration
(Pinbelegung, Telnet-Terminal-Logik, `LOAD`/`SAVE`) neu und ungetestet.
Der PS/2-**Maus**-Treiber ist ebenfalls experimentell/ungetestet (die
Host-zu-Maus-Initialisierung per Bit-Banging ist deutlich fehleranfälliger) —
Tastatur und Bildschirm funktionieren unabhängig davon auch ohne
angeschlossene Maus. Nichts davon konnte in dieser Umgebung auf echter
Hardware getestet werden (kein Pico 2 W und kein ARM-Cross-Compiler
verfügbar) — bitte beim ersten Flashen mit offenen Augen testen und
Rückmeldung geben, falls etwas nicht passt.

## Hardware-Aufbau

### Video: DVI über HSTX (GPIO 12–19, fest vorgegeben)

Die HSTX-Peripherie ist nur auf GPIO 12–19 verfügbar. Pinbelegung (identisch
zum offiziellen [Pico-DVI-Sock](https://github.com/Wren6991/Pico-DVI-Sock)):

| Signal | GPIO |
|--------|------|
| D0+    | 12   |
| D0-    | 13   |
| CK+    | 14   |
| CK-    | 15   |
| D2+    | 16   |
| D2-    | 17   |
| D1+    | 18   |
| D1-    | 19   |

Jede Leitung braucht einen Vorwiderstand (z.B. 270 Ω) zum DVI/HDMI-Stecker,
da der Pico keine echten TMDS-Treiberpegel liefert. Am einfachsten: fertige
[Pico-DVI-Sock](https://github.com/Wren6991/Pico-DVI-Sock)-Platine aufsetzen,
oder die Widerstände + eine HDMI-Buchse selbst verdrahten. Es wird nur das
digitale Bild übertragen, kein Audio.

Auflösung: 640×480 @ 60 Hz, Textkonsole 40 Spalten × 30 Zeilen (16×16-Pixel-
Zeichen, 8×8-Font auf 2× skaliert).

### Tastatur: PS/2 (GPIO 2/3)

| Signal | GPIO |
|--------|------|
| DATA   | 2    |
| CLOCK  | 3    |

### Maus (optional): PS/2 (GPIO 4/5)

| Signal | GPIO |
|--------|------|
| DATA   | 4    |
| CLOCK  | 5    |

### SD-Karte: SPI1 (GPIO 8–11)

| Signal | GPIO |
|--------|------|
| MISO   | 8    |
| CS     | 9    |
| SCK    | 10   |
| MOSI   | 11   |

SD-Karten sind 3,3V-Bauteile — ein handelsübliches SD-Karten-Breakout-Modul
mit eigenem 3,3V-Regler/Pegelwandler verwenden (viele billige "SD Card
Module"-Platinen sind dafür ausgelegt); direkt an eine blanke SD-Karte ohne
Pegelanpassung sollte man nur gehen, wenn man die 3,3V-Variante hat.

### WLAN: intern (CYW43439 auf dem Pico 2 W)

Keine zusätzliche Verdrahtung nötig — der WLAN-Chip ist auf dem Pico 2 W
bereits verbaut und intern an GPIO 23/24/25/29 angebunden (deshalb sind diese
vier Pins für eigene Projekte nicht mehr frei).

**Hinweis Stack-Größe:** Der Pico-SDK-Standard-Stack für Core 0 ist nur 2KB
groß. In Kombination mit WLAN (lwIP/CYW43-Hintergrundverarbeitung läuft per
Interrupt oben drauf auf dem, was der BASIC-Interpreter gerade an Aufruftiefe
belegt) reicht das oft nicht und führt zu einem Stack-Overflow/Absturz nach
einigen Sekunden WLAN-Betrieb — ein bekanntes Muster bei Pico-W-Projekten.
`CMakeLists.txt` setzt deshalb `PICO_STACK_SIZE=0x4000` (16KB); RAM ist dafür
reichlich vorhanden, da der Framebuffer statisch (nicht auf dem Stack) liegt.

**Wichtig — Pegel:** PS/2-Tastaturen/Mäuse arbeiten mit 5 V-Logik, der Pico
ist aber nur 3,3 V-tolerant. Nicht direkt verbinden! Entweder einen echten
Pegelwandler (Level Shifter) verwenden, oder die verbreitete einfache Lösung:
Pull-ups auf 3,3 V statt 5 V (10 kΩ an DATA/CLOCK nach 3,3V) und die
Gerätespannung weiterhin mit 5 V versorgen — die meisten PS/2-Geräte ziehen
die Leitungen dann sauber genug auf 3,3 V "high", und das Low-Pegel-Treiben
unterschreitet ohnehin die 3,3 V-Schwelle. Das ist der Standardtrick in sehr
vielen Hobby-PS/2-Projekten, aber *keine* offizielle Spezifikation — ein
richtiger bidirektionaler Pegelwandler (z.B. 74LVC245 oder diskret mit
MOSFETs) ist die sauberere, sicherere Lösung.

### Debug-UART (optional)

UART0 auf GPIO 0 (TX) / GPIO 1 (RX), 115200 8N1 — für `printf`-Debugging,
falls ihr `stdio` nutzen wollt (z.B. via `printf()` im Code ergänzen).

## Build

Voraussetzungen: [Pico SDK](https://github.com/raspberrypi/pico-sdk) (Version
mit RP2350-/HSTX-Unterstützung, d.h. aktuell), ARM-GNU-Toolchain, CMake,
Ninja oder Make.

### SD-Karten-Bibliothek besorgen

Die FAT-Dateisystem/SPI-Treiber-Bibliothek ist eine externe Abhängigkeit
(nicht Teil des Pico SDK). Einmalig herunterladen:

```bash
git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git \
    third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
```

(Pfad relativ zum Projektordner `retro-pico2350/`. CMake bricht mit einer
klaren Fehlermeldung ab, falls dieser Ordner fehlt.)

### Bauen

```bash
export PICO_SDK_PATH=/pfad/zu/pico-sdk

mkdir build
cd build
cmake -G Ninja ..
ninja
```

Das Ergebnis ist `retro_pico2350.uf2`. Pico 2 W im BOOTSEL-Modus (BOOTSEL
beim Einstecken gedrückt halten) als Massenspeicher mounten und die `.uf2`
draufkopieren.

Falls ihr einen normalen Pico 2 (ohne WLAN) verwendet: `PICO_BOARD` in der
obersten `CMakeLists.txt` von `pico2_w` auf `pico2` ändern, und die
`WIFI`/`TELNET`-Quelle ([src/net/net.c](src/net/net.c)) sowie die
`pico_cyw43_arch_lwip_threadsafe_background`-Abhängigkeit entfernen, da es
dann keinen WLAN-Chip gibt.

## BASIC-Dialekt

Zeilennummerierte BASIC im Tiny-BASIC-Stil:

```
10 PRINT "HALLO RETRO-PICO"
20 FOR I = 1 TO 10
30 PRINT I, I*I
40 NEXT I
50 END
```

- Variablen: `A`–`Z`, global, Fließkommazahlen (keine Strings/Arrays in v1).
- Anweisungen: `LET`, `PRINT`, `INPUT`, `IF…THEN`, `FOR…TO…STEP…NEXT`,
  `GOTO`, `GOSUB`/`RETURN`, `REM`, `END`/`STOP`, `CLS`, `COLOR fg[,bg]`
  (RGB332-Farbwerte), mehrere Anweisungen pro Zeile getrennt durch `:`.
- Funktionen in Ausdrücken: `ABS(x)`, `INT(x)`, `SGN(x)`, `RND(x)`
  (Zufallszahl 0..x).
- Sofortmodus: Zeile ohne führende Nummer wird direkt ausgeführt
  (z.B. `PRINT 2+2`). `RUN`, `LIST`, `NEW` als Direktbefehle.
- Strg+C während `RUN` bricht das Programm ab (`?BREAK`).

### Dateisystem (SD-Karte)

- `DIR` bzw. `FILES` — listet die Dateien im Wurzelverzeichnis.
- `SAVE "NAME.BAS"` — speichert das aktuelle Programm (eine
  `<zeilennummer> <text>`-Zeile pro Programmzeile).
- `LOAD "NAME.BAS"` — ersetzt das aktuelle Programm durch den Dateiinhalt.
- Ohne eingelegte SD-Karte melden diese Befehle `?NO SD CARD`, der Rest des
  Systems läuft unverändert weiter.

### WLAN / Telnet (nur Pico 2 W)

- `WIFI "SSID","PASSWORT"` — verbindet sich mit einem WLAN-Access-Point
  (WPA2). Zugangsdaten werden nur im RAM gehalten, nicht gespeichert — nach
  jedem Neustart erneut eingeben (oder in ein BASIC-Programm schreiben, das
  per `LOAD` von der SD-Karte kommt).
- `TELNET "HOST",PORT` — baut eine Telnet-Verbindung auf (Port-Parameter
  optional, Default 23) und schaltet in einen interaktiven Vollbildmodus:
  Tastatureingaben gehen direkt an den Server, die Antwort erscheint auf dem
  Bildschirm. `ESC` beendet die Sitzung und kehrt zu `READY` zurück.
  Unterstützt eine minimale Telnet-Verhandlung (lehnt alle Optionen ab, um
  Hänger zu vermeiden) sowie ein kleines Subset von ANSI-Escape-Codes
  (Textfarbe via `ESC[…m`, Bildschirm löschen via `ESC[…J`/`H`). Cursor-
  Positionierung (`ESC[row;colH`) wird **nicht** nachgebildet — viele
  BBS-Menüs bleiben trotzdem gut lesbar, vollständige ANSI-Art-Bildschirme
  eher nicht.

**Bekannte Einschränkungen (bewusste Vereinfachungen für v1):**
- `GOSUB`/`FOR` sollten jeweils die einzige/letzte Anweisung ihrer Zeile
  sein — `NEXT`/`RETURN` springen zeilenweise zurück, nicht auf Byte-Ebene
  innerhalb derselben Zeile. Schleifenkörper also auf eigene Zeilen
  schreiben, z.B. wie im Beispiel oben, nicht `10 FOR I=1 TO 5:PRINT I:NEXT I`
  auf einer Zeile.
- `IF` unterstützt genau einen Vergleich (`<`,`>`,`<=`,`>=`,`=`,`<>`), kein
  `AND`/`OR`.
- Keine String-Variablen, keine Arrays, keine verschachtelten Funktionsaufrufe
  mit mehreren Argumenten.

Das ist bewusst ein kompakter Startpunkt — die Struktur in
[src/basic/basic.c](src/basic/basic.c) ist so gehalten, dass sich Strings,
Arrays, `AND`/`OR` usw. später ergänzen lassen.

## Projektstruktur

```
retro-pico2350/
  CMakeLists.txt
  pico_sdk_import.cmake
  src/
    main.c                  Verdrahtet alles zusammen, Pin-Zuordnung
    video/
      dvi.c / dvi.h          HSTX-DVI-Treiber + Textkonsole
      font8x8_basic.h        8x8-Bitmap-Font (Public Domain, dhepper/font8x8)
    input/
      ps2.pio                Gemeinsamer PS/2-Empfänger (PIO-Assembler)
      ps2kbd.c / .h          Tastatur: Scancode-Set-2 -> ASCII
      ps2mouse.c / .h        Maus: Host-Init + Bewegungspakete (optional)
    storage/
      hw_config.c             SPI/Pin-Konfiguration für die SD-Karten-Bibliothek
      sdcard.c / .h           Mount + Datei-Hilfsfunktionen (FatFs-Wrapper)
    net/
      lwipopts.h              lwIP-Konfiguration (minimal, für Telnet-Terminal)
      net.c / .h              WiFi-Verbindung + Telnet-Client (lwIP raw API)
    basic/
      basic.c / .h           BASIC-Interpreter + REPL/Zeileneditor
```

Externe Abhängigkeit (nicht im Repo enthalten, siehe Build-Abschnitt):

```
third_party/
  no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/   FatFs + SPI/SDIO-SD-Kartentreiber
                                       (github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
```

## Nächste Schritte / Ideen zum Ausbauen

- String-Variablen (`A$`) und `ARRAY`-Unterstützung im Interpreter.
- Maus-Cursor / einfache Grafikbefehle (`PLOT`, `LINE`) im Textmodus oder
  einem zusätzlichen Grafikmodus.
- WLAN-Zugangsdaten + zuletzt genutzte BBS-Adressen auf der SD-Karte
  speichern (`WIFI`/`TELNET` merken sich aktuell nichts über einen Neustart
  hinaus — das wäre ein `CONFIG.TXT`, das beim Boot automatisch geladen wird).
- Vollständigere ANSI/VT100-Emulation (echte Cursor-Positionierung) für
  BBS-ANSI-Art, statt nur Farbe + Clear-Screen.
- USB-Tastatur als Alternative/Ergänzung zu PS/2 über `tinyusb`.
