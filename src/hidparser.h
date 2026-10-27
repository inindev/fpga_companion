#include "usb_controller_maps.h"

#ifndef HIDPARSER_H
#define HIDPARSER_H

#include <stdint.h>
#include <stdbool.h>

#define REPORT_TYPE_NONE     0
#define REPORT_TYPE_MOUSE    1
#define REPORT_TYPE_KEYBOARD 2
#define REPORT_TYPE_JOYSTICK 3
#define REPORT_TYPE_CONSUMER 4

#define MAX_AXES 8
#define MAX_KBD_FIELDS 8

// a keyboard input field (usage page 7) from the report descriptor
typedef struct {
  uint16_t offset;      // bit offset inside report (without report id)
  uint16_t usage_min;   // usage of the first element (variable fields)
  uint16_t usage_max;
  uint8_t size;         // size of one element in bits
  uint8_t count;        // number of elements
  uint8_t variable: 1;  // 1: one bit per usage, 0: array of usage codes
} hid_kbd_field_t;

// currently only joysticks and mice are supported
typedef struct {
  uint8_t type: 3;               // REPORT_TYPE_...
  uint8_t report_id_present: 1;  // REPORT_TYPE_...
  uint8_t report_id;
  uint8_t report_size;

  union {
    struct {
      struct {
	uint16_t offset;  // bit offset inside report
	uint8_t size;     // size in bits
	int8_t index;     // index of axis inside report
	struct {
	  uint16_t min;
	  uint16_t max;
	} logical;
      } axis[MAX_AXES];               // x and y axis + wheel or right hat
      
      struct {
	uint8_t byte_offset;
	uint8_t bitmask;
       } button[32]; // 32 buttons max
      
      struct {
	uint16_t offset;
	uint8_t size;
	struct {
	  uint16_t min;
	  uint16_t max;
	} logical;
	struct {
	  uint16_t min;
	  uint16_t max;
	} physical;
      } hat;                   // 1 hat (joystick only)
    } joystick_mouse;
  };

  // keyboard layout, so boot style and one-bit-per-key reports both decode
  struct {
    uint8_t num_fields;
    hid_kbd_field_t field[MAX_KBD_FIELDS];
  } keyboard;

	/* --- SDL map base --- */
	const UsbGamepadMap *map; // NULL if no match; points to .rodata (flash)
	uint8_t map_found : 1;	  // 1=ok, 0=no
	uint8_t map_checked : 1;  // 1=already checked
	uint8_t _reserved_flags : 6;

} hid_report_t;

bool parse_report_descriptor(const uint8_t *rep, uint16_t rep_size, hid_report_t *conf, uint16_t *rbytes);
void fix_report_descriptor(uint16_t vid, uint16_t pid, uint16_t version, uint8_t *desc, uint16_t len);

#endif // HIDPARSER_H
