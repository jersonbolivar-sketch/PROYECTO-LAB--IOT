# Cableado del enlace LoRa y los sensores

Pines tomados del resumen técnico del proyecto. Todo funciona a **3,3 V**: no conectes ningún módulo de esta lista a 5 V.

## Nodo 1 (transmisor): ESP32 + sensores + LoRa

### LoRa Ra-01 SX1278 (SPI)

| Pin Ra-01 | ESP32 | Nota |
|---|---|---|
| VCC | 3V3 | Nunca 5 V. Pon 100 nF + 10 µF cerca del módulo |
| GND | GND | |
| SCK | GPIO 18 | |
| MISO | GPIO 19 | |
| MOSI | GPIO 23 | |
| NSS / CS | GPIO 27 | |
| RESET | GPIO 14 | |
| DIO0 | GPIO 26 | |
| ANT | antena 433 MHz | **Conéctala antes de transmitir**, sin antena el módulo se puede dañar |

### DS18B20 x2 (OneWire, mismo bus)

| Pin DS18B20 | ESP32 |
|---|---|
| VCC (rojo) | 3V3 |
| GND (negro) | GND |
| DATA (amarillo) | GPIO 4 (los dos sensores al mismo pin) |
| Pull-up | **10 kΩ entre GPIO 4 y 3V3** (una sola resistencia para los dos) |

Con cables largos o lecturas de -127 °C prueba 4,7 kΩ (o dos de 10 kΩ en paralelo = 5 kΩ).
El sketch identifica cada sensor por su dirección única: T1 es el primero que encuentra en el bus.
Si quieres saber cuál es cuál, calienta uno con la mano y mira cuál sube en el Monitor serie.

### BME280 y BMP280 (I²C, mismo bus)

| Pin módulo | ESP32 |
|---|---|
| VIN / VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| CSB | sin conectar (o 3V3) para modo I²C |
| SDO | define la dirección, ver abajo |

Los dos módulos comparten SDA/SCL, pero **no pueden tener la misma dirección**:

| Módulo | SDO a | Dirección |
|---|---|---|
| BME280 | GND | 0x76 |
| BMP280 | 3V3 | 0x77 |

Muchos módulos vienen con SDO a GND (0x76) de fábrica, así que lo normal es que tengas que cambiar el del BMP280.
Al arrancar, el Nodo 1 escanea el bus I²C e imprime las direcciones encontradas, para comprobarlo.

El BMP280 es opcional: el sketch funciona sin él. Si el BME280 falla, la lógica usa la temperatura del BMP280 como respaldo.

> Ojo: muchos módulos vendidos como "GY-BME280" son en realidad BMP280 (sin humedad). Si el Monitor serie dice "BME280 NO encontrado" pero detecta un BMP280, es eso.

### Actuadores

> Las láminas y el motor van con **su propia fuente de 12 V**. Une el GND de esa fuente con el GND del ESP32. Nunca alimentes el motor ni las láminas desde el ESP32.

**Calefacción (3 láminas 12 V / 7 W en paralelo ≈ 1,75 A)**

| Elemento | Conexión |
|---|---|
| Gate del MOSFET | GPIO 25 (con 220 Ω en serie y 10 kΩ de gate a GND) |
| Drain | negativo de las láminas |
| Source | GND |
| Positivo de las láminas | +12 V |

Comprueba que el MOSFET conduzca bien con 3,3 V en el gate (tipo "logic level"); si se calienta, usa uno logic level o un driver.

**Cortina: motorreductor DC con TB6612FNG**

| Pin TB6612FNG | Conexión |
|---|---|
| PWMA | GPIO 32 |
| AIN1 | GPIO 16 |
| AIN2 | GPIO 17 |
| STBY | GPIO 33 |
| VCC | 3V3 |
| VM | +12 V del motor (más 100 µF cerca del módulo) |
| GND (todos) | GND común |
| AO1 / AO2 | terminales del motor |

El TB6612FNG aguanta ~1,2 A continuos (3,2 A de pico). Verifica que la corriente de arranque/bloqueo de tu motor no lo supere.

**Finales de carrera** (contacto normalmente abierto entre el pin y GND)

| Final de carrera | ESP32 | Pull-up externo |
|---|---|---|
| Cortina desplegada | GPIO 34 | 10 kΩ de GPIO 34 a 3V3 |
| Cortina recogida (opcional) | GPIO 35 | 10 kΩ de GPIO 35 a 3V3 |

GPIO 34 y 35 son solo entrada y no tienen pull-up interno: la resistencia externa es obligatoria.
Si solo tienes un final de carrera, deja `USAR_FC_RETRAIDA 0`: la cortina se recoge por tiempo (`TIEMPO_RECOGER_MS`, mídelo con un cronómetro).

**Lógica (ajustable al inicio de `nodo1_transmisor.ino`)**

| Condición (temperatura ambiente) | Acción |
|---|---|
| ≤ 3 °C | enciende calefacción y despliega cortina |
| ≥ 7 °C | apaga calefacción |
| ≥ 8 °C | recoge cortina |

Seguridad: si el motor no llega al final de carrera en `TIEMPO_MAX_MOTOR_MS` se para y queda en FALLA (no reintenta hasta reiniciar). Sin temperatura válida la calefacción se apaga. Con `ACTUADORES_HABILITADOS 0` no se mueve nada, solo se reporta.

## Nodo 2 (receptor): ESP32 + LoRa

Mismo cableado del Ra-01 que el Nodo 1 (18, 19, 23, 27, 14, 26, 3V3, GND, antena).

## Parámetros de radio (iguales en los dos nodos)

433 MHz · SF7 · BW 125 kHz · CR 4/5 · Sync word 0x12 · CRC activado · 14 dBm

## Formato del paquete

```
ID=1,SEQ=12,T1=3.20,T2=3.50,TA=2.80,H=78.5,P=1012.4,TB=2.90,PB=1012.1,CALEFACCION=1,CORTINA=1,CEST=1,ERR=0
```

`NA` = lectura inválida. `CORTINA` = orden (1 desplegar). `CEST` = posición real: 0 recogida, 1 desplegada, 2 desplegando, 3 recogiendo, 4 desconocida, 5 falla.
`ERR` es un mapa de bits: 1 = T1, 2 = T2, 4 = BME280, 8 = BMP280, 16 = motor de cortina.
