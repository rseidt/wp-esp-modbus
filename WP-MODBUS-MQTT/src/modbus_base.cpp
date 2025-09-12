/*
 modbus_base.cpp - Modbus functions
 Copyright (C) 2020 Germain Masse

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "modbus_base.h"

// TODO: add this to WifiManager

#define RXD 14 // aka D5
#define TXD 12 // aka D6
#define RTS NOT_A_PIN

#define MODBUS_BAUDRATE 9600
#define MODBUS_UNIT 1
#define MODBUS_RETRIES 2
#define MODBUS_SCANRATE 10 // in seconds

// instantiate ModbusMaster object
ModbusMaster modbus_client;

// The ESP32 has 3 hardware serial ports, the ESP8266 has only 1 which we use for debugging.
// So we do the modbus communication over Software Serial.
SoftwareSerial swSerial(RXD, TXD); // RX, TX

void preTransmission() {
  digitalWrite(RTS, 1);
}

void postTransmission() {
  digitalWrite(RTS, 0);
}

void initModbus() {

  swSerial.begin(9600);  // Using ESP32 UART2 for Modbus
  modbus_client.begin(MODBUS_UNIT, swSerial);

  // do we have a flow control pin?
  if (RTS != NOT_A_PIN) {
    // Init in receive mode
    pinMode(RTS, OUTPUT);
    digitalWrite(RTS, 0);

    // Callbacks allow us to configure the RS485 transceiver correctly
    modbus_client.preTransmission(preTransmission);
    modbus_client.postTransmission(postTransmission);
  }
}

bool getModbusResultMsg(ModbusMaster *node, uint8_t result) {
  String tmpstr2 = "";
  switch (result) {
    case node->ku8MBSuccess:
      return true;
      break;
      case node->ku8MBIllegalFunction:
      tmpstr2 += "Illegal Function";
      break;
    case node->ku8MBIllegalDataAddress:
      tmpstr2 += "Illegal Data Address";
      break;
    case node->ku8MBIllegalDataValue:
      tmpstr2 += "Illegal Data Value";
      break;
    case node->ku8MBSlaveDeviceFailure:
      tmpstr2 += "Slave Device Failure";
      break;
    case node->ku8MBInvalidSlaveID:
      tmpstr2 += "Invalid Slave ID";
      break;
    case node->ku8MBInvalidFunction:
      tmpstr2 += "Invalid Function";
      break;
    case node->ku8MBResponseTimedOut:
      tmpstr2 += "Response Timed Out";
      break;
    case node->ku8MBInvalidCRC:
      tmpstr2 += "Invalid CRC";
      break;
    default:
      tmpstr2 += "Unknown error: " + String(result);
      break;
  }
  log(LOG_LEVEL_ERROR, tmpstr2);
  return false;
}

bool writeModbusRegister(const char* register_name, uint16_t value) {
  log(LOG_LEVEL_WARNING, "Writing data");
  uint16_t register_id = -1;
  for (uint8_t i = 0; i < sizeof(registers) / sizeof(modbus_register_t); ++i) {
    if (strcmp(registers[i].name, register_name) == 0) {
      register_id = registers[i].id;
    }
  }
  if (register_id == -1) {
    log(LOG_LEVEL_ERROR, "Register name '"+String(register_name)+"' not found");
    return true;
  }
  for (uint8_t i = 1; i <= MODBUS_RETRIES + 1; ++i) {
    log(LOG_LEVEL_INFO, "Trial "+String(i)+"/"+String(MODBUS_RETRIES+1));
    uint8_t result;
    result = modbus_client.writeSingleRegister(register_id, value);
    if (getModbusResultMsg(&modbus_client, result)) {
      log(LOG_LEVEL_WARNING, "Data written: " + String(value) + ", Register ID: " + String(register_id));
      return true;
    }
  }
  // Time-out
  log(LOG_LEVEL_ERROR, "Time out");
  return false;
}


bool getModbusValue(uint16_t register_id, modbus_entity_t modbus_entity, uint16_t *value_ptr) {
  log(LOG_LEVEL_INFO, "Requesting data");
  for (uint8_t i = 1; i <= MODBUS_RETRIES + 1; ++i) {
    log(LOG_LEVEL_INFO, "Trial "+String(i)+"/"+String(MODBUS_RETRIES+1));
    switch (modbus_entity) {
      case MODBUS_TYPE_HOLDING:
        uint8_t result;
        result = modbus_client.readHoldingRegisters(register_id, 1);
        if (getModbusResultMsg(&modbus_client, result)) {
          *value_ptr = modbus_client.getResponseBuffer(0);
          log(LOG_LEVEL_INFO, "Data read: " + String(*value_ptr));
          return true;
        } else {
          log(LOG_LEVEL_ERROR, "Error reading data from register ID: " + String(register_id) + ". Tried "+String(i)+" of "+String(MODBUS_RETRIES+1) + " times.");
        }
        break;
      default:
        log(LOG_LEVEL_ERROR, "Unsupported Modbus entity type");
        value_ptr = nullptr;
        return false;
        break;
    }
  }
  // Time-out
  log(LOG_LEVEL_ERROR, "Time-out after "+String(MODBUS_RETRIES+1)+" trials");
  value_ptr = nullptr;
  return false;
}

String toBinary(uint16_t input) {
    String output;
    while (input != 0) {
      output = (input % 2 == 0 ? "0" : "1") + output;
      input /= 2;
    }
    return output;
}

bool decodeDiematicDecimal(uint16_t int_input, int8_t decimals, float *value_ptr) {
  log(LOG_LEVEL_INFO, "Decoding "+String(int_input)+" with "+String(decimals)+" decimal(s)");
  if (int_input == 65535) {
    value_ptr = nullptr;
    return false;
  } else {
    uint16_t masked_input = int_input & 0x7FFF;
    float output = static_cast<float>(masked_input);
    if (int_input >> 15 == 1) {
      output = -output;
    }
    *value_ptr = output / pow(10, decimals);
    log(LOG_LEVEL_INFO, "Decoded value: "+String(*value_ptr));
    return true;
  }
}

void readModbusRegisterToJson(uint16_t register_id, ArduinoJson::JsonVariant variant) {
  // searchin for register matching register_id
  const uint8_t item_nb = sizeof(registers) / sizeof(modbus_register_t);
  for (uint8_t i = 0; i < item_nb; ++i) {
    if (registers[i].id != register_id) {
      // not this one
      continue;
    } else {
      // register found
      log(LOG_LEVEL_INFO, "Register id="+String(registers[i].id)+" type=0x"+String(registers[i].type)+" name=" + String(registers[i].name));
      uint16_t raw_value;
      if (getModbusValue(registers[i].id, registers[i].modbus_entity, &raw_value)) {
        log(LOG_LEVEL_INFO, "Raw value: "+String(registers[i].name)+"="+String(raw_value));
        switch (registers[i].type) {
          case REGISTER_TYPE_U16:
            log(LOG_LEVEL_INFO, "Value: " + String(raw_value));
            variant[registers[i].name] = raw_value;
            break;
          case REGISTER_TYPE_DIEMATIC_ONE_DECIMAL:
            float final_value;
            if (decodeDiematicDecimal(raw_value, 1, &final_value)) {
              log(LOG_LEVEL_INFO, "Value: "+String(final_value));
              variant[registers[i].name] = final_value;
            } else {
              log(LOG_LEVEL_INFO, "Value: Invalid Diematic value");
            }
            break;
          case REGISTER_TYPE_BITFIELD:
            for (uint8_t j = 0; j < 16; ++j) {
              const char *bit_varname = registers[i].optional_param.bitfield[j];
              if (bit_varname == nullptr) {
                log(LOG_LEVEL_INFO, " [bit"+String(j)+"] end of bitfield reached");
                break;
              }
              const uint8_t bit_value = raw_value >> j & 1;
              log(LOG_LEVEL_INFO, " [bit"+String(j)+"] "+String(bit_varname)+"=" + String(bit_value));
              variant[bit_varname] = bit_value;
            }
            break;
          case REGISTER_TYPE_DEBUG:
            log(LOG_LEVEL_INFO, "Raw DEBUG value: "+String(registers[i].name)+"=" + String(raw_value) + " (0b" + toBinary(raw_value) + ")");
            break;
          default:
            log(LOG_LEVEL_ERROR, "Unsupported register type");
            break;
        }
      } else {
        log(LOG_LEVEL_ERROR, "Request failed!");
      }
      return;
    }
  }
}

void parseModbusToJson(ArduinoJson::JsonVariant variant) {
  log(LOG_LEVEL_INFO, "Parsing all Modbus registers");
  uint8_t item_nb = sizeof(registers) / sizeof(modbus_register_t);
  for (uint8_t i = 0; i < item_nb; ++i) {
    readModbusRegisterToJson(registers[i].id, variant);
  }
}
