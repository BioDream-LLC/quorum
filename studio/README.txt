quorum studio
=============

Start it from the Start menu. It opens in your web browser; close the black window to stop it.

  1 Load      open a rulebook (.cb), import a HAZOP / LOPA / FMEA worksheet (.csv), or learn from past records (.csv)
  2 Rulebook  edit it. Ctrl+F find, Ctrl+H replace; the to-do list shows what a draft still needs
  3 Check     every combination of sensor readings is tried. Any problem, and no image is made
  4 Image     download the chip file, or press Write with the board plugged in
                Raspberry Pi Pico 2: hold BOOTSEL while plugging it in
                Arduino Uno R4 Minima: just plug it in (needs the Arduino UNO R4 board package installed once)

The board prints a DEVICE line after start-up. If it matches the line the studio shows, the chip decided every
situation exactly as the studio checked.

The program is not code-signed yet, so Windows SmartScreen may warn the first time. Choose "More info", then
"Run anyway".
