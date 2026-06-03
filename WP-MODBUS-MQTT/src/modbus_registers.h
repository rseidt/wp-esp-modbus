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
	// Temperatur-Block 51–55 (Rohwert ×10 wie temp_akt; ÷10 → °C) — alle noch Kandidaten (_v).
	// Quervergleich mit den alten 1-basierten Scans (old reg = new+1) im selben Vollheiz-Zustand
	// (Außentemp damals ~20 °C, jetzt 14 °C, also -6 °C / -60 roh):
	//   Reg 53: 150→91 (-60) = trackt die Umgebungs-Differenz exakt, liegt aber im Heizbetrieb
	//           ~5 °C UNTER Umgebung → Verdampfer-/Spulentemp (coiler_temp, DP23), NICHT Umgebung.
	//   Reg 51: ähnliches Verhalten (kalt im Heizbetrieb) → zweite Spulen-/Verdampferseite.
	//   Reg 52/54: warm (~20–27 °C), wasser-/kondensatorseitig.
	//   Reg 55: 16–17 °C (alt) → 14 °C (neu); trackt die Umgebung NICHT sauber → unklar.
	// FAZIT: KEIN Register liest echte Umgebungsluft (~20 alt / ~14 neu) → around_temp (DP26)
	// bleibt UNIDENTIFIZIERT. Block trotzdem als Kandidaten loggen, um coiler/effluent/return
	// per weiterer Diffs (Kühlbetrieb, andere Außentemp) endgültig zuzuordnen.
	{51, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_spule_v"},
	{53, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_spule2_v"},
	{52, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_wasser_v"},
	{54, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_wasser2_v"},
	{55, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "temp_55_v"},
	// Vermutete Status-/Last-Register — zur weiteren Identifikation über MQTT loggen.
	{41, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "frequenz_soll_v"},
	{48, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "kompressor_ist_v"},
	{64, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "luefter_v"},
	{75, MODBUS_TYPE_HOLDING, REGISTER_TYPE_U16, "kompressor_last_v"},
};

#endif // SRC_MODBUS_REGISTERS_H_
