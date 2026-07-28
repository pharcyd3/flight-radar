# Credits & License

## Data sources

- **Flight data** — [airplanes.live](https://airplanes.live/), a free, keyless, community-driven ADS-B aggregator. Frank's Flight Radar uses their public REST API under its fair-use guidance.
- **Map tiles** — © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors, served via the standard OSM tile server. Please be considerate of [OSM's tile usage policy](https://operations.osmfoundation.org/policies/tiles/) if you modify tile-fetching behaviour.

## Libraries

Frank's Flight Radar is built on:

- [M5Dial](https://github.com/m5stack/M5Dial) / [M5Unified](https://github.com/m5stack/M5Unified) / [M5GFX](https://github.com/m5stack/M5GFX) — M5Stack's hardware abstraction and graphics libraries
- [ArduinoJson](https://arduinojson.org/) — JSON parsing, including its streaming filter feature used to keep heap usage down when parsing flight-data responses
- [WiFiManager](https://github.com/tzapu/WiFiManager) — the captive-portal WiFi/settings provisioning flow
- The classic [PJRC Encoder library](https://www.pjrc.com/teensy/td_libs_Encoder.html) (bundled with M5Dial), for interrupt-driven quadrature encoder reading

## License

This project does not currently specify a license. All rights are reserved by the repository owner unless a license is added.
