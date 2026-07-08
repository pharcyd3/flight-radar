# Credits & License

## Data sources

- **Flight data** — [OpenSky Network](https://opensky-network.org/), a non-profit, community-driven ADS-B receiver network. FlightDial uses their public REST API under its [terms of use](https://opensky-network.org/about/terms-of-use).
- **Map tiles** — © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, served via the standard OSM tile server. Please be considerate of [OSM's tile usage policy](https://operations.osmfoundation.org/policies/tiles/) if you modify tile-fetching behaviour.

## Libraries

FlightDial is built on:

- [M5Dial](https://github.com/m5stack/M5Dial) / [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX) — M5Stack's hardware abstraction and graphics libraries
- [ArduinoJson](https://arduinojson.org/) — JSON parsing, including its streaming filter feature used to keep heap usage down when parsing OpenSky responses
- [WiFiManager](https://github.com/tzapu/WiFiManager) — the captive-portal WiFi/settings provisioning flow
- The classic [PJRC Encoder library](https://www.pjrc.com/teensy/td_libs_Encoder.html) (bundled with M5Dial), for interrupt-driven quadrature encoder reading

## License

This project does not currently specify a license. All rights are reserved by the repository owner unless a license is added.
