#include "gip.h"

// cmd=0x05 (power), flags=0x20 (internal/system), seq=0x00, len=0x01, ON=0x00.
const uint8_t GIP_POWER_ON[5] = { 0x05, 0x20, 0x00, 0x01, 0x00 };

// Xbox One S / Series follow-up init (xpad: xboxone_s_init).
const uint8_t GIP_S_INIT[5] = { 0x05, 0x20, 0x00, 0x0f, 0x06 };

bool GipIsInterface(uint8_t cls, uint8_t sub, uint8_t proto) {
	return cls == GIP_INTERFACE_CLASS &&
		sub == GIP_INTERFACE_SUBCLASS &&
		proto == GIP_INTERFACE_PROTOCOL;
}

// GIP multi byte fields are little endian; the console is big endian, so read
// them by hand instead of dereferencing a uint16_t pointer.
static inline uint16_t rd16le(const uint8_t* p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

// 10 bit GIP trigger (0..1023) -> XInput 8 bit trigger (0..255).
static inline uint8_t scaleTrigger(uint16_t raw) {
	if (raw > 1023) raw = 1023;
	return (uint8_t)(((uint32_t)raw * 255u) / 1023u);
}

void GipDecodeInput(const uint8_t* data, ButtonsReport* out, uint8_t guideState) {
	uint8_t b1 = data[GIP_INPUT_BUTTONS1];
	uint8_t b2 = data[GIP_INPUT_BUTTONS2];

	out->a_button = (b1 & GIP_BTN_A) ? 1 : 0;
	out->b_button = (b1 & GIP_BTN_B) ? 1 : 0;
	out->x_button = (b1 & GIP_BTN_X) ? 1 : 0;
	out->y_button = (b1 & GIP_BTN_Y) ? 1 : 0;
	out->start    = (b1 & GIP_BTN_MENU) ? 1 : 0;
	out->back     = (b1 & GIP_BTN_VIEW) ? 1 : 0;

	out->l1 = (b2 & GIP_BTN_LB) ? 1 : 0;
	out->r1 = (b2 & GIP_BTN_RB) ? 1 : 0;
	out->l3 = (b2 & GIP_BTN_LS) ? 1 : 0;
	out->r3 = (b2 & GIP_BTN_RS) ? 1 : 0;

	// GIP gives us a raw dpad, not a HID hat.
	out->has_hat_switch = false;
	out->dpad_up    = (b2 & GIP_BTN_DPAD_UP) ? 1 : 0;
	out->dpad_down  = (b2 & GIP_BTN_DPAD_DOWN) ? 1 : 0;
	out->dpad_left  = (b2 & GIP_BTN_DPAD_LEFT) ? 1 : 0;
	out->dpad_right = (b2 & GIP_BTN_DPAD_RIGHT) ? 1 : 0;

	// Sticks are already full range int16 in XInput orientation (up positive).
	out->x  = (int16_t)rd16le(&data[GIP_INPUT_LX]);
	out->y  = (int16_t)rd16le(&data[GIP_INPUT_LY]);
	out->z  = (int16_t)rd16le(&data[GIP_INPUT_RX]);
	out->rz = (int16_t)rd16le(&data[GIP_INPUT_RY]);

	// XInputdReadStateHook reads triggers out of rx/ry as 0..255.
	out->rx = scaleTrigger(rd16le(&data[GIP_INPUT_LTRIGGER]));
	out->ry = scaleTrigger(rd16le(&data[GIP_INPUT_RTRIGGER]));

	// Analog triggers are authoritative; l2/r2 stay 0 so the hook uses rx/ry.
	out->l2 = 0;
	out->r2 = 0;

	out->xbox = guideState;
}

uint8_t GipDecodeGuide(const uint8_t* data) {
	return (data[GIP_INPUT_BUTTONS1] & GIP_BTN_GUIDE) ? 1 : 0;
}
