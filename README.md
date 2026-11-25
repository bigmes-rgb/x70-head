# X70 head

Firmware for a stepper-converted Vidpro MH-430 pan/tilt head carrying a Sony PXW-X70, and for its joystick box.

| Folder | What |
|---|---|
| `x70_head/` | Head: ESP32 + 2x TMC2209. Web joystick page, travel limits, presets, VISCA for OBS, joystick-box input |
| `x70_joystick/` | Joystick box: ESP32 inside the Vidpro controller, reads the original proportional stick |
| `site/` | Installer page (flashes over USB from Chrome/Edge, no software needed) |
| `tools/` | Bench test sketch used during the build |

## How updates work
1. Edit a file here (the pencil icon on any file) and commit to `main`.
2. GitHub compiles both firmwares (a few minutes; see the Actions tab) and publishes a new build.
3. On the head's page open **Update and Wi-Fi** and tap **Update both now**.

First install over USB: open the installer page (repository Settings > Pages shows its address).

No Wi-Fi names or passwords are stored in this code. Each device keeps its own, set on its Wi-Fi page.
The default device password is `x70head123`: change it on the Wi-Fi page.
