#include "modbus_base.h"

// TODO: add this to WifiManager
#ifdef ARDUINO_ARCH_ESP32
#define RXD 16 // aka D5
#define TXD 17 // aka D6
#else
#define RXD 14 // aka D5
#define TXD 12 // aka D6
#endif
#define RTS NOT_A_PIN

#define MODBUS_BAUDRATE 9600
#define MODBUS_UNIT 1
#define MODBUS_RETRIES 2
#define MODBUS_SCANRATE 10 // in seconds

// instantiate ModbusMaster object
ModbusMaster modbus_client;

uint16_t *register_values; // array to hold the register values
int num_registers = sizeof(registers) / sizeof(modbus_register_t);
int currentRegisterIndex = 0;
int currentTryIndex = 0;

bool modbus_poller_task_running = false;

// The ESP32 has 3 hardware serial ports, the ESP8266 has only 1 which we use for debugging.
// So we do the modbus communication over Software Serial.
#ifdef ARDUINO_ARCH_ESP32
HardwareSerial modbusSerial(2); // Use UART2
#else
SoftwareSerial modbusSerial(RXD, TXD); // RX, TX
#endif
void preTransmission()
{
	digitalWrite(RTS, 1);
}

void postTransmission()
{
	digitalWrite(RTS, 0);
}

void initModbus()
{

	register_values = new uint16_t[num_registers];
	modbusSerial.begin(9600); // Using ESP32 UART2 for Modbus
	modbus_client.begin(MODBUS_UNIT, modbusSerial);

	// do we have a flow control pin?
	if (RTS != NOT_A_PIN)
	{
		// Init in receive mode
		pinMode(RTS, OUTPUT);
		digitalWrite(RTS, 0);

		// Callbacks allow us to configure the RS485 transceiver correctly
		modbus_client.preTransmission(preTransmission);
		modbus_client.postTransmission(postTransmission);
	}
}

bool getModbusResultMsg(ModbusMaster *node, uint8_t result)
{
	String tmpstr2 = "";
	switch (result)
	{
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

bool writeModbusRegister(const char *register_name, uint16_t value)
{
	log(LOG_LEVEL_WARNING, "Writing data");
	uint16_t register_id = -1;
	for (uint8_t i = 0; i < sizeof(registers) / sizeof(modbus_register_t); ++i)
	{
		if (strcmp(registers[i].name, register_name) == 0)
		{
			register_id = registers[i].id;
		}
	}
	if (register_id == -1)
	{
		log(LOG_LEVEL_ERROR, "Register name '" + String(register_name) + "' not found");
		return true;
	}
	for (uint8_t i = 1; i <= MODBUS_RETRIES + 1; ++i)
	{
		log(LOG_LEVEL_INFO, "Trial " + String(i) + "/" + String(MODBUS_RETRIES + 1));
		uint8_t result;
		result = modbus_client.writeSingleRegister(register_id, value);
		if (getModbusResultMsg(&modbus_client, result))
		{
			log(LOG_LEVEL_WARNING, "Data written: " + String(value) + ", Register ID: " + String(register_id));
			return true;
		}
	}
	// Time-out
	log(LOG_LEVEL_ERROR, "Time out");
	return false;
}

bool getModbusValue(uint16_t register_id, modbus_entity_t modbus_entity, uint16_t *value_ptr)
{
	log(LOG_LEVEL_INFO, "Requesting data");
	switch (modbus_entity)
	{
	case MODBUS_TYPE_HOLDING:
		uint8_t result;
		result = modbus_client.readHoldingRegisters(register_id, 1);
		if (getModbusResultMsg(&modbus_client, result))
		{
			*value_ptr = modbus_client.getResponseBuffer(0);
			log(LOG_LEVEL_INFO, "Data read: " + String(*value_ptr));
			return true;
		}
		break;
	default:
		log(LOG_LEVEL_ERROR, "Unsupported Modbus entity type");
		break;
	}
	value_ptr = nullptr;
	return false;
}

bool fillRegisterValues()
{
	uint16_t raw_value;
	log(LOG_LEVEL_INFO, "Filling register " + String(registers[currentRegisterIndex].name) + " (index " + String(currentRegisterIndex) + "/" + String(num_registers - 1) + "); quantity of tries: " + String(currentTryIndex + 1) + "/" + String(MODBUS_RETRIES + 1));
	if (getModbusValue(registers[currentRegisterIndex].id, registers[currentRegisterIndex].modbus_entity, &raw_value))
	{
		register_values[currentRegisterIndex] = raw_value;
		log(LOG_LEVEL_INFO, "Filled register " + String(registers[currentRegisterIndex].name) + " with value " + String(raw_value));
		currentTryIndex = 0;
		currentRegisterIndex++;
	}
	else
	{
		log(LOG_LEVEL_ERROR, "Failed to read register " + String(registers[currentRegisterIndex].name));
		if (currentTryIndex < MODBUS_RETRIES)
		{
			currentTryIndex++;
		}
		else
		{
			log(LOG_LEVEL_ERROR, "Max retries reached for register " + String(registers[currentRegisterIndex].name) + ". Moving to next register.");
			register_values[currentRegisterIndex] = 0xFFFF; // indicate failure
			currentTryIndex = 0;
			currentRegisterIndex++;
		}
	}
	if (currentRegisterIndex < num_registers)
	{
		return false;
	}
	else
	{
		currentRegisterIndex = 0;
		currentTryIndex = 0;
		return true;
	}
}

String toBinary(uint16_t input)
{
	String output;
	while (input != 0)
	{
		output = (input % 2 == 0 ? "0" : "1") + output;
		input /= 2;
	}
	return output;
}

bool decodeDiematicDecimal(uint16_t int_input, int8_t decimals, float *value_ptr)
{
	log(LOG_LEVEL_INFO, "Decoding " + String(int_input) + " with " + String(decimals) + " decimal(s)");
	if (int_input == 65535)
	{
		value_ptr = nullptr;
		return false;
	}
	else
	{
		uint16_t masked_input = int_input & 0x7FFF;
		float output = static_cast<float>(masked_input);
		if (int_input >> 15 == 1)
		{
			output = -output;
		}
		*value_ptr = output / pow(10, decimals);
		log(LOG_LEVEL_INFO, "Decoded value: " + String(*value_ptr));
		return true;
	}
}

void writeRegisterValuesToJson(ArduinoJson::JsonVariant variant)
{
	// searchin for register matching register_id
	for (uint8_t i = 0; i < num_registers; ++i)
	{
		// register found
		log(LOG_LEVEL_INFO, "Register id=" + String(registers[i].id) + " type=0x" + String(registers[i].type) + " name=" + String(registers[i].name));
		if (register_values[i] != 0xFFFF)
		{
			log(LOG_LEVEL_INFO, "Raw value: " + String(registers[i].name) + "=" + String(register_values[i]));
			switch (registers[i].type)
			{
			case REGISTER_TYPE_U16:
				log(LOG_LEVEL_INFO, "Value: " + String(register_values[i]));
				variant[registers[i].name] = register_values[i];
				break;
			case REGISTER_TYPE_DIEMATIC_ONE_DECIMAL:
				float final_value;
				if (decodeDiematicDecimal(register_values[i], 1, &final_value))
				{
					log(LOG_LEVEL_INFO, "Value: " + String(final_value));
					variant[registers[i].name] = final_value;
				}
				else
				{
					log(LOG_LEVEL_INFO, "Value: Invalid Diematic value");
				}
				break;
			case REGISTER_TYPE_BITFIELD:
				for (uint8_t j = 0; j < 16; ++j)
				{
					const char *bit_varname = registers[i].optional_param.bitfield[j];
					if (bit_varname == nullptr)
					{
						log(LOG_LEVEL_INFO, " [bit" + String(j) + "] end of bitfield reached");
						break;
					}
					const uint8_t bit_value = register_values[i] >> j & 1;
					log(LOG_LEVEL_INFO, " [bit" + String(j) + "] " + String(bit_varname) + "=" + String(bit_value));
					variant[bit_varname] = bit_value;
				}
				break;
			case REGISTER_TYPE_DEBUG:
				log(LOG_LEVEL_INFO, "Raw DEBUG value: " + String(registers[i].name) + "=" + String(register_values[i]) + " (0b" + toBinary(register_values[i]) + ")");
				break;
			default:
				log(LOG_LEVEL_ERROR, "Unsupported register type");
				break;
			}
		}
		else
		{
			log(LOG_LEVEL_ERROR, "Request failed!");
		}
	}
}
