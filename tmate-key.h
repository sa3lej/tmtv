/*
 * The sharing protocol uses the tmux 3.6 key numbers, independent of tmux's
 * internal key representation. tmux 3.8 moved special keys into typed ranges.
 * Keep protocol 5/6 peers compatible without exposing the new internal values.
 */
#ifndef TMATE_KEY_H
#define TMATE_KEY_H

#define TMATE_WIRE_BASE       0x10e000ULL
#define TMATE_WIRE_BSPACE     0x10e25aULL
#define TMATE_WIRE_USER       0x10f000ULL
#define TMATE_WIRE_NONE       0x000ff000000000ULL
#define TMATE_WIRE_UNKNOWN    0x000fe000000000ULL
#define TMATE_WIRE_MASK_KEY   0x000fffffffffffULL
/* Protocol 11: tagged scalar, never a process-local utf8_char cache handle.
 * The tag also disambiguates Unicode private-use scalars from special keys. */
#define TMATE_WIRE_UNICODE    0x000100000000ULL

static inline key_code
tmate_key_to_wire(key_code key, int protocol)
{
	key_code code = key & KEYC_MASK_KEY;
	key_code flags = key & (KEYC_MASK_MODIFIERS | KEYC_MASK_FLAGS);
	struct utf8_data ud;
	wchar_t wc;

	if (KEYC_IS_USER(key)) {
		if (code - KEYC_USER >= KEYC_NUSER)
			return (TMATE_WIRE_UNKNOWN);
		code = TMATE_WIRE_USER + code - KEYC_USER;
	} else if (KEYC_IS_TYPE(key, KEYC_TYPE_FUNCTION)) {
		if (code >= KEYC_FOCUS_IN && code <= KEYC_PASTE_END)
			code = TMATE_WIRE_BASE + code - KEYC_FOCUS_IN;
		else if (code >= KEYC_BSPACE && code <= KEYC_REPORT_LIGHT_THEME)
			code = TMATE_WIRE_BSPACE + code - KEYC_BSPACE -
			    (protocol == 5 ? 9 : 0);
		else
			return (TMATE_WIRE_UNKNOWN);
	} else if (!KEYC_IS_TYPE(key, KEYC_TYPE_UNICODE))
		return (TMATE_WIRE_UNKNOWN); /* Mouse transport is unsupported. */
	else if (protocol >= 11 && code > 0x7f && !(key & KEYC_LITERAL)) {
		utf8_to_data(code, &ud);
		if (utf8_towc(&ud, &wc) != UTF8_DONE || wc < 0x80 ||
		    wc > 0x10ffff || (wc >= 0xd800 && wc <= 0xdfff))
			return (TMATE_WIRE_UNKNOWN);
		code = TMATE_WIRE_UNICODE | (key_code)wc;
	}
	return (flags | code);
}

static inline key_code
tmate_key_from_wire(key_code key)
{
	key_code code = key & TMATE_WIRE_MASK_KEY;
	key_code flags = key & (KEYC_MASK_MODIFIERS | KEYC_MASK_FLAGS);
	struct utf8_data ud;
	utf8_char uc;
	key_code scalar;

	if (code == TMATE_WIRE_NONE)
		return (KEYC_NONE);
	if (code & TMATE_WIRE_UNICODE) {
		scalar = code & ~TMATE_WIRE_UNICODE;
		if (scalar < 0x80 || scalar > 0x10ffff ||
		    (scalar >= 0xd800 && scalar <= 0xdfff) ||
		    (flags & KEYC_LITERAL) ||
		    utf8_fromwc(scalar, &ud) != UTF8_DONE ||
		    utf8_from_data(&ud, &uc) != UTF8_DONE)
			return (KEYC_UNKNOWN);
		code = uc;
	} else if (code >= TMATE_WIRE_BASE && code < TMATE_WIRE_BASE + 5)
		code = KEYC_FOCUS_IN + code - TMATE_WIRE_BASE;
	else if (code >= TMATE_WIRE_BSPACE &&
	    code <= TMATE_WIRE_BSPACE + KEYC_REPORT_LIGHT_THEME - KEYC_BSPACE)
		code = KEYC_BSPACE + code - TMATE_WIRE_BSPACE;
	else if (code >= TMATE_WIRE_USER && code < TMATE_WIRE_USER + KEYC_NUSER)
		code = KEYC_USER + code - TMATE_WIRE_USER;
	else if (code == TMATE_WIRE_NONE)
		return (KEYC_NONE);
	else if (code == TMATE_WIRE_UNKNOWN || code > 0x10ffff ||
	    (code >= TMATE_WIRE_BASE && code < TMATE_WIRE_BSPACE))
		return (KEYC_UNKNOWN);
	return (flags | code);
}
#endif
