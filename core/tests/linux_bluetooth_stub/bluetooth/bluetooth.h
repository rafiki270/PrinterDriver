#ifndef PD_LINUX_BLUETOOTH_STUB_BLUETOOTH_H
#define PD_LINUX_BLUETOOTH_STUB_BLUETOOTH_H

/*
 * Hand-written stand-in for BlueZ's <bluetooth/bluetooth.h>, used ONLY by
 * scripts/check_linux_bluetooth_syntax.sh to type-check core/src/transport_bluez.cpp on
 * a host without BlueZ. It is not a reimplementation and nothing links against it.
 *
 * The declarations below mirror the real ones in the ways that can catch a mistake:
 *
 *   - bdaddr_t is a packed six-byte structure, not an integer, so passing an address
 *     by value where BlueZ wants a pointer fails here exactly as it would there;
 *   - AF_BLUETOOTH / PF_BLUETOOTH and the BTPROTO_* protocol numbers carry BlueZ's own
 *     values, so socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM) is checked with
 *     three arguments of the right types;
 *   - str2ba/ba2str have BlueZ's signatures (const char*, bdaddr_t*) and (const
 *     bdaddr_t*, char*), which is the pair most often transposed.
 *
 * What it does not do: define behaviour, define the SDP or HCI layers, or claim to be
 * ABI-identical to any particular BlueZ release. The numeric values match BlueZ's
 * headers because a wrong constant is exactly the kind of error a stub should not hide,
 * but they are not used for anything at check time.
 */

#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef PF_BLUETOOTH
#define PF_BLUETOOTH AF_BLUETOOTH
#endif

#define BTPROTO_L2CAP 0
#define BTPROTO_HCI 1
#define BTPROTO_SCO 2
#define BTPROTO_RFCOMM 3
#define BTPROTO_BNEP 4
#define BTPROTO_CMTP 5
#define BTPROTO_HIDP 6
#define BTPROTO_AVDTP 7

typedef struct {
  uint8_t b[6];
} __attribute__((packed)) bdaddr_t;

/* BlueZ spells these exactly this way; the const-ness is the point of copying them. */
int str2ba(const char* str, bdaddr_t* ba);
int ba2str(const bdaddr_t* ba, char* str);
int bachk(const char* str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PD_LINUX_BLUETOOTH_STUB_BLUETOOTH_H */
