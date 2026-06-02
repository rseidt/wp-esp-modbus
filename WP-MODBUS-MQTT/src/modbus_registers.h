#ifndef SRC_MODBUS_REGISTERS_H_
#define SRC_MODBUS_REGISTERS_H_

#include "Arduino.h"

typedef enum
{
	MODBUS_TYPE_HOLDING = 0x00, /*!< Modbus Holding register. */
								//    MODBUS_TYPE_INPUT,                  /*!< Modbus Input register. */
								//    MODBUS_TYPE_COIL,                   /*!< Modbus Coils. */
								//    MODBUS_TYPE_DISCRETE,               /*!< Modbus Discrete bits. */
								//    MODBUS_TYPE_COUNT,
								//    MODBUS_TYPE_UNKNOWN = 0xFF
} modbus_entity_t;

typedef enum
{
	//    REGISTER_TYPE_U8 = 0x00,                   /*!< Unsigned 8 */
	REGISTER_TYPE_U16 = 0x01, /*!< Unsigned 16 */
	//    REGISTER_TYPE_U32 = 0x02,                  /*!< Unsigned 32 */
	//    REGISTER_TYPE_FLOAT = 0x03,                /*!< Float type */
	//    REGISTER_TYPE_ASCII = 0x04,                 /*!< ASCII type */
	REGISTER_TYPE_DIEMATIC_ONE_DECIMAL = 0x05,
	REGISTER_TYPE_BITFIELD = 0x06,
	REGISTER_TYPE_DEBUG = 0x07
} register_type_t;

typedef union
{
	const char *bitfield[16];
} optional_param_t;

typedef struct
{
	uint16_t id;
	modbus_entity_t modbus_entity; /*!< Type of modbus parameter */
	register_type_t type;		   /*!< Float, U8, U16, U32, ASCII, etc. */
	const char *name;
	optional_param_t optional_param;
} modbus_register_t;

const modbus_register_t registers[] = { //register IDs are zero-based, i.e. register 40001 has id 0
	{92, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "ein_aus"},
	{93, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "modus"},
	{132, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "sub_modus"},
	{50, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_akt"},
	{106, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_soll_kuehl"},
	{105, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_soll_heiz"},
	{108, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_soll_auto"},
	{39, MODBUS_TYPE_HOLDING, REGISTER_TYPE_BITFIELD, "status_bits", {.bitfield = {
		"wasserpumpe",       // Bit 0: bestätigt
		"kompressor_aktiv",  // Bit 1: bestätigt (Standby vs. Heizen)
		"status_bit2",       // Bit 2: unbekannt
		"status_bit3",       // Bit 3: vermutet 4-Wege-/Magnetventil
		"status_bit4",       // Bit 4: vermutet Heizmodus aktiv
		"status_bit5"        // Bit 5: vermutet Heizmodus aktiv
	}}},
	// Vermutete Sensor-/Status-Register — zur weiteren Identifikation über MQTT loggen
	{15, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_umgebung_v"},
	{17, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_sensor2_v"},
	{19, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_abgas_v"},
	{41, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "frequenz_soll_v"},
	{48, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "kompressor_ist_v"},
	{64, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "luefter_v"},
	{75, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "kompressor_last_v"},
};

#endif // SRC_MODBUS_REGISTERS_H_
