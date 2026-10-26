/* 
 * Project Home Monitor Gateway P2
 * Author: Cameron Haegle
 * Date: 10/24/2026
 * For comprehensive documentation and examples, please visit:
 * https://docs.particle.io/firmware/best-practices/firmware-template/
 */

#include "Particle.h"
#include "Adafruit_SH110X.h"

SYSTEM_MODE(SEMI_AUTOMATIC);
// Device OS 6.4.1 enables system threading by default.

// ============================================================
// Home Monitoring Network - Particle P2 BLE Gateway
//
// Hardware:
//   Particle P2
//   Adafruit SH1107 128x64 OLED FeatherWing
//
// Device OS: 6.4.1
//
// Sensor: HM-Garage-ENV
//
// OLED wiring:
//   SDA -> D0
//   SCL -> D1
//   VCC -> 3V3
//   GND -> GND
// ============================================================


// ============================================================
// Configuration
// ============================================================

constexpr uint8_t OLED_ADDRESS = 0x3C;

constexpr uint32_t BLE_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t OLED_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t NETWORK_STATUS_INTERVAL_MS = 10000;

// Measurements become stale after 60 seconds.
// Stale measurements do not cause a BLE disconnection.
constexpr uint32_t SENSOR_STALE_TIMEOUT_MS = 60000;

const char SENSOR_NAME[] = "HM-Garage-ENV";

const BleUuid SENSOR_SERVICE_UUID(
    "8b7e0001-6a2d-4c91-9e31-4f2a8c6d1000"
);

const BleUuid SENSOR_CHARACTERISTIC_UUID(
    "8b7e0002-6a2d-4c91-9e31-4f2a8c6d1000"
);


// ============================================================
// OLED
// ============================================================

// Native SH1107 orientation: 64x128.
// Rotation 1 produces 128x64 landscape.

Adafruit_SH1107 display(64, 128, &Wire, -1);

bool oledReady = false;


// ============================================================
// BLE
// ============================================================

BlePeerDevice sensorPeer;
BleCharacteristic sensorCharacteristic;

bool sensorConnected = false;
bool sensorDataValid = false;

uint32_t lastScanTime = 0;
uint32_t lastOLEDRefresh = 0;
uint32_t lastNetworkStatusLog = 0;
uint32_t lastSensorUpdate = 0;


// ============================================================
// Sensor Data
// ============================================================

float sensorTemperature = 0.0f;
float sensorHumidity = 0.0f;

uint32_t sensorSequence = 0;


// ============================================================
// BLE Notification Buffer
// ============================================================

// BLE callbacks can execute asynchronously.
//
// A mutex protects the payload shared between the callback
// and the main application loop.

os_mutex_t payloadMutex = nullptr;

uint8_t pendingPayload[8] = {0};

bool pendingPayloadAvailable = false;


// ============================================================
// Function Prototypes
// ============================================================

void initializeOLED();
void updateOLED();

void initializeBLE();
void scanForSensor();
bool connectToSensor(const BleScanResult& result);
void disconnectSensor();

void sensorNotificationHandler(
    const uint8_t* data,
    size_t len,
    const BlePeerDevice& peer,
    void* context
);

void processSensorPayload(
    const uint8_t* data,
    size_t length
);


// ============================================================
// OLED Initialization
// ============================================================

void initializeOLED()
{
    Serial.println("Initializing SH1107 OLED...");

    Wire.begin();
    Wire.setSpeed(CLOCK_SPEED_100KHZ);

    oledReady = display.begin(OLED_ADDRESS, true);

    if (!oledReady) {
        Serial.println("ERROR: OLED initialization failed");
        return;
    }

    display.setRotation(1);
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    display.setCursor(0, 0);
    display.println("HOME MONITOR");

    display.drawLine(
        0,
        12,
        display.width() - 1,
        12,
        SH110X_WHITE
    );

    display.setCursor(0, 24);
    display.println("Particle P2 Gateway");

    display.setCursor(0, 42);
    display.println("Starting BLE...");

    display.display();

    Serial.printf(
        "OLED initialized: %dx%d\n",
        display.width(),
        display.height()
    );
}


// ============================================================
// OLED Update
// ============================================================

void updateOLED()
{
    if (!oledReady) {
        return;
    }

    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    // --------------------------------------------------------
    // Header
    // --------------------------------------------------------

    display.setCursor(0, 0);
    display.print("HOME MONITOR");

    // W = Wi-Fi ready; C = Particle Cloud connected.
    display.setCursor(98, 0);
    display.print(WiFi.ready() ? "W" : "-");
    display.print(Particle.connected() ? "C" : "-");

    display.drawLine(
        0,
        12,
        127,
        12,
        SH110X_WHITE
    );

    // --------------------------------------------------------
    // BLE connection and data status
    // --------------------------------------------------------

    display.setCursor(0, 16);
    display.print("Garage: ");

    if (!sensorConnected) {
        display.println("Offline");
    }
    else if (!sensorDataValid) {
        display.println("Waiting");
    }
    else if (
        millis() - lastSensorUpdate >
        SENSOR_STALE_TIMEOUT_MS
    ) {
        display.println("Stale");
    }
    else {
        display.println("Connected");
    }

    // --------------------------------------------------------
    // Sensor measurements
    // --------------------------------------------------------

    if (sensorDataValid) {

        display.setCursor(0, 29);
        display.print("Temp: ");
        display.print(sensorTemperature, 2);
        display.println(" C");

        display.setCursor(0, 41);
        display.print("RH:   ");
        display.print(sensorHumidity, 2);
        display.println(" %");

        display.setCursor(0, 54);
        display.print("Seq: ");
        display.println(sensorSequence);
    }
    else {

        display.setCursor(0, 32);
        display.println("Waiting for data...");
    }

    display.display();
}


// ============================================================
// BLE Notification Callback
// ============================================================

void sensorNotificationHandler(
    const uint8_t* data,
    size_t len,
    const BlePeerDevice& peer,
    void* context
)
{
    if (data == nullptr || len != 8) {
        return;
    }

    if (payloadMutex == nullptr) {
        return;
    }

    // Copy received data into the shared buffer.
    // Do not perform OLED operations inside this callback.

    os_mutex_lock(payloadMutex);

    memcpy(pendingPayload, data, sizeof(pendingPayload));

    pendingPayloadAvailable = true;

    os_mutex_unlock(payloadMutex);
}


// ============================================================
// Decode Sensor Payload
// ============================================================

void processSensorPayload(
    const uint8_t* data,
    size_t length
)
{
    if (data == nullptr || length != 8) {
        return;
    }

    // Temperature: signed 16-bit, little-endian.

    uint16_t temperatureBits =
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);

    int16_t temperatureRaw =
        static_cast<int16_t>(temperatureBits);

    // Humidity: unsigned 16-bit, little-endian.

    uint16_t humidityRaw =
        static_cast<uint16_t>(data[2]) |
        (static_cast<uint16_t>(data[3]) << 8);

    // Sequence: unsigned 32-bit, little-endian.

    uint32_t sequence =
        static_cast<uint32_t>(data[4]) |
        (static_cast<uint32_t>(data[5]) << 8) |
        (static_cast<uint32_t>(data[6]) << 16) |
        (static_cast<uint32_t>(data[7]) << 24);

    sensorTemperature = temperatureRaw / 100.0f;
    sensorHumidity = humidityRaw / 100.0f;
    sensorSequence = sequence;

    sensorDataValid = true;
    lastSensorUpdate = millis();

    Serial.printf(
        "[Garage ENV] Temp: %.2f C | RH: %.2f%% | Seq: %lu\n",
        sensorTemperature,
        sensorHumidity,
        static_cast<unsigned long>(sensorSequence)
    );
}


// ============================================================
// BLE Initialization
// ============================================================

void initializeBLE()
{
    Serial.println("Initializing BLE...");

    BLE.on();

    Serial.println("BLE initialized");
}


// ============================================================
// BLE Scanning
// ============================================================

void scanForSensor()
{
    Serial.println();
    Serial.println("========== BLE SCAN ==========");

    constexpr size_t MAX_SCAN_RESULTS = 20;

    BleScanResult results[MAX_SCAN_RESULTS];

    int count = BLE.scan(
        results,
        MAX_SCAN_RESULTS
    );

    Serial.printf(
        "BLE devices found: %d\n",
        count
    );

    for (int i = 0; i < count; i++) {

        const BleScanResult& result = results[i];

        String deviceName =
            result.advertisingData().deviceName();

        String deviceAddress =
            result.address().toString();

        Serial.printf(
            "Device %d:\n"
            "  Name: %s\n"
            "  Address: %s\n"
            "  RSSI: %d dBm\n",
            i + 1,
            deviceName.length()
                ? deviceName.c_str()
                : "(none)",
            deviceAddress.c_str(),
            result.rssi()
        );

        // ----------------------------------------------------
        // Identify sensor using its advertised service UUID.
        //
        // The sensor advertises its service UUID in the
        // primary advertising packet and its name in the
        // scan-response packet.
        // ----------------------------------------------------

        BleUuid advertisedService;

        bool serviceFound =
            result.advertisingData().serviceUUID(
                &advertisedService,
                1
            ) > 0;

        if (
            serviceFound &&
            advertisedService == SENSOR_SERVICE_UUID
        ) {

            Serial.println();
            Serial.println(
                "HOME MONITORING SENSOR FOUND"
            );

            Serial.printf(
                "Address: %s\n",
                deviceAddress.c_str()
            );

            if (connectToSensor(result)) {
                return;
            }
        }
    }

    Serial.println(
        "No Home Monitoring sensors found"
    );
}


// ============================================================
// Connect to Sensor
// ============================================================

bool connectToSensor(const BleScanResult& result)
{
    Serial.println("Connecting to sensor...");

    sensorPeer = BLE.connect(result.address());

    if (!sensorPeer.connected()) {

        Serial.println("BLE connection failed");

        sensorConnected = false;

        return false;
    }

    Serial.println("BLE connected");

    // --------------------------------------------------------
    // Discover characteristics
    // --------------------------------------------------------

    BleCharacteristic characteristics[8];

    int count = sensorPeer.discoverAllCharacteristics(
        characteristics,
        8
    );

    bool characteristicFound = false;

    for (int i = 0; i < count; i++) {

        if (
            characteristics[i].UUID() ==
            SENSOR_CHARACTERISTIC_UUID
        ) {

            sensorCharacteristic = characteristics[i];

            characteristicFound = true;

            break;
        }
    }

    if (!characteristicFound) {

        Serial.println(
            "ERROR: Sensor characteristic not found"
        );

        sensorPeer.disconnect();

        sensorConnected = false;

        return false;
    }

    Serial.println(
        "Sensor characteristic discovered"
    );

    // --------------------------------------------------------
    // Register notification callback
    // --------------------------------------------------------

    sensorCharacteristic.onDataReceived(
        sensorNotificationHandler,
        nullptr
    );

    // --------------------------------------------------------
    // Subscribe to notifications
    // --------------------------------------------------------

    int resultCode =
        sensorCharacteristic.subscribe(true);

    if (resultCode != SYSTEM_ERROR_NONE) {

        Serial.printf(
            "ERROR: BLE subscribe failed: %d\n",
            resultCode
        );

        sensorPeer.disconnect();

        sensorConnected = false;

        return false;
    }

    sensorConnected = true;

    // Reset the data freshness state for this connection.
    // Previously received measurements remain available
    // for display until new measurements arrive.

    sensorDataValid = false;

    Serial.println(
        "Subscribed to sensor notifications"
    );

    updateOLED();

    return true;
}


// ============================================================
// Disconnect Handling
// ============================================================

void disconnectSensor()
{
    if (sensorPeer.connected()) {
        sensorPeer.disconnect();
    }

    sensorConnected = false;

    Serial.println("Sensor disconnected");

    updateOLED();
}


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(115200);

    waitFor(
        Serial.isConnected,
        10000
    );

    Serial.println();
    Serial.println("================================");
    Serial.println("HOME MONITORING NETWORK");
    Serial.println("Particle P2 BLE Gateway");
    Serial.println("Device OS 6.4.1");
    Serial.println("================================");

    // --------------------------------------------------------
    // Create mutex for BLE notification buffer
    // --------------------------------------------------------

    int mutexResult =
        os_mutex_create(&payloadMutex);

    if (mutexResult != 0) {

        Serial.printf(
            "ERROR: Mutex creation failed: %d\n",
            mutexResult
        );
    }

    // --------------------------------------------------------
    // Initialize hardware and BLE
    // --------------------------------------------------------

    initializeOLED();

    initializeBLE();

    // Request Wi-Fi and Particle Cloud connectivity after local
    // monitoring hardware is initialized. Do not wait for cloud.
    Serial.println("Starting Particle Cloud connection...");
    Particle.connect();

    updateOLED();

    // Allow immediate scanning on the first loop.

    lastScanTime =
        millis() - BLE_RETRY_INTERVAL_MS;

    Serial.println(
        "Gateway initialization complete"
    );
}


// ============================================================
// Main Loop
// ============================================================

void loop()
{
    // Device OS handles cloud reconnection in semi-automatic mode.
    // Report connectivity periodically without blocking BLE processing.
    if (millis() - lastNetworkStatusLog >= NETWORK_STATUS_INTERVAL_MS) {
        lastNetworkStatusLog = millis();
        Serial.printf("[Network] Wi-Fi: %s | Cloud: %s\n",
            WiFi.ready() ? "Ready" : "Offline",
            Particle.connected() ? "Connected" : "Offline");
    }

    // --------------------------------------------------------
    // Process pending BLE notification
    // --------------------------------------------------------

    if (payloadMutex != nullptr) {

        uint8_t payload[8];

        bool havePayload = false;

        os_mutex_lock(payloadMutex);

        if (pendingPayloadAvailable) {

            memcpy(
                payload,
                pendingPayload,
                sizeof(payload)
            );

            pendingPayloadAvailable = false;

            havePayload = true;
        }

        os_mutex_unlock(payloadMutex);

        if (havePayload) {

            processSensorPayload(
                payload,
                sizeof(payload)
            );
        }
    }

    // --------------------------------------------------------
    // Monitor actual BLE connection
    //
    // IMPORTANT:
    // Do not disconnect merely because measurements
    // have become stale.
    // --------------------------------------------------------

    if (sensorConnected) {

        if (!sensorPeer.connected()) {

            Serial.println(
                "BLE connection lost"
            );

            sensorConnected = false;
        }
    }

    // --------------------------------------------------------
    // Reconnect when disconnected
    // --------------------------------------------------------

    if (!sensorConnected) {

        if (
            millis() - lastScanTime >=
            BLE_RETRY_INTERVAL_MS
        ) {

            scanForSensor();

            // Record the time after scanning completes.

            lastScanTime = millis();
        }
    }

    // --------------------------------------------------------
    // Refresh OLED
    // --------------------------------------------------------

    if (
        millis() - lastOLEDRefresh >=
        OLED_REFRESH_INTERVAL_MS
    ) {

        lastOLEDRefresh = millis();

        updateOLED();
    }
}