/*
VW Instrument Cluster Controller v1.0
Arduino and MCP2515 based controller for a 2018 Volkswagen UP instrument cluster

BSD 2-Clause License

Copyright (c) 2022, Andraž Vene
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <CAN.h>
#include <Chrono.h>

// Program settings
#define VERSION_STRING  "VW Instrument Cluster Controller v1.0"  // ID/Version message string

#define UPDATE_TIMEOUT  0    // Update timeout period after which instrument readings get set to initial state
#define MAIN_UPD_PERIOD 20      // Main CAN message sending period (RPM, Speed, Odometer,...)
#define AUX_UPD_PERIOD  100     // Aux CAN message sending period (Controll lights, temperatures,...)
#define TS_BLINK_PERIOD 500     // Turn signal indicator light blinking period

// Initial Values
#define INIT_SPEED      0       // Initial speed value
#define INIT_RPM        500     // Initial RPM value
#define INIT_CTP        25      // Initial Coolant Temp. value
#define INIT_OTP        25      // Initial Oil Temp. value
#define INIT_FUEL       75      // Initial fuel gauge value

// General IO definitions and settings
#define SWPW_OUT_PIN    A5      // Switched power controll output pin

const uint8_t fuel_gauge_pins[] = {3, 4, 5, 6, 7, 8, 10, 11, 12};

// CAN interface settings
#define MCP2515_CS      SS      // MCP2515 slave select pin
#define MCP2515_INT     2       // MCP2515 interrupt pin
#define MCP2515_FCLK    8000000 // MCP2515 clock frequency
#define CAN_BITRATE     500000  // CAN bus bitrate

// CAN packet IDs
#define SPD_CAN_ID      0x5A0   // Speed and distance counter packet ID
#define RPM_CAN_ID      0x280   // Engine speed packet ID
#define FCS_CAN_ID      0x480   // Fuel consumption packet ID
#define ABG_CAN_ID      0x050   // Airbag and seatbelt warning light packet ID
#define IND_CAN_ID      0x470   // Indicator lights packet ID
#define OTP_CAN_ID      0x588   // Oil temperature packet ID
#define CTP_CAN_ID      0x288   // Coolant temperature packet ID
#define SSM_CAN_ID      0x58C   // Start/Stop system message packet ID
// CAN packet buffers
uint8_t spd_pkt[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAD};   // speed and distance
uint8_t rpm_pkt[] = {0x49, 0x0E, 0x00, 0x00, 0x0E, 0x00, 0x1B, 0x0E};   // RPM
uint8_t fcs_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // Fuel consumption and some indicators
uint8_t abg_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // airbag and seatbelt status
uint8_t ind_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // indicator lights
uint8_t otp_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // oil temperature
uint8_t ctp_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // coolant temperature
uint8_t ssm_pkt[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};   // Start/Stop system message

// Coolant temperature conversion array (temperature cdoes from -45°c to 130°c in 5°c increments)
const uint8_t ctp_code[] = {
    0x01, 0x08, 0x0E, 0x15, 0x1C, 0x22, 0x29, 0x30, 0x36, 0x3D,
    0x44, 0x4A, 0x51, 0x58, 0x5F, 0x66, 0x6C, 0x72, 0x79, 0x80,
    0x85, 0x89, 0x8E, 0x92, 0x96, 0x9A, 0x9E, 0xA2, 0xD1, 0xD4,
    0xD7, 0xDB, 0xDE, 0xE1, 0xE6, 0xED
};

// Chrono object instantiations
Chrono chr_update_timeout;  // Update timeout timer
Chrono chr_main_update;     // Main CAN message sending loop timer
Chrono chr_aux_update;      // Aux CAN message sending loop timer
Chrono chr_ts_blinker;      // Turn signal light blinking loop timer

// Dashboard state variables
uint16_t rpm = 0;           // Engine speed in [RPM]
uint16_t spd = 0;           // Vehicle speed in [km/h]
uint16_t dst = 0;           // Distance counter [50 counts/m]
uint8_t ts_state = 0;       // Turn signal state (0...OFF, 1...LEFT, 2...RIGHT, 3...BOTH)

// Fuel consumption counter
uint16_t fc_counter = 0;    // Fuel consumption counter [0.25 ml/count]
uint16_t fc_period = 0;     // Fuel consumption count period [ms], 0 -> Stop counting

// ----------------------------------------------------------------------------
// Setup routine
// ----------------------------------------------------------------------------
void setup() {
    // Initialize IO
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(SWPW_OUT_PIN, OUTPUT);

    // Initialize fuel gauge resistor ladder pins 
    for (uint8_t i = 0; i < sizeof(fuel_gauge_pins); i++) {
        digitalWrite(fuel_gauge_pins[i], LOW);  // Make sure output value is low
        pinMode(fuel_gauge_pins[i], INPUT);     // Put all pin in Hi-Z mode
    }
    // Make one of fuel gauge pin low impedance and pulling low (half tank)
    pinMode(fuel_gauge_pins[4], OUTPUT);

    // Initialize USB serial port
    SerialUSB.begin(115200);
    delay(3000);

    // Setup CAN bus interface
    CAN.setPins(MCP2515_CS, MCP2515_INT);
    CAN.setClockFrequency(MCP2515_FCLK);
    // Initialize CAN bus
    while (true) {
        SerialUSB.print("Initializing CAN bus interface ... ");
        if (CAN.begin(CAN_BITRATE)) {
            SerialUSB.println("OK");
            break;
        } else {
            SerialUSB.println("FAIL");
            delay(1000);
        }
    }

    // Set some initial values
    setInitVal();
    setFuelLevel(INIT_FUEL);    // Only on power up
}

// ----------------------------------------------------------------------------
// Main program loop
// ----------------------------------------------------------------------------
void loop() {
    static uint8_t blink_step;
    static unsigned long fc_next_update;

    // Update timout check
    #if UPDATE_TIMEOUT
    if (chr_update_timeout.hasPassed(UPDATE_TIMEOUT)) {
        chr_update_timeout.restart();
        setInitVal();
    }
    #endif

    // Fuel consumption counter
    if (fc_period > 0) {
        if (millis() > (fc_next_update)) {
            fc_next_update += fc_period;
            fc_counter++;
        }
    }

    // Turn signal blinking loop
    if ((ts_state > 0) && (chr_ts_blinker.hasPassed(TS_BLINK_PERIOD))) {
        chr_ts_blinker.restart();
        if (blink_step) {
            setTurnSignal(0);
            blink_step = 0;
        } else {
            setTurnSignal(ts_state);
            blink_step = 1;
        }
    } else if (ts_state == 0) {
        setTurnSignal(0);
    }

    // If main CAN message update period has passed
    if (chr_main_update.hasPassed(MAIN_UPD_PERIOD)) {
        digitalWrite(LED_BUILTIN, HIGH);
        chr_main_update.restart();
        // Increment distance counter and constrain
        dst += spd * MAIN_UPD_PERIOD / 72;
        if (dst > 30000) dst -= 30000;
        setDist(dst);
        // Update fuel consumption counter
        setFcBytes(fc_counter);
        // Send CAN packets
        sendPacket(SPD_CAN_ID, spd_pkt, sizeof(spd_pkt));
        sendPacket(RPM_CAN_ID, rpm_pkt, sizeof(rpm_pkt));
        sendPacket(FCS_CAN_ID, fcs_pkt, sizeof(fcs_pkt));

        digitalWrite(LED_BUILTIN, LOW);
    }

    // If aux CAN message update pereiod has passed
    if (chr_aux_update.hasPassed(AUX_UPD_PERIOD)) {
        digitalWrite(LED_BUILTIN, HIGH);
        chr_aux_update.restart();
        // Send aux function can packets
        sendPacket(ABG_CAN_ID, abg_pkt, sizeof(abg_pkt));
        sendPacket(IND_CAN_ID, ind_pkt, sizeof(ind_pkt));
        sendPacket(OTP_CAN_ID, otp_pkt, sizeof(otp_pkt));
        sendPacket(CTP_CAN_ID, ctp_pkt, sizeof(ctp_pkt));
        sendPacket(SSM_CAN_ID, ssm_pkt, sizeof(ssm_pkt));

        digitalWrite(LED_BUILTIN, LOW);
    }

    // Receive incomming commands over USB serial
    getCommands();
}

// Set fuel consumption in [ml/s] (calculate and set fuel counter increment period)
void setFuelCons(float fc) {
    if (fc==0) {
        fc_period = 0;
    } else {
        fc_period = (uint32_t)(1000 / (4 * fc));
    }
}

// Set initial values
void setInitVal() {
    setRPM(INIT_RPM);
    setSpeed(INIT_SPEED);
    setOilTemp(INIT_OTP);
    setCoolantTemp(INIT_CTP);
    setFuelCons(0);
    // Lights out
    setTurnSignal(0);
    setFogLight(0);
    setHighBeam(0);
    setBatInd(0);
    setTirePressInd(0);
    setEpcInd(0);
    setDpfInd(0);
    setDoor(0);
    setDispMsg(0);
}

// ----------------------------------------------------------------------------
// CAN message sending routine
// ----------------------------------------------------------------------------

// Send CAN message (id, buffer, length)
uint8_t sendPacket(int16_t id, uint8_t *buf, uint8_t len) {
    CAN.beginPacket(id);
    CAN.write(buf, len);
    return CAN.endPacket();
}

// ----------------------------------------------------------------------------
// CAN message buffer modifying routines
// ----------------------------------------------------------------------------

// Set RPM value in rpm_pkt Bytes 2(LSB) and 3(MSB)
void setRPM(int16_t n) {
    rpm = n;
    rpm_pkt[2] = (uint8_t)((rpm * 4) & 0x00FF);     // update RPM low byte
    rpm_pkt[3] = (uint8_t)((rpm * 4) >> 8);         // update RPM high byte
}

// Set speed value [km/h] in spd_pkt Bytes 1(LSB) and 2(MSB)
void setSpeed(uint16_t v) {
    spd = v;
    spd_pkt[1] = (uint8_t)((spd * 148) & 0x00FF);   // update speed low byte
    spd_pkt[2] = (uint8_t)((spd * 148) >> 8);       // update speed high byte
}

// Update distance counter in spd_pkt Bytes 5(LSB) and 6(MSB)
void setDist(uint16_t d) {
    spd_pkt[5] = (uint8_t)(d & 0x00FF);   // update distance low byte
    spd_pkt[6] = (uint8_t)(d >> 8);       // update distance high byte
}

// Update fuel consumption counter: fcs_pkt Bytes 2(MSB) and 3(LSB)
void setFcBytes(uint16_t cnt) {
    fcs_pkt[3] = (uint8_t)(cnt & 0x00FF);   // Update fuel counter low byte
    fcs_pkt[2] = (uint8_t)(cnt >> 8);       // Update fuel counter high byte
}

// Set instrument cluster backlight level
void setBacklight(uint8_t lvl) {
    ind_pkt[2] = lvl;
}

// Set turn signal indicator (ind_pkt Byte 0: Bits 0 and 1)
void setTurnSignal(uint8_t ts) {
    ind_pkt[0] &= ~(0x03);
    ind_pkt[0] |= ts & 0x03;
}

// Set fog light indicator (ind_pkt Byte 7: Bit 5)
void setFogLight(uint8_t st) {
    if (st) ind_pkt[7] |= 0x20; else ind_pkt[7] &= ~(0x20);
}

// Set high beam indicator (ind_pkt Byte 7: Bit 6)
void setHighBeam(uint8_t st) {
    if (st) ind_pkt[7] |= 0x40; else ind_pkt[7] &= ~(0x40);
}

// Set cruise controll indicator (ctp_pkt Byte 2: Bit 7)
void setCruiseCtl(uint8_t st) {
    ctp_pkt[2] &= ~(0b10000000);
    ctp_pkt[2] |= (st<<7) & 0b10000000;
}

// Set Battery warning light
void setBatInd(uint8_t st) {
    ind_pkt[0] &= ~(0b10000000);
    ind_pkt[0] |= (st<<7) & 0b10000000;
    
}

// Set Tire Pressure warning light
void setTirePressInd(uint8_t st) {
    spd_pkt[3] &= ~(0b00001000);
    spd_pkt[3] |= (st<<3) & 0b00001000;
}

// Set EPC warning light
void setEpcInd(uint8_t st) {
    fcs_pkt[1] &= ~(0b00000100);
    fcs_pkt[1] |= (st<<2) & 0b00000100;
}

// Set DPF warning light
void setDpfInd(uint8_t st) {
    fcs_pkt[5] &= ~(0b00000010);
    fcs_pkt[5] |= (st<<1) & 0b00000010;
}

// Set door status indicator (ind_pkt Byte 1: Bits 0 to 5)
// Open door states (Bit0...FL, Bit1...FR, Bit2...RL, Bit3...RR, Bit4...Bonet, Bit5...Trunk)
void setDoor(uint8_t d) {
    ind_pkt[1] &= ~(0b00111111);
    ind_pkt[1] |= d & 0b00111111;
}

// Set seat belt indicator (abg_pkt Byte 2: Bit 2)
void setSeatbelt(uint8_t st) {
    abg_pkt[2] &= ~(0x04);
    abg_pkt[2] |= (st<<2)&0x04; 
}

// Set oil temperature in [°c] (otp_pkt Byte 7)
void setOilTemp(int t) {
    if ((t<195)&&(t>49)) {
        otp_pkt[7] = t + 60;
    } else {
        otp_pkt[7] = 0;
    }
}

// Set coolant temperature in [°c] (ctp_pkt Byte 1)
void setCoolantTemp(int t) {
    uint8_t idx = 0;
    if ((t<131)&&(t>-46)) {
        idx = (t+45)/5;
    }
    ctp_pkt[1] = ctp_code[idx];
}

// Set display message acording to ID parameter (See README.txt for available messages)
void setDispMsg(uint8_t msg_id) {
    if ((msg_id > 0)&&(msg_id < 16)) {
        ssm_pkt[0] = msg_id;
    } else {
        ssm_pkt[0] = 0;
    }
}

// ----------------------------------------------------------------------------
// Various IO setting routines
// ----------------------------------------------------------------------------

// Set "Switched power" controll output
void setSwPwr(uint8_t p) {
    if (p) {
        digitalWrite(SWPW_OUT_PIN, HIGH);
    } else {
        digitalWrite(SWPW_OUT_PIN, LOW);
    }
}

// Fuel level (0...Empty, 100...Full)
void setFuelLevel(int f) {
    // Determine which resistor ladder pin to pull low
    uint8_t fg_pin_idx = (uint8_t)(constrain(f/12, 0, 8));
    // Set apropriate fuel gauge pin to pull low and the rest high impedance (Hi-Z)
    for (uint8_t i = 0; i < sizeof(fuel_gauge_pins); i++) {
        digitalWrite(fuel_gauge_pins[i], LOW);  // Make sure output value is low
        if (i==fg_pin_idx) {
            pinMode(fuel_gauge_pins[i], OUTPUT);     // Put pin in output mode
        } else {
            pinMode(fuel_gauge_pins[i], INPUT);     // Put pin in Hi-Z mode
        }
    }
}


// ----------------------------------------------------------------------------
// Incomming command processing routine
// ----------------------------------------------------------------------------
// Read incomming characters and parse command strings
void getCommands() {
    static String cmd_string;

    // While new data is available
    while (SerialUSB.available()) {
        digitalWrite(LED_BUILTIN, HIGH);
        // Get next character
        char next_char = SerialUSB.read();
        
        // When next character is command terimination character start processing command string
        if (next_char == '\n') {
            SerialUSB.print("In Command: ");
            SerialUSB.println(cmd_string);
            // Reset update timeout timer
            chr_update_timeout.restart();
            // Set start position to 0 and find first subcomand termination character
            int start_pos = 0;
            int end_pos = cmd_string.indexOf(';', start_pos);
            // Search for subcommands until none are left
            while (end_pos >= 0) {
                // Extract subcommand from command string and get rid of whitespace
                String sub_cmd = cmd_string.substring(start_pos, end_pos);
                sub_cmd.trim();
                // Set new start position and find termination character for next iteration
                start_pos = end_pos + 1;
                end_pos = cmd_string.indexOf(';', start_pos);
                // Execute subcomand
                execCmd(sub_cmd);
            }
            // Empty command string
            cmd_string = "";
        
        // Else append incomming character to command string
        // (also limit command lenght by droping extra characters)
        } else {
            if (cmd_string.length() < 64) {
                cmd_string += next_char;
            }
        }

        digitalWrite(LED_BUILTIN, LOW);
    }
}

// Execute individual sub commands
void execCmd(String cmd) {
    String c = "";
    String p = "";

    // Separate command in to command and parameter
    int dlm_pos = cmd.indexOf('=');
    if (dlm_pos > 0) {
        c = cmd.substring(0, dlm_pos);
        p = cmd.substring(dlm_pos+1);
    } else {
        c = cmd.substring(0, dlm_pos);
    }

    Serial.print("Cmd: ");
    Serial.print(c);
    Serial.print(", Param: ");
    Serial.println(p);

    // Select apropriate action
    if (c == "ID") {
        SerialUSB.println(VERSION_STRING);
    } else if (c == "v") {  // Set Speed
        setSpeed(p.toInt());
    } else if (c == "r") {  // Set RPM
        setRPM(p.toInt());
    } else if (c == "c") {  // Set Fuel consumption
        setFuelCons(p.toFloat());
    } else if (c == "F") {  // Set Fuel level
        setFuelLevel(p.toInt());
    } else if (c == "Tc") { // Set coolant temperature
        setCoolantTemp(p.toInt());
    } else if (c == "To") { // Set oil temperature
        setOilTemp(p.toInt());
    } else if (c == "t") {  // Set turn signal
        ts_state = p.toInt();
    } else if (c == "f") {  // Set Fog light indicator
        setFogLight(p.toInt());
    } else if (c == "h") {  // Set High beam indicator
        setHighBeam(p.toInt());
    } else if (c == "CC") {  // Set Cruise controll indicator
        setCruiseCtl(p.toInt());
    } else if (c == "BW") {  // Set Battery warning indicator
        setBatInd(p.toInt());
    } else if (c == "TP") {  // Set Tire Pressure indicator
        setTirePressInd(p.toInt());
    } else if (c == "EPC") { // Set EPC indicator
        setEpcInd(p.toInt());
    } else if (c == "DPF") { // Set DPF indicator
        setDpfInd(p.toInt());
    } else if (c == "b") {  // Set seat belt indicator
        setSeatbelt(p.toInt());
    } else if (c == "D") {  // Set open door status display
        setDoor(p.toInt());
    } else if (c == "p") {  // Set switched power state
        setSwPwr(p.toInt());
    } else if (c == "M") {  // Display message
        setDispMsg(p.toInt());
    } else if (c == "BL") { // Set Backlight level
        setBacklight(p.toInt());
    } else {                // Print "unknown "
        SerialUSB.println("Unknown subcommand");
    }

}
