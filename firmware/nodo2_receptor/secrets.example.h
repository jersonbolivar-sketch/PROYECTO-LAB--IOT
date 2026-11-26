// Plantilla de credenciales. Copia este archivo como "secrets.h" en la misma
// carpeta y rellena tus datos. secrets.h NO se sube a GitHub (.gitignore).
#pragma once

#define WIFI_SSID      "NOMBRE_RED_WIFI"
#define WIFI_PASSWORD  "CLAVE_WIFI"

// URL de tu instancia de InfluxDB Cloud (sin "/" al final), por ejemplo:
// https://us-east-1-1.aws.cloud2.influxdata.com
#define INFLUX_URL     "https://REGION.cloud2.influxdata.com"
#define INFLUX_ORG     "NOMBRE_ORGANIZACION"
#define INFLUX_BUCKET  "iot_ganado"
#define INFLUX_TOKEN   "TOKEN_PRIVADO"
