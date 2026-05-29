#pragma once
#include <stdint.h>
#include "usb.h"

// GIP = Game Input Protocol, the proprietary protocol used by Xbox One and
// Xbox Series controllers. Unlike the HID controllers handled elsewhere in
// this driver, GIP devices are NOT USB HID: they expose a vendor specific
// interface, ship no HID report descriptor, must be powered on with a vendor
// command before they emit anything, and stream fixed binary frames instead
// of self describing HID reports.
//
// This header covers wired USB GIP controllers only.

// GIP gamepad interface identity (interface 0 on Xbox One / Series pads).
#define GIP_INTERFACE_CLASS     0xFF
#define GIP_INTERFACE_SUBCLASS  0x47
#define GIP_INTERFACE_PROTOCOL  0xD0

// GIP message command bytes (data[0] of every frame).
#define GIP_CMD_INPUT   0x20   // gamepad state frame
#define GIP_CMD_GUIDE   0x07   // dedicated guide/xbox button frame

// Byte offsets inside a 0x20 input frame.
#define GIP_INPUT_BUTTONS1   4   // menu/view + A/B/X/Y
#define GIP_INPUT_BUTTONS2   5   // dpad + LB/RB + stick clicks
#define GIP_INPUT_LTRIGGER   6   // uint16 LE, 0..1023
#define GIP_INPUT_RTRIGGER   8   // uint16 LE, 0..1023
#define GIP_INPUT_LX        10   // int16 LE
#define GIP_INPUT_LY        12   // int16 LE
#define GIP_INPUT_RX        14   // int16 LE
#define GIP_INPUT_RY        16   // int16 LE

// buttons1 bits
#define GIP_BTN_MENU   (1 << 2)  // Start
#define GIP_BTN_VIEW   (1 << 3)  // Back
#define GIP_BTN_A      (1 << 4)
#define GIP_BTN_B      (1 << 5)
#define GIP_BTN_X      (1 << 6)
#define GIP_BTN_Y      (1 << 7)

// buttons2 bits
#define GIP_BTN_DPAD_UP     (1 << 0)
#define GIP_BTN_DPAD_DOWN   (1 << 1)
#define GIP_BTN_DPAD_LEFT   (1 << 2)
#define GIP_BTN_DPAD_RIGHT  (1 << 3)
#define GIP_BTN_LB          (1 << 4)
#define GIP_BTN_RB          (1 << 5)
#define GIP_BTN_LS          (1 << 6)  // left stick click
#define GIP_BTN_RS          (1 << 7)  // right stick click

// guide frame bit (data[4] of a 0x07 frame). xpad masks bits 0-1.
#define GIP_BTN_GUIDE  0x03

// Power-on command. Sent on the interrupt OUT endpoint, this makes the pad
// start streaming 0x20 input frames. Layout: cmd, flags, sequence, payload
// length, payload(power=ON). Works for wired Xbox One and Series pads.
extern const uint8_t GIP_POWER_ON[5];

// Follow-up init required by Xbox One S / Elite 2 / Series pads before they
// stream input. Sent right after the power-on packet completes.
extern const uint8_t GIP_S_INIT[5];

// True if the given interface triple identifies a GIP gamepad.
bool GipIsInterface(uint8_t cls, uint8_t sub, uint8_t proto);

// Decode a 0x20 input frame into a ButtonsReport. guideState is the latched
// guide button value carried over from the most recent 0x07 frame, because
// GIP reports the guide button in a separate frame from everything else.
void GipDecodeInput(const uint8_t* data, ButtonsReport* out, uint8_t guideState);

// Extract the guide button bit from a 0x07 frame.
uint8_t GipDecodeGuide(const uint8_t* data);
