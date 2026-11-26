/*
 * NODO 1 - Transmisor (sensores + LoRa)
 * Proyecto IoT: protección térmica del ganado ante heladas
 *
 * Lee:
 *   - 2 x DS18B20 (temperatura de suelo/superficie) en bus OneWire, GPIO 4
 *   - BME280 (temperatura, humedad, presión) por I2C, GPIO 21/22
 *   - BMP280 opcional (temperatura, presión) por I2C, mismo bus
 * Calcula el estado SOLICITADO de calefacción y cortina (histéresis),
 * pero NO mueve actuadores: quedan bloqueados hasta validar el hardware.
 * Envía un paquete de texto por LoRa 433 MHz cada INTERVALO_ENVIO_MS.
 *
 * Librerías (Gestor de librerías de Arduino IDE):
 *   OneWire, DallasTemperature, Adafruit BME280, Adafruit BMP280,
 *   Adafruit Unified Sensor, LoRa (Sandeep Mistry)
 * Placa: "ESP32 Dev Module"
 */

#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

// ---------------- Pines ----------------
// LoRa Ra-01 SX1278 (SPI por defecto del ESP32: SCK 18, MISO 19, MOSI 23)
#define LORA_SCK   18
#define LORA_MISO  19
#define LORA_MOSI  23
#define LORA_CS    27
#define LORA_RST   14
#define LORA_DIO0  26

#define ONEWIRE_PIN 4     // DATA de los dos DS18B20 + pull-up 10k a 3V3
#define I2C_SDA     21
#define I2C_SCL     22

// Actuadores (solo se fijan en estado seguro, no se accionan)
#define PIN_MOSFET_CALEF 25
#define PIN_TB_STBY      33

// ---------------- Radio LoRa (igual en ambos nodos) ----------------
#define LORA_FRECUENCIA  433E6
#define LORA_SF          7
#define LORA_BW          125E3
#define LORA_CR          5       // 4/5
#define LORA_SYNC_WORD   0x12
#define LORA_TX_DBM      14

// ---------------- Aplicación ----------------
#define NODE_ID              1
#define INTERVALO_ENVIO_MS   10000UL

// Umbrales preliminares (°C), solo para pruebas
#define CALEF_ON_C    3.0
#define CALEF_OFF_C   7.0
#define CORT_BAJA_C   3.0
#define CORT_SUBE_C   8.0

// Bits del campo ERR (0 = todo bien)
#define ERR_T1   0x01
#define ERR_T2   0x02
#define ERR_BME  0x04
#define ERR_BMP  0x08

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature ds18(&oneWire);
DeviceAddress dirT1, dirT2;
bool hayT1 = false, hayT2 = false;

Adafruit_BME280 bme;
Adafruit_BMP280 bmp;
bool hayBME = false, hayBMP = false;
uint8_t dirBME = 0, dirBMP = 0;

uint32_t secuencia = 0;
bool calefSolicitada = false;
bool cortinaDesplegada = false;
unsigned long ultimoEnvio = 0;

// ---------------------------------------------------------------
void imprimirDireccion(const DeviceAddress d) {
  for (uint8_t i = 0; i < 8; i++) {
    if (d[i] < 16) Serial.print('0');
    Serial.print(d[i], HEX);
  }
}

void escanearI2C() {
  Serial.println(F("Escaneando bus I2C..."));
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Dispositivo en 0x%02X\n", a);
      n++;
    }
  }
  if (n == 0) Serial.println(F("  Ninguno: revisa SDA=21, SCL=22, 3V3 y GND"));
}

void iniciarSensores() {
  // DS18B20
  ds18.begin();
  int n = ds18.getDeviceCount();
  Serial.printf("DS18B20 detectados: %d (se esperan 2)\n", n);
  hayT1 = ds18.getAddress(dirT1, 0);
  hayT2 = ds18.getAddress(dirT2, 1);
  if (hayT1) { Serial.print(F("  T1 = ")); imprimirDireccion(dirT1); Serial.println(); }
  if (hayT2) { Serial.print(F("  T2 = ")); imprimirDireccion(dirT2); Serial.println(); }
  ds18.setResolution(12);

  // I2C: BME280 y BMP280 deben tener direcciones distintas (0x76 / 0x77)
  Wire.begin(I2C_SDA, I2C_SCL);
  escanearI2C();

  const uint8_t direcciones[2] = {0x76, 0x77};
  for (uint8_t i = 0; i < 2 && !hayBME; i++) {
    if (bme.begin(direcciones[i], &Wire)) { hayBME = true; dirBME = direcciones[i]; }
  }
  for (uint8_t i = 0; i < 2 && !hayBMP; i++) {
    if (direcciones[i] == dirBME) continue;   // esa dirección ya es del BME280
    if (bmp.begin(direcciones[i], BMP280_CHIPID)) { hayBMP = true; dirBMP = direcciones[i]; }
  }
  if (hayBME) Serial.printf("BME280 OK en 0x%02X\n", dirBME);
  else        Serial.println(F("BME280 NO encontrado"));
  if (hayBMP) Serial.printf("BMP280 OK en 0x%02X\n", dirBMP);
  else        Serial.println(F("BMP280 no encontrado (opcional)"));
  if (!hayBME && hayBMP)
    Serial.println(F("  Aviso: si solo aparece un BMP280, tu módulo 'GY-BME280' podría ser en realidad un BMP280 (sin humedad)."));
}

void iniciarLoRa() {
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
  LoRa.setTxPower(LORA_TX_DBM);
  Serial.println(F("LoRa OK a 433 MHz"));
}

// Lee un DS18B20 por su dirección; devuelve NAN si falla.
float leerDS(bool presente, const DeviceAddress dir) {
  if (!presente) return NAN;
  float t = ds18.getTempC(dir);
  // -127 = desconectado; 85 = valor de arranque (lectura no válida)
  if (t == DEVICE_DISCONNECTED_C || t == 85.0f || t < -55.0f || t > 125.0f) return NAN;
  return t;
}

// Agrega ",CLAVE=valor" o ",CLAVE=NA" si el valor no es válido
void agregar(String &s, const char *clave, float v, uint8_t decimales) {
  s += ',';
  s += clave;
  s += '=';
  if (isnan(v)) s += "NA";
  else          s += String(v, (unsigned int)decimales);
}

// ---------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=== NODO 1 - Transmisor ==="));

  // Actuadores en estado seguro: calefacción apagada, driver de motor deshabilitado
  pinMode(PIN_MOSFET_CALEF, OUTPUT); digitalWrite(PIN_MOSFET_CALEF, LOW);
  pinMode(PIN_TB_STBY, OUTPUT);      digitalWrite(PIN_TB_STBY, LOW);

  iniciarSensores();
  iniciarLoRa();
}

void loop() {
  if (millis() - ultimoEnvio < INTERVALO_ENVIO_MS && ultimoEnvio != 0) return;
  ultimoEnvio = millis();

  // --- Lecturas ---
  ds18.requestTemperatures();             // ~750 ms a 12 bits
  float t1 = leerDS(hayT1, dirT1);
  float t2 = leerDS(hayT2, dirT2);

  float ta = NAN, h = NAN, p = NAN;
  if (hayBME) {
    ta = bme.readTemperature();
    h  = bme.readHumidity();
    p  = bme.readPressure() / 100.0f;      // hPa
  }
  float tb = NAN, pb = NAN;
  if (hayBMP) {
    tb = bmp.readTemperature();
    pb = bmp.readPressure() / 100.0f;
  }

  uint8_t err = 0;
  if (isnan(t1)) err |= ERR_T1;
  if (isnan(t2)) err |= ERR_T2;
  if (isnan(ta) || isnan(h) || isnan(p)) err |= ERR_BME;
  if (hayBMP && (isnan(tb) || isnan(pb))) err |= ERR_BMP;

  // --- Lógica (solo estado solicitado; actuadores bloqueados) ---
  // Referencia ambiental: BME280; si falla, se usa el BMP280.
  float tRef = !isnan(ta) ? ta : tb;
  if (!isnan(tRef)) {
    if (!calefSolicitada && tRef <= CALEF_ON_C)  calefSolicitada = true;
    if ( calefSolicitada && tRef >= CALEF_OFF_C) calefSolicitada = false;
    if (!cortinaDesplegada && tRef <= CORT_BAJA_C) cortinaDesplegada = true;
    if ( cortinaDesplegada && tRef >= CORT_SUBE_C) cortinaDesplegada = false;
  }

  // --- Paquete ---
  // ID=1,SEQ=12,T1=3.20,T2=3.50,TA=2.80,H=78.5,P=1012.4,TB=2.90,PB=1012.1,CALEFACCION=1,CORTINA=1,ERR=0
  String pkt = "ID=" + String(NODE_ID) + ",SEQ=" + String(secuencia);
  agregar(pkt, "T1", t1, 2);
  agregar(pkt, "T2", t2, 2);
  agregar(pkt, "TA", ta, 2);
  agregar(pkt, "H",  h,  1);
  agregar(pkt, "P",  p,  1);
  agregar(pkt, "TB", tb, 2);
  agregar(pkt, "PB", pb, 1);
  pkt += ",CALEFACCION=" + String(calefSolicitada ? 1 : 0);
  pkt += ",CORTINA="     + String(cortinaDesplegada ? 1 : 0);
  pkt += ",ERR="         + String(err);

  LoRa.beginPacket();
  LoRa.print(pkt);
  bool ok = LoRa.endPacket();   // true = salió del módulo, NO confirma recepción

  Serial.printf("[TX %s] %s\n", ok ? "ok" : "FALLO", pkt.c_str());
  secuencia++;
}
