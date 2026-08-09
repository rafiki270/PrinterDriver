#ifndef PD_LINUX_BLUETOOTH_STUB_RFCOMM_H
#define PD_LINUX_BLUETOOTH_STUB_RFCOMM_H

/*
 * Hand-written stand-in for BlueZ's <bluetooth/rfcomm.h>. See the header comment in
 * bluetooth.h for what this is and is not.
 *
 * sockaddr_rc's field names and order are BlueZ's: rc_family, rc_bdaddr, rc_channel.
 * Getting them wrong is the single most common RFCOMM mistake — the address and the
 * channel are adjacent and the compiler is happy to accept them swapped when the types
 * are both integers. Here rc_bdaddr is a bdaddr_t and rc_channel a uint8_t, so it
 * cannot be.
 */

#include <bluetooth/bluetooth.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_rc {
  sa_family_t rc_family;
  bdaddr_t rc_bdaddr;
  uint8_t rc_channel;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PD_LINUX_BLUETOOTH_STUB_RFCOMM_H */
