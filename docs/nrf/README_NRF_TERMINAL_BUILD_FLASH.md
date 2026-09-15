# nRF Connect Terminal – Build & Flash

Kurze Erinnerung, wie das richtige Terminal für Zephyr-/Nordic-Befehle geöffnet wird.

## nRF-Terminal in VS Code öffnen

In VS Code:

1. `Cmd + Shift + P` öffnen.
2. Nach folgendem Command suchen:

```text
nRF Connect: Create Shell Terminal
```

3. Dieses Terminal verwenden, nicht ein normales VS-Code-Terminal.

Alternative:

```text
Terminal-Panel → kleiner Pfeil neben + → Launch Profile... → nRF Connect
```

## Prüfen, ob das Terminal korrekt ist

```bash
west --version
```

Wenn `west` gefunden wird, ist die Nordic-/Zephyr-Umgebung aktiv.

## Build

Im Projektordner ausführen:

```bash
west build -p always -b xiao_nrf54l15/nrf54l15/cpuapp .
```

## Flash

Nach erfolgreichem Build:

```bash
west flash
```

## Merksatz

Wenn `west` nicht gefunden wird, ist sehr wahrscheinlich das falsche Terminal geöffnet.
