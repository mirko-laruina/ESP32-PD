
# ESP32-C3 USB PD Hacky Stack

This project is a fork of [yangminglong/ESP32-PD](https://github.com/yangminglong/ESP32-PD), which is itself a fork of [g3gg0/ESP32-PD](https://github.com/g3gg0/ESP32-PD). The original project is described in the [ESP32 USB PD blog post](https://www.g3gg0.de/esp32-pd-usb-pd-using-esp32-zigbee-crib/).

A lightweight and experimental USB Power Delivery (PD) stack for the ESP32-C3. This project enables the ESP32-C3 to request voltages from a power supply, sniff and log PD communication, inject commands for experimentation with Vendor Defined Messages (VDM), and provide a web interface for monitoring and control.

## Why This Project?

Typical USB PD solutions require external ICs like the CH224K, which can be as large as the ESP32 itself. The original implementation replaces such ICs with a minimalistic approach using **MUN5233 NPN transistors with a monolithic bias resistor network** ([datasheet](https://www.farnell.com/datasheets/1672232.pdf)) for voltage level conversion. I've tested this fork using a discrete **BC547** instead, with a **10 kΩ base series resistor** and a **47 kΩ base-to-GND pull-down**. The CC1 line also uses **5.1 kΩ pull-down resistors** as the PD termination.

-   **RX Path:** Converts the 0-1.2V CC signal to 3.3V using a MUN5233 transistor (or a BC547 with resistors) and the IO in weak mode as a pull up.
-   **TX Path:** Either directly drives with 3.3V (very hacky) or uses two outputs driving against each other, reducing voltage to approximately 1.7V - still outside protocol spec but works with all tested devices.

Just good enough for your casual DIY project, saving you another huge component on your design. This is a hacky solution, use at your own risk!

## Features

-   **USB PD Communication:** Successfully requests user-defined voltage and maximum current from a USB PD power supply.
-   **Sniffing & Logging:** Monitor and log USB PD communication on the CC line.
-   **Command Injection:** Send messages to experiment with PD communication, including Vendor Defined Messages (VDM).
-   **Minimal Hardware Footprint:** Removes the need for bulky PD controllers by leveraging simple transistor-based level shifting.
-   **Exploits** the GPIOs capabilities by intentionally using them as weak outputs being driven to GND by the transistor or the CC-line. It's all still inside the specs, yet nothing one would really use for a reliable field design.
-   Uses one GPIO for receiving and one GPIO (far beyond PD spec) or two GPIOs (closer to PD spec) for driving one CC line with 3.3V/1.7V, whilst both variants seem to work reliable.
-   **Web Control Interface:** Built-in web server (Wi-Fi station, credentials stored in NVS) that shows the live protocol status and lets you negotiate power modes from a browser.
    -   Status page: connection/protocol state, last communication age, charger presence, negotiated output values and uptime.
    -   Guided three-step power request: pick a mode, set voltage and current, apply and watch the result (`pending` → `accepted` → `ready`, or `rejected`/`timeout`).
    -   Fixed PDO selection with per-PDO current limit; PPS selection with the voltage/current range advertised by the charger.
     -   Optional live **PPS status polling** (`Get_PPS_Status`) reporting the charger-side voltage, current and status while a PPS contract is active. The device probes support automatically and enables the polling checkbox only when the charger responds with `PPS_Status`. If `Not_Supported` or timeout, it marks the feature as unavailable.
    -   Server-side validation of every request against the capabilities the charger actually advertised (mode existence, current limits, PPS voltage range and 20 mV granularity).
-   **Console Commands:** Interactive console over USB-Serial-JTAG (`get_src_cap`, `req_obj`, `req_pps`, `vdm`, `webconfig`, `log_level`, ...).

### Using the Web Interface

1. Connect to the serial console and run `webconfig <ssid> <password>` - the credentials are stored persistently in NVS and reused on every boot.
2. Open the URL printed in the log (e.g. `http://192.168.1.42/`) from any device on the same network.
3. The page polls the device once per second and disables all controls while the charger is unreachable, a request is in flight, or the advertised capabilities are stale.
4. The **Request fresh capabilities** button is in the connection and protocol status card. Use it after connecting or replacing the charger.
5. After a PPS contract reaches `ready`, the device automatically probes `Get_PPS_Status`. The device enables the PPS polling checkbox only when the charger responds with `PPS_Status`. When `Not_Supported` or timeout it marks the feature as unavailable.
6. The interface treats the charger as disconnected after a short period without received PD traffic and clears the controls.

### Wi-Fi Configuration

The device operates as a Wi-Fi station only. Run the following command once through the USB-Serial-JTAG console:

```text
webconfig <ssid> <password>
```

The credentials are stored in the NVS namespace `web` under the keys `ssid` and `pass`, then reused automatically after reboot. The HTTP server starts only after the station obtains an IP address. The Wi-Fi manager uses one STA netif and one event handler; it reconnects using the standard ESP-IDF station flow.

## Current State

The implementation is capable of successfully requesting a user-defined voltage and maximum current from a USB PD power supply. It operates using a hacky but functional method of voltage level shifting for communication. Although outside the official PD specifications, it has been tested and confirmed to work with multiple devices.
It is outside the spec for the protocol encoding - but still a valid voltage on that line(!). In worst case scenario, the power supply will issue a PD protocol recovery over and over.
Also the current state does only drive one CC-line. So in your design you have to combine the CC1/CC2 into one pin, which works not so reliable with all cables [as raspberry users have noticed](https://www.scorpia.co.uk/2019/06/28/pi4-not-working-with-some-chargers-or-why-you-need-two-cc-resistors/) **or** simply rotate the plug if the device doesn't come up.

Alternatively use the second transistor in the MUN5233 to drive the same ESP32 RX pin low (unverified) and use a second TX pin, driving the second CC line as well. Needs another (or two) GPIOs. Something for next year or so :)

## Hardware Requirements

The requirements of the original projects were:
-   **ESP32-C3**
-   **MUN5233 NPN Transistor** I am using the MUN5233DW with two transistors ([datasheet](https://www.farnell.com/datasheets/1672232.pdf))
-  **5.1kΩ** pulldown on CC
-   **USB PD Power Supply**
-   **CC Line Access** for monitoring/sniffing

For the developments of this fork, I've used:
-   **ESP32-C3**
-   **BC547 NPN transistor** for the RX level shifter with **10 kΩ** base series resistor and **47 kΩ** base-to-GND pull-down
-  **5.1 kΩ** pull-down on CC
-  **USB PD Power Supply**


## Disclaimer

This project is an experimental, hacky implementation and does not fully comply with USB PD specifications. Use at your own risk!
If you want to contribute, please do so. I will happily share access to the repository.

----------

## License

MIT License
