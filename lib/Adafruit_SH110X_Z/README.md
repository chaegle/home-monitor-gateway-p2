# Home Monitoring Gateway P2

A Bluetooth Low Energy (BLE) environmental monitoring gateway built around the **Particle P2 (Photon 2)** and an **Adafruit SH1107 OLED FeatherWing**.

This project is part of the **Home Monitoring Network**, a distributed environmental monitoring system using Particle devices to collect and display sensor measurements.

The gateway acts as a BLE central device, receiving environmental measurements from remote monitoring nodes and displaying the information locally.

The project is being developed using **Particle Workbench** and **Particle Device OS 6.4.1**.

## 1. Project Overview

The Home Monitoring Gateway provides a local interface for environmental monitoring nodes distributed throughout a home or other monitored environment.

The initial implementation communicates with a Particle P2 environmental sensor installed in a garage.

The sensor measures temperature and relative humidity using a Honeywell HIH-6130 I²C sensor and periodically transmits measurements over BLE.

The gateway receives these measurements and displays them on a 128×64 OLED.

### Current Features

- Particle P2 operating as a BLE central device.
- BLE discovery using a dedicated environmental monitoring service UUID.
- Automatic connection to the environmental sensor.
- Subscription to BLE characteristic notifications.
- Temperature and relative humidity decoding.
- Measurement sequence tracking.
- Local display using an Adafruit SH1107 OLED FeatherWing.
- Automatic BLE reconnection following sensor or gateway resets.
- Detection of stale sensor measurements.
- Nonblocking BLE data processing using a mutex-protected notification buffer.

### Planned Features

- Wi-Fi connectivity.
- Particle Cloud integration.
- Local temperature monitoring using a DS1721 I²C sensor.
- Support for multiple BLE environmental monitoring nodes.
- Automatic rotation between monitored nodes on the OLED.
- Manual node selection using OLED FeatherWing buttons.
- Temperature display selection between Celsius and Fahrenheit.
- Cloud publishing of environmental measurements.
- Additional gateway diagnostics and status reporting.

## 2. Hardware

### Gateway Hardware

| Component | Description |
|---|---|
| Microcontroller | Particle P2 / Photon 2 |
| Display | Adafruit SH1107 OLED FeatherWing |
| Display resolution | 128 × 64 pixels |
| Display interface | I²C |
| Display I²C address | `0x3C` |
| Wireless interface | Bluetooth Low Energy |
| Development environment | Particle Workbench |
| Device OS | 6.4.1 |

### I²C Connections

The OLED display communicates with the Particle P2 using the primary I²C interface.

| Particle P2 Pin | Function |
|---|---|
| D0 | I²C SDA |
| D1 | I²C SCL |
| 3V3 | Display power |
| GND | Ground |

The SH1107 display operates at I²C address `0x3C`.

The display is configured in landscape orientation using the SH1107 graphics library.

### Planned Local Temperature Sensor

A Maxim/Dallas DS1721 temperature sensor will be added to the existing I²C bus.

The DS1721 supports configurable I²C addresses from `0x48` through `0x4F`.

The final address will depend on the hardware configuration.

## 3. Software Architecture

The gateway firmware is organized around four primary functions:

1. BLE device discovery and connection management.
2. BLE notification reception and measurement decoding.
3. Environmental measurement state management.
4. OLED display updates.

The application uses a nonblocking main loop to coordinate these operations.

### BLE Discovery

The gateway scans for devices advertising the Home Monitoring environmental service UUID.

Once a compatible device is discovered, the gateway attempts to establish a BLE connection.

After connecting, the gateway locates the environmental measurement characteristic and subscribes to notifications.

### BLE Connection Recovery

The gateway periodically checks its BLE connection state.

If the sensor disconnects, the gateway resumes scanning and attempts to reconnect.

This allows recovery from events such as:

- Environmental sensor resets.
- Gateway resets.
- Temporary BLE signal loss.
- Sensor power interruptions.

The initial gateway implementation has been validated through overnight operation and device reset testing.

### Measurement Processing

BLE notifications are received asynchronously.

The notification callback copies the incoming measurement payload into a mutex-protected buffer.

The main application loop subsequently decodes the measurement data and updates the displayed values.

This design minimizes processing inside the BLE callback.

## 4. BLE Protocol

The gateway communicates with compatible Home Monitoring environmental nodes using a custom BLE service and characteristic.

### BLE Service

| Parameter | Value |
|---|---|
| Service UUID | `8b7e0001-6a2d-4c91-9e31-4f2a8c6d1000` |
| Characteristic UUID | `8b7e0002-6a2d-4c91-9e31-4f2a8c6d1000` |
| Characteristic properties | READ, NOTIFY |
| Sensor advertised name | `HM-Garage-ENV` |

The gateway identifies compatible devices by their advertised service UUID rather than relying exclusively on the device name.

### Measurement Payload

Environmental measurements are encoded in an eight-byte binary payload.

All multibyte values use little-endian byte ordering.

| Offset | Size | Data Type | Description |
|---|---|---|---|
| 0 | 2 bytes | `int16_t` | Temperature in °C × 100 |
| 2 | 2 bytes | `uint16_t` | Relative humidity in % × 100 |
| 4 | 4 bytes | `uint32_t` | Measurement sequence number |

### Example

A temperature of 22.45°C and relative humidity of 48.32% would be encoded as:

- Temperature: `2245`
- Humidity: `4832`
- Sequence: Incrementing 32-bit unsigned integer.

The gateway converts these integer values into floating-point measurements for display.

The sequence number helps verify that new measurements are being received.

## 5. OLED Display

The gateway uses the Adafruit SH1107 OLED FeatherWing with a 128×64 display.

### Display Configuration

The display is initialized using:

```cpp
Adafruit_SH1107 display(64, 128, &Wire, -1);
display.setRotation(1);
```

This configures the display for landscape operation.

### Displayed Information

The current implementation displays:

- Environmental sensor temperature.
- Environmental sensor relative humidity.
- Measurement sequence number.
- BLE connection status.
- Sensor data availability.

### Connection States

The gateway distinguishes between several monitoring states.

| State | Description |
|---|---|
| Offline | No active sensor connection |
| Waiting | Connected but awaiting measurement data |
| Connected | Receiving valid environmental measurements |
| Stale | No new measurement received within the timeout |

The stale measurement threshold is currently **60 seconds**.

A stale measurement does not automatically trigger a BLE disconnection.

This allows the gateway to distinguish between an interrupted measurement stream and a lost BLE connection.

## 6. Timing and Operation

The gateway uses periodic checks rather than blocking delays.

| Operation | Interval |
|---|---|
| BLE scan retry | 3 seconds |
| Sensor measurement interval | 30 seconds |
| Stale measurement threshold | 60 seconds |

The environmental sensor controls the measurement reporting interval.

The gateway receives measurements through BLE notifications.

### Reliability Testing

The gateway and environmental sensor have been tested together during extended operation.

Testing included:

- Gateway reset and reconnection.
- Environmental sensor reset and reconnection.
- Automatic recovery following BLE disconnection.
- Overnight continuous operation.
- Measurement sequence continuity.

An overnight test successfully reached measurement sequence number 1258 without reported communication problems.

## 7. Development Environment

### Requirements

- Particle Workbench.
- Visual Studio Code.
- Particle Device OS 6.4.1.
- Particle P2 / Photon 2.
- Adafruit SH1107 OLED FeatherWing.

### Libraries

The gateway uses the following display libraries:

| Library | Purpose |
|---|---|
| `Adafruit_SH110X_Z` | SH1107 OLED display driver |
| `Adafruit_GFX_RK_Z` | Graphics and text rendering |
| `Adafruit_BusIO_RK` | I²C communication support |

Library versions are managed through the project's `project.properties` file.

### Building the Firmware

Open the project directory in Visual Studio Code with Particle Workbench installed.

Select:

- Device platform: P2 