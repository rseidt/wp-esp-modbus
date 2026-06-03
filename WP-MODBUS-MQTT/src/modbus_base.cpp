#include "modbus_base.h"
#include "modbus_faults.h"

// instantiate ModbusMaster object
ModbusMaster modbus_client;

uint16_t *register_values; // array to hold the register values
int num_registers = sizeof(registers) / sizeof(modbus_register_t);
int currentRegisterIndex = 0;
int currentTryIndex = 0;

String *modbusResultMsg;
uint8_t lastModbusResult = ModbusMaster::ku8MBSuccess;

bool modbus_poller_task_running = false;

// True für Fehler, die durch Buskollisionen mit dem Tuya-Master entstehen (verstümmelte/fremde Frames).
// Diese werden weggeworfen und sofort erneut versucht — der Slave selbst hat das Frame nie gesehen.
bool isTransientModbusError(uint8_t result)
{
	return result == ModbusMaster::ku8MBInvalidSlaveID
		|| result == ModbusMaster::ku8MBInvalidCRC
		|| result == ModbusMaster::ku8MBResponseTimedOut
		|| result == ModbusMaster::ku8MBInvalidFunction;
}

// The ESP32 has 3 hardware serial ports, the ESP8266 has only 1 which we use for debugging.
// So we do the modbus communication over Software Serial.
#ifdef ARDUINO_ARCH_ESP32
HardwareSerial modbusSerial(2); // Use UART2
#else
SoftwareSerial modbusSerial(RXD, TXD); // RX, TX
#endif

#if MODBUS_BAUDRATE > 19200
    uint32_t t3_5 = 1750;
#else
    uint32_t t3_5 = (1000000 * 39) / MODBUS_BAUDRATE + 500; // +500us : to be safe
#endif



void preTransmission()
{
	if (RTS != NOT_A_PIN)
	{
		digitalWrite(RTS, 1);
	}
}

void postTransmission()
{
	if (RTS != NOT_A_PIN)
	{
		digitalWrite(RTS, 0);
	}
}

// Idle-Callback der Modbus-Lib: wird waehrend des Wartens auf eine Antwort aufgerufen,
// solange keine Bytes anliegen. Ohne diesen Callback busy-wartet readHoldingRegisters()
// bis zu ku16MBResponseTimeout (2 s) OHNE yield -> bei mehreren Retams am Stueck (Block-Read
// unter Tuya-Buskollision) summieren sich die Wartezeiten > 5 s -> Task-Watchdog-Reset.
// delay(1) == vTaskDelay(1 Tick): laesst die niederpriore IDLE-Task laufen, die den
// Task-Watchdog zuruecksetzt. Greift waehrend echter Datenstroeme nicht (dann ist
// _serial->available() true), bremst die normale Kommunikation also nicht.
static void modbusIdle()
{
	delay(1);
}

void initModbus()
{

	register_values = new uint16_t[num_registers];
	modbusSerial.begin(MODBUS_BAUDRATE); // Using ESP32 UART2 for Modbus
	#ifdef ARDUINO_ARCH_ESP32
	modbusSerial.setPins(RXD, TXD);
	#endif
	modbus_client.begin(MODBUS_UNIT, modbusSerial);
	// do we have a flow control pin?
	if (RTS != NOT_A_PIN)
	{
		// Init in receive mode
		pinMode(RTS, OUTPUT);
		digitalWrite(RTS, 0);
	}
	modbus_client.preTransmission(preTransmission);
	modbus_client.postTransmission(postTransmission);
	modbus_client.idle(modbusIdle); // waehrend des Wartens an Scheduler abgeben (WDT-Schutz)
}

bool getModbusResultMsg(ModbusMaster *node, uint8_t result)
{
	lastModbusResult = result;
	String tmpstr2 = "";
	switch (result)
	{
	case node->ku8MBSuccess:
		modbusResultMsg = new String(tmpstr2);
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
	modbusResultMsg = new String(tmpstr2);
	return false;
}

String getModbusState()
{
	if (modbusResultMsg != nullptr)
	{
		return *modbusResultMsg;
	}
	else
	{
		return "";
	}
}

bool writeModbusRegister(const char *register_name, uint16_t value)
{
	log(LOG_LEVEL_WARNING, "Writing data");
	delayMicroseconds(t3_5); // inter-frame delay for Modbus RTU
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
	// Write tolerieren Buskollisionen: bei transienten Fehlern bis MODBUS_RETRIES_BUS_COLLISION+1 Versuche.
	// Echte Slave-Fehler werden weiterhin nach MODBUS_RETRIES+1 Versuchen aufgegeben.
	uint16_t max_attempts = MODBUS_RETRIES_BUS_COLLISION + 1;
	for (uint16_t i = 1; i <= max_attempts; ++i)
	{
		log(LOG_LEVEL_INFO, "Trial " + String(i) + "/" + String(max_attempts));
		uint8_t result = modbus_client.writeSingleRegister(register_id, value);
		if (getModbusResultMsg(&modbus_client, result))
		{
			log(LOG_LEVEL_WARNING, "Data written: " + String(value) + ", Register ID: " + String(register_id));
			return true;
		}
		if (!isTransientModbusError(result) && i > MODBUS_RETRIES)
		{
			log(LOG_LEVEL_ERROR, "Permanent Modbus error (0x" + String(result, HEX) + "), giving up.");
			return false;
		}
	}
	log(LOG_LEVEL_ERROR, "Time out after " + String(max_attempts) + " attempts");
	return false;
}

bool getModbusValue(uint16_t register_id, modbus_entity_t modbus_entity, uint16_t *value_ptr)
{
	log(LOG_LEVEL_INFO, "Requesting data");
	delayMicroseconds(t3_5); // inter-frame delay for Modbus RTU
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

// Liest die Holding-Register [start_id .. start_id+count-1] block-weise in values[] und
// vermerkt pro Register in valid[], ob der Wert gueltig ist. Gedacht fuer den Webserver-Dump.
// Block-Reads (bis MODBUS_DUMP_CHUNK Register je Transaktion) statt Einzel-Reads → weniger
// Kollisionsfenster mit dem Tuya-Master und beschraenkte Worst-Case-Dauer.
// Rueckgabe: true, wenn alle Chunks erfolgreich gelesen wurden.
bool readHoldingRange(uint16_t start_id, uint16_t count, uint16_t *values, bool *valid)
{
	bool all_ok = true;
	uint16_t done = 0;
	while (done < count)
	{
		uint16_t chunk = count - done;
		if (chunk > MODBUS_DUMP_CHUNK)
		{
			chunk = MODBUS_DUMP_CHUNK;
		}
		bool chunk_ok = false;
		for (uint8_t attempt = 0; attempt <= MODBUS_DUMP_RETRIES && !chunk_ok; ++attempt)
		{
			delayMicroseconds(t3_5); // inter-frame delay for Modbus RTU
			uint8_t result = modbus_client.readHoldingRegisters(start_id + done, chunk);
			if (getModbusResultMsg(&modbus_client, result))
			{
				for (uint16_t i = 0; i < chunk; ++i)
				{
					values[done + i] = modbus_client.getResponseBuffer(i);
					valid[done + i] = true;
				}
				chunk_ok = true;
			}
			else if (!isTransientModbusError(result))
			{
				// Permanenter Fehler (z.B. Illegal Data Address im Block) → Retries sinnlos.
				break;
			}
			yield(); // zwischen den Versuchen abgeben, damit kein langer Span ohne yield entsteht
		}
		if (!chunk_ok)
		{
			log(LOG_LEVEL_WARNING, "Dump chunk starting at " + String(start_id + done) + " failed (result=0x" + String(lastModbusResult, HEX) + ")");
			for (uint16_t i = 0; i < chunk; ++i)
			{
				valid[done + i] = false;
			}
			all_ok = false;
		}
		done += chunk;
		yield(); // TCP/MQTT-Stacks bedienen und Watchdog fuettern
	}
	return all_ok;
}

// Liest die bekannten Fault-Register und haengt die AKTIVEN Geraetefehler an das
// uebergebene JSON (dasselbe Dokument wie die Registerwerte -> Topic .../data):
//   "fault_active": bool, "faults": ["flow_fault", "E03", ...]
// Nur gesetzte Bits werden aufgenommen (kein Dauer-Null-Ballast). Solange alle Adressen
// in modbus_faults.h auf FAULT_ADDR_TODO stehen, bleibt "faults" leer und es findet kein
// Bus-Zugriff statt. Sobald echte Adressen eingetragen sind, erscheinen gesetzte Bits
// automatisch als Code-Liste (semantisch z.B. "flow_fault", numerisch z.B. "E03").
void writeFaultStatusToJson(ArduinoJson::JsonVariant variant)
{
	JsonArray active = variant["faults"].to<JsonArray>();
	bool any = false;

	int num_fault_regs = sizeof(faultRegisters) / sizeof(fault_register_t);
	for (int i = 0; i < num_fault_regs; ++i)
	{
		const fault_register_t &fr = faultRegisters[i];
		if (fr.modbus_addr == FAULT_ADDR_TODO)
		{
			continue; // Adresse noch nicht identifiziert -> ueberspringen (kein Bus-Zugriff)
		}

		uint16_t val;
		if (!getModbusValue(fr.modbus_addr, MODBUS_TYPE_HOLDING, &val))
		{
			continue; // Lesefehler (z.B. Buskollision) -> diesen Zyklus auslassen
		}
		if (val == 0)
		{
			continue; // keine Fehlerbits gesetzt
		}

		for (uint8_t b = 0; b < fr.bit_count && b < 16; ++b)
		{
			if (!((val >> b) & 1))
			{
				continue;
			}
			if (fr.labels[b] != nullptr)
			{
				active.add(fr.labels[b]); // statisches Literal -> ArduinoJson speichert Zeiger
			}
			else if (fr.code_prefix != 0)
			{
				char buf[8];
				snprintf(buf, sizeof(buf), "%c%02u", fr.code_prefix, fr.first_code + b);
				active.add(String(buf)); // String erzwingt Kopie des fluechtigen Puffers
			}
			any = true;
		}
	}

	variant["fault_active"] = any;
}

bool fillRegisterValues()
{
	uint16_t raw_value;
	log(LOG_LEVEL_INFO, "Filling register " + String(registers[currentRegisterIndex].name) + " (index " + String(currentRegisterIndex) + "/" + String(num_registers - 1) + "); try " + String(currentTryIndex + 1));
	if (getModbusValue(registers[currentRegisterIndex].id, registers[currentRegisterIndex].modbus_entity, &raw_value))
	{
		register_values[currentRegisterIndex] = raw_value;
		log(LOG_LEVEL_INFO, "Filled register " + String(registers[currentRegisterIndex].name) + " with value " + String(raw_value));
		currentTryIndex = 0;
		currentRegisterIndex++;
	}
	else
	{
		// Retry-Budget abhängig vom Fehlertyp: bei Buskollision (Tuya-Master stört) viel mehr Versuche,
		// bei echten Slave-Fehlern (Illegal Function/Address/Value, Slave Device Failure) schnell aufgeben.
		int retry_budget = isTransientModbusError(lastModbusResult) ? MODBUS_RETRIES_BUS_COLLISION : MODBUS_RETRIES;
		log(LOG_LEVEL_WARNING, "Failed to read register " + String(registers[currentRegisterIndex].name) + " (try " + String(currentTryIndex + 1) + "/" + String(retry_budget + 1) + ", result=0x" + String(lastModbusResult, HEX) + ")");
		if (currentTryIndex < retry_budget)
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
