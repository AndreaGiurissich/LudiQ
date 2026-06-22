# LudiQ

**Human-Computer Interaction** project: a cube-shaped physical controller designed to manage card and board game tournaments. The cube combines physical input (buttons, touch, rotation), player identification via **NFC**, and **WiFi** communication with a host PC acting as the tournament "scoreboard".

Supported games: **Pokémon TCG**, **Yu-Gi-Oh!**, **Chess**, plus an **RNG** utility (coin flip + dice) that can be called during any match.

---

## System Architecture

Two components communicating via WebSockets:

    ┌──────────────┐   WiFi / WebSocket   ┌───────────────────────────┐
    │  ESP32 CUBE  │ ───────────────────► │  Host PC                  │
    │ (WS client)  │ ◄─────────────────── │  server.js (hub)          │
    │  firmware    │         JSON         │     │                     │
    │   .ino       │                      │     └─► public/ (web app) │
    └──────────────┘                      │         browser (UI)      │
                                          └───────────────────────────┘

- The **cube** acts as the client: it connects to the host and sends events (player identification, match start/end, etc.).
- **`server.js`** functions purely as a **switchboard/hub**: it serves the web app and forwards messages between the cube and the browser. It does not contain tournament logic.
- The **web app** (running in the browser) handles all the bracket logic, renders the scoreboard dashboard, and controls the cube (specifying the game and time limit to be used).

### Project Files

- `cubo_unificato__3.ino`: ESP32 firmware (Arduino)
- `server.js`: Node hub: HTTP + WebSocket (port 8080)
- `package.json`: Host dependencies (only "ws")
- `public/`
  - `index.html`: Web app structure
  - `style.css`: Styling (scoreboard layout, dark theme)
  - `app.js`: Application logic: WebSocket handling + bracket model + UI rendering

---

## Hardware Specifications

| Component | Model / Note |
| :--- | :--- |
| **Microcontroller** | ESP32-WROOM-32 DevKit ("ESP32 Dev Module" board) |
| **Display** | 2.8" TFT 240×320, ST7789/ILI9341 driver, SPI |
| **Touch Screen** | XPT2046 resistive (shared SPI bus, separate CS) |
| **IMU** | MPU6500 (I²C 0x68) — orientation / gesture detection |
| **NFC Reader** | PN532 v3 (I²C 0x24) — player identification |
| **Input** | 4 physical buttons: OK, Cancel, Left, Right |
| **Feedback** | Passive buzzer |

> **Warning:** All hardware modules must be powered at **3.3V**, not 5V.
