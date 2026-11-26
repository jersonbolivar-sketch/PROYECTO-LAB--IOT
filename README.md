# PROYECTO-LAB--IOT
PROYECTO DE LABOATORIO, PARA PROTEGER A LOS ANIMALES ANTE LOS FRIOS DE LAS ALTAS NEVADAS DEL PERU

## Estructura

| Carpeta | Contenido |
|---|---|
| `firmware/nodo1_transmisor/` | ESP32 Nodo 1: lee 2x DS18B20, BME280 y BMP280, controla calefacción y cortina, envía por LoRa 433 MHz |
| `firmware/nodo2_receptor/` | ESP32 Nodo 2: recibe por LoRa, valida y muestra; envío opcional a InfluxDB Cloud |
| `docs/cableado.md` | Tabla de conexiones de ambos nodos y actuadores |
| `grafana/dashboard_ganado.json` | Tablero de Grafana Cloud importable (InfluxDB, Flux) |

## Cómo probar el enlace LoRa

1. Instala en Arduino IDE: OneWire, DallasTemperature, Adafruit BME280, Adafruit BMP280, Adafruit Unified Sensor y LoRa (Sandeep Mistry). Placa: *ESP32 Dev Module*.
2. Cablea según `docs/cableado.md` y conecta las antenas.
3. Sube `nodo1_transmisor` a un ESP32 y `nodo2_receptor` al otro. Monitor serie a 115200.
4. El Nodo 1 envía un paquete cada 10 s; el Nodo 2 lo imprime con su RSSI y SNR.
5. Para enviar a InfluxDB: copia `secrets.example.h` como `secrets.h`, complétalo y pon `ENVIAR_A_INFLUX` en 1. `secrets.h` no se sube a GitHub.

## Tablero en Grafana Cloud

1. En Grafana Cloud crea el data source InfluxDB con lenguaje **Flux** (URL de tu InfluxDB Cloud, organización, token y bucket `iot_ganado`).
2. Dashboards → New → Import → sube `grafana/dashboard_ganado.json` → elige ese data source → Import.
