/*
 * NODO 2 - Receptor / gateway (LoRa -> Monitor serie -> InfluxDB Cloud)
 * Proyecto IoT: protección térmica del ganado ante heladas
 *
 * 1) Recibe los paquetes LoRa del Nodo 1 y los muestra en el Monitor serie.
 * 2) Valida campos, detecta paquetes perdidos (SEQ) y falta de recepción.
 * 3) Arma la línea en formato "line protocol" de InfluxDB.
 * 4) Si ENVIAR_A_INFLUX = 1, se conecta al Wi-Fi y escribe en InfluxDB Cloud
 *    por HTTPS. Por defecto está en 0 para probar primero solo el enlace LoRa.
 *
 * Credenciales: copia secrets.example.h como secrets.h (misma carpeta) y
 * complétalo. secrets.h está en .gitignore: no lo subas a GitHub.
 *
 * Librerías: LoRa (Sandeep Mistry). WiFi y HTTPClient vienen con el core ESP32.
 * Placa: "ESP32 Dev Module"
 */

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Si existe secrets.h en la carpeta del sketch se usa; si no, estos marcadores.
// (Así el sketch compila aunque solo pegues este archivo.)
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID      "NOMBRE_RED_WIFI"
  #define WIFI_PASSWORD  "CLAVE_WIFI"
  #define INFLUX_URL     "https://REGION.cloud2.influxdata.com"
  #define INFLUX_ORG     "NOMBRE_ORGANIZACION"
  #define INFLUX_BUCKET  "iot_ganado"
  #define INFLUX_TOKEN   "TOKEN_PRIVADO"
#endif

// 0 = solo LoRa + Monitor serie;  1 = además enviar a InfluxDB Cloud
#define ENVIAR_A_INFLUX 0

// ---------------- Pines LoRa (mismos que el Nodo 1) ----------------
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_CS    27
#define LORA_RST   14
#define LORA_DIO0  26

// ---------------- Radio (DEBE coincidir con el Nodo 1) ----------------
#define LORA_FRECUENCIA  433E6
#define LORA_SF          7
#define LORA_BW          125E3
#define LORA_CR          5
#define LORA_SYNC_WORD   0x12

#define NODE_ID_ESPERADO   1
#define TIMEOUT_LORA_MS    35000UL   // ~3 paquetes perdidos con envío cada 10 s

struct Datos {
  int   id = -1;
  long  seq = -1;
  float t1 = NAN, t2 = NAN, ta = NAN, h = NAN, p = NAN, tb = NAN, pb = NAN;
  int   calefaccion = -1, cortina = -1, cest = -1, err = -1;
};

long ultimaSeq = -1;
unsigned long ultimaRecepcion = 0;
bool avisoSinSenal = false;
unsigned long perdidos = 0;
unsigned long paquetesOk = 0;
unsigned long ultimoEstado = 0;
#define INTERVALO_ESTADO_MS 60000UL   // cada cuánto se envía el estado del gateway

// ---------------------------------------------------------------
float aFloat(const String &v) {
  if (v == "NA" || v.length() == 0) return NAN;
  return v.toFloat();
}

// Separa "CLAVE=valor,CLAVE=valor,..." y rellena la estructura.
bool parsear(const String &pkt, Datos &d) {
  int inicio = 0;
  while (inicio < (int)pkt.length()) {
    int coma = pkt.indexOf(',', inicio);
    if (coma < 0) coma = pkt.length();
    String par = pkt.substring(inicio, coma);
    int igual = par.indexOf('=');
    if (igual > 0) {
      String k = par.substring(0, igual);
      String v = par.substring(igual + 1);
      if      (k == "ID")          d.id = v.toInt();
      else if (k == "SEQ")         d.seq = v.toInt();
      else if (k == "T1")          d.t1 = aFloat(v);
      else if (k == "T2")          d.t2 = aFloat(v);
      else if (k == "TA")          d.ta = aFloat(v);
      else if (k == "H")           d.h  = aFloat(v);
      else if (k == "P")           d.p  = aFloat(v);
      else if (k == "TB")          d.tb = aFloat(v);
      else if (k == "PB")          d.pb = aFloat(v);
      else if (k == "CALEFACCION") d.calefaccion = v.toInt();
      else if (k == "CORTINA")     d.cortina = v.toInt();
      else if (k == "CEST")        d.cest = v.toInt();
      else if (k == "ERR")         d.err = v.toInt();
    }
    inicio = coma + 1;
  }
  return d.id >= 0 && d.seq >= 0;
}

// Agrega un campo float a la línea de InfluxDB solo si es válido
void campo(String &s, bool &primero, const char *k, float v) {
  if (isnan(v)) return;
  s += primero ? ' ' : ',';
  primero = false;
  s += k; s += '='; s += String(v, 2);
}
void campoInt(String &s, bool &primero, const char *k, long v) {
  if (v < 0) return;
  s += primero ? ' ' : ',';
  primero = false;
  s += k; s += '='; s += String(v); s += 'i';
}

// Formato: ganado,nodo=1 temp_amb=2.8,humedad=78.5,...,seq=125i
// Sin marca de tiempo: InfluxDB asigna la hora de llegada.
// cortina = orden (1 desplegar), cortina_estado = posición física
//   0 recogida, 1 desplegada, 2 desplegando, 3 recogiendo, 4 desconocida, 5 falla
String lineaInflux(const Datos &d, int rssi, float snr, long perdidosAhora) {
  String s = "ganado,nodo=" + String(d.id);
  bool primero = true;
  campo(s, primero, "temp_suelo1", d.t1);
  campo(s, primero, "temp_suelo2", d.t2);
  campo(s, primero, "temp_amb",    d.ta);
  campo(s, primero, "humedad",     d.h);
  campo(s, primero, "presion",     d.p);
  campo(s, primero, "temp_bmp",    d.tb);
  campo(s, primero, "presion_bmp", d.pb);
  campoInt(s, primero, "calefaccion", d.calefaccion);
  campoInt(s, primero, "cortina",     d.cortina);
  campoInt(s, primero, "cortina_estado", d.cest);
  campoInt(s, primero, "err",         d.err);
  campoInt(s, primero, "seq",         d.seq);
  campoInt(s, primero, "perdidos",    perdidosAhora);
  campo(s, primero, "rssi", (float)rssi);
  campo(s, primero, "snr",  snr);
  return s;
}

// ---------------- Wi-Fi + InfluxDB ----------------
#if ENVIAR_A_INFLUX
void asegurarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Conectando a Wi-Fi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("Wi-Fi OK, IP %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println(F("Wi-Fi FALLO (se reintenta con el siguiente paquete)"));
}

bool enviarInflux(const String &linea) {
  asegurarWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure cliente;
  cliente.setInsecure();   // prototipo: no valida el certificado. Para producción, usar setCACert().
  HTTPClient http;
  String url = String(INFLUX_URL) + "/api/v2/write?org=" + INFLUX_ORG +
               "&bucket=" + INFLUX_BUCKET + "&precision=s";
  if (!http.begin(cliente, url)) return false;
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int codigo = http.POST(linea);
  if (codigo != 204) {
    Serial.printf("InfluxDB respondió %d: %s\n", codigo, http.getString().c_str());
  }
  http.end();
  return codigo == 204;
}
#endif

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== NODO 2 - Receptor ==="));

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  while (!LoRa.begin(LORA_FRECUENCIA)) {
    Serial.println(F("LoRa NO inicia: revisa cableado SPI, NSS=27, RST=14, 3V3. Reintento en 2 s"));
    delay(2000);
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  Serial.println(F("LoRa OK a 433 MHz, esperando paquetes..."));

#if ENVIAR_A_INFLUX
  asegurarWiFi();
#endif
  ultimaRecepcion = millis();
}

void loop() {
  int tam = LoRa.parsePacket();
  if (tam > 0) {
    String pkt;
    while (LoRa.available()) pkt += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    float snr = LoRa.packetSnr();

    Datos d;
    if (!parsear(pkt, d)) {
      Serial.printf("[RX descartado] formato inválido: %s\n", pkt.c_str());
      return;
    }
    if (d.id != NODE_ID_ESPERADO) {
      Serial.printf("[RX ignorado] ID=%d no es el Nodo %d\n", d.id, NODE_ID_ESPERADO);
      return;
    }

    long perdidosAhora = 0;
    if (ultimaSeq >= 0 && d.seq > ultimaSeq + 1) {
      perdidosAhora = d.seq - ultimaSeq - 1;
      perdidos += perdidosAhora;
      Serial.printf("  Aviso: se perdieron %ld paquete(s)\n", d.seq - ultimaSeq - 1);
    } else if (ultimaSeq >= 0 && d.seq <= ultimaSeq) {
      Serial.println(F("  Aviso: SEQ reiniciada (¿se reinició el Nodo 1?)"));
    }
    ultimaSeq = d.seq;
    ultimaRecepcion = millis();
    avisoSinSenal = false;
    paquetesOk++;

    Serial.printf("[RX] RSSI=%d dBm SNR=%.1f dB | %s\n", rssi, snr, pkt.c_str());
    Serial.printf("  Suelo T1=%.2f T2=%.2f | Amb %.2f °C %.1f %% %.1f hPa | Calef=%d Cortina=%d (estado %d) ERR=%d | perdidos=%lu\n",
                  d.t1, d.t2, d.ta, d.h, d.p, d.calefaccion, d.cortina, d.cest, d.err, perdidos);

    String linea = lineaInflux(d, rssi, snr, perdidosAhora);
    Serial.printf("  Influx: %s\n", linea.c_str());
#if ENVIAR_A_INFLUX
    Serial.println(enviarInflux(linea) ? F("  -> InfluxDB OK") : F("  -> InfluxDB FALLO"));
#endif
  }

  if (!avisoSinSenal && millis() - ultimaRecepcion > TIMEOUT_LORA_MS) {
    Serial.println(F("ALERTA: no llegan paquetes LoRa del Nodo 1"));
    avisoSinSenal = true;
    ultimoEstado = 0;   // avisar ya a InfluxDB
  }

  // Estado del gateway (sirve para ver en Grafana si el Nodo 1 dejó de llegar)
  if (ultimoEstado == 0 || millis() - ultimoEstado >= INTERVALO_ESTADO_MS) {
    ultimoEstado = millis();
    String e = "gateway,nodo=2 sin_senal=" + String(avisoSinSenal ? 1 : 0) + "i" +
               ",paquetes_ok=" + String(paquetesOk) + "i" +
               ",perdidos_total=" + String(perdidos) + "i" +
               ",uptime_s=" + String(millis() / 1000) + "i";
#if ENVIAR_A_INFLUX
    if (WiFi.status() == WL_CONNECTED) e += ",wifi_rssi=" + String(WiFi.RSSI()) + "i";
    enviarInflux(e);
#endif
    Serial.printf("  Estado gateway: %s\n", e.c_str());
  }
}
