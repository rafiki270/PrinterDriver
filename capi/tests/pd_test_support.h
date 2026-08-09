#ifndef PRINTERDRIVER_PD_TEST_SUPPORT_H
#define PRINTERDRIVER_PD_TEST_SUPPORT_H

/*
 * Test-only companions to pd.h, compiled into the ABI test binary and into nothing
 * else. Two things live here that must never ship inside printerdriver_capi:
 *
 *   1. pd_add_printer_scripted(), which attaches a printer whose transport is an
 *      in-process scripted device. A C test cannot describe such a transport — the
 *      factory is a C++ std::function — so the test target supplies it. Nothing in the
 *      library knows this function exists.
 *   2. the enum bridge, which reports the C++ side's member counts and names so the C
 *      test can compare them against pd.h's. The library already fails to build if the
 *      values drift; this catches a count or a spelling drifting on top of that.
 */

#include "printerdriver/pd.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * script_id selects the device's behaviour:
 *   "ok"       — GS ( H printer, healthy, answers everything. Jobs reach Done at
 *                CutFaultFree.
 *   "gsr1"     — queued GS r 1 fence only; jobs cap at CutProcessed.
 *   "silent"   — accepts bytes, never answers the completion marker: jobs end Unknown.
 *   "paperout" — reports paper out, so strict preflight refuses before any payload byte.
 *   "refuse"   — the connection itself fails.
 *
 * M14 — docs/cash-drawer.md. A drawer is a solenoid latch plus a microswitch, and each
 * id below holds one of the document's cases still:
 *   "drawer"              — documented Epson-style 24 V port, working switch, polarity
 *                           already calibrated. The whole sequence reaches
 *                           PD_DRAWER_OPEN_VERIFIED.
 *   "drawer-uncalibrated" — the same hardware before anybody measured which level
 *                           means open, so a reading carries the needs-calibration
 *                           marker instead of a state.
 *   "drawer-locked"       — the pulse goes out and the switch never moves:
 *                           PD_DRAWER_FAILED_TO_OPEN.
 *   "drawer-unknown-port" — an unclassified 6P6C socket. Every pulse is refused with
 *                           zero bytes written.
 *
 * M19 — docs/receipt-dsl.md "Degradation rules":
 *   "no-barcode"          — a healthy GS ( H printer whose profile has no GS k path, so a
 *                           `barcode` block in a DSL document is declared in the render
 *                           report and omitted instead of printed as literal text.
 * Returns NULL for an unknown id.
 */
pd_printer* pd_add_printer_scripted(pd_driver* driver, const char* printer_id,
                                    const char* script_id);

/* What the scripted device behind this printer actually saw. */
size_t pd_test_print_data_bytes(pd_printer* printer);
size_t pd_test_cuts(pd_printer* printer);
int pd_test_received_contains(pd_printer* printer, const char* needle);

/* M14. Pulses that reached the device, the microswitch's position, and an operator's
 * hand moving the drawer without the printer's help. */
size_t pd_test_drawer_kicks(pd_printer* printer);
int pd_test_drawer_is_open(pd_printer* printer);
void pd_test_set_drawer_open(pd_printer* printer, int open);

/* --- Scripted link behind a caller-supplied transport vtable ---------------------- */

/*
 * The other half of the custom-transport test (docs/compatibility-brief.md §25). The C
 * test writes the pd_transport_vtable itself — that is the thing under test — and this
 * object plays the printer on the far side of it.
 *
 * It exists because the responses have to come back on a DIFFERENT thread from the one
 * that called write(), which is both what pd.h requires and what every real Bluetooth
 * stack does (a CoreBluetooth delegate queue, an Android reader thread, a BlueZ recv
 * loop). A C11 test cannot portably spawn a thread, so the thread lives here, in C++,
 * next to the scripted device it is pumping. Nothing about the ABI under test is
 * hidden by that: the vtable, the ctx, the pd_add_printer_custom call and the assertions
 * are all in test_capi.c.
 *
 * Usage: create, pd_add_printer_custom with a vtable that forwards to
 * pd_test_link_connect/write/close, then pd_test_link_bind so the reader thread knows
 * which printer to feed. Destroy after pd_destroy.
 */
typedef struct pd_test_link pd_test_link;

/* script_id: "ok" (GS ( H, healthy) or "gsr1" (queued fence only). NULL for unknown. */
pd_test_link* pd_test_link_create(const char* script_id);
void pd_test_link_destroy(pd_test_link* link);

/* Tells the reader thread where to deliver bytes. Call after pd_add_printer_custom. */
void pd_test_link_bind(pd_test_link* link, pd_printer* printer);

/* Make the next connect fail, as an unpaired or out-of-range device would. */
void pd_test_link_refuse_connections(pd_test_link* link);

/* The three operations a vtable forwards to. */
int32_t pd_test_link_connect(pd_test_link* link);
int64_t pd_test_link_write(pd_test_link* link, const uint8_t* data, size_t size);
void pd_test_link_close(pd_test_link* link);

/* What the scripted printer on the far side saw. */
size_t pd_test_link_connects(pd_test_link* link);
size_t pd_test_link_closes(pd_test_link* link);
size_t pd_test_link_bytes_written(pd_test_link* link);
size_t pd_test_link_cuts(pd_test_link* link);
int pd_test_link_received_contains(pd_test_link* link, const char* needle);

/* --- M15: a loopback ESC/POS listener, for the discovery and auto-detection paths ---
 *
 * pd_discover and pd_auto_detect are the only calls in this ABI that need a real socket
 * on the far side rather than a transport factory: they sweep addresses, so there is no
 * printer handle to attach a scripted device to. A wrapper test cannot portably open a
 * TCP listener from Swift, Dart and C# alike and script an ESC/POS device behind it, so
 * the fixture lives here, next to the scripted device it is already pumping.
 *
 * script_id:
 *   "ok"     — answers DLE EOT, GS I ("EPOSN"/"TM-T88V"), GS ( H and GS r 1.
 *   "silent" — accepts the connection and answers nothing at all, which is the LAN
 *              module that does not forward status bytes and a real finding.
 * NULL for an unknown id.
 */
typedef struct pd_test_listener pd_test_listener;

pd_test_listener* pd_test_listener_start(const char* script_id);
/* The ephemeral loopback port it bound, or 0. */
uint16_t pd_test_listener_port(pd_test_listener* listener);
/* How many printable bytes ever reached the device behind it. The number a detection
 * test has to watch stay at zero. */
size_t pd_test_listener_print_data_bytes(pd_test_listener* listener);
/* Closes the socket without destroying the handle, so a test can turn a listener into a
 * refused port at a known address. */
void pd_test_listener_stop(pd_test_listener* listener);
void pd_test_listener_destroy(pd_test_listener* listener);

/* --- M16: the "acme.x-idle" reference completion method as C function pointers ------
 *
 * The made-up vendor idle scheme test_capi.c defines inline, exported so a WRAPPER test
 * can register it too: the host sends ESC 'x' + the job's four-character verification
 * token behind the payload, and an idle device (the "vendor-idle" script above) echoes
 * ESC 'y' + the same token.
 *
 * It lives here because pd_completion_method's callbacks are invoked on a core thread and
 * must answer there and then, and not every FFI runtime can serve such a callback from
 * its own code -- `dart:ffi` cannot, which is why the Dart wrapper's registration API
 * takes native function pointers (wrappers/dart/lib/src/custom_methods.dart). Without
 * these two symbols that wrapper's §16 surface could only be checked for refusals, never
 * driven end to end against a real fence.
 */
size_t pd_test_acme_fence_bytes(void* ctx, const char* job_token, uint8_t* out, size_t cap);
pd_match_result pd_test_acme_matcher(void* ctx, const uint8_t* data, size_t size);

/* --- M19: a reference formatter for the receipt-DSL render path ---------------------
 *
 * `{{ v | acme.stars }}` -> `***v***`. Exported for the same reason the two symbols above
 * are: pd_formatter's callback is invoked on the thread that renders the document and has
 * to answer there and then, which `dart:ffi` cannot do from Dart code, so the Dart
 * wrapper's registration API takes native function pointers. Without this symbol that
 * wrapper could bind pd_register_formatter but never watch a registered formatter reach
 * paper through pd_print_document_json — which is the whole point of M19.
 *
 * `ctx` is ignored, so a test may pass NULL.
 */
size_t pd_test_stars_formatter(void* ctx, const char* value, const char* args,
                               const char* locale, char* out, size_t cap,
                               int32_t* handled);

/* --- Enum bridge ------------------------------------------------------------------ */

typedef enum pd_test_enum {
  PD_TEST_ENUM_JOB_STATE = 0,
  PD_TEST_ENUM_CONFIDENCE = 1,
  PD_TEST_ENUM_DEVICE_EVENT = 2,
  PD_TEST_ENUM_FAILURE_REASON = 3,
  PD_TEST_ENUM_JOB_OUTCOME = 4,
  PD_TEST_ENUM_CUT = 5,
  PD_TEST_ENUM_PREFLIGHT = 6,
  PD_TEST_ENUM_PAYLOAD_KIND = 7,
  PD_TEST_ENUM_COMPLETION = 8,
  PD_TEST_ENUM_CUT_VARIANT = 9,
  PD_TEST_ENUM_ALIGNMENT = 10,
  PD_TEST_ENUM_CODE_PAGE = 11,
  PD_TEST_ENUM_BINARIZATION = 12,
  PD_TEST_ENUM_CONFIDENCE_GRADE = 13,
  PD_TEST_ENUM_COMPLETION_AUTHORITY = 14,
  /* M14 — docs/cash-drawer.md. */
  PD_TEST_ENUM_DRAWER_STATE = 15,
  PD_TEST_ENUM_DRAWER_PORT_STANDARD = 16,
  PD_TEST_ENUM_DRAWER_KICK_METHOD = 17,
  PD_TEST_ENUM_DRAWER_STATUS_METHOD = 18,
  /* M15 — docs/api.md §15. */
  PD_TEST_ENUM_PROFILE_SELECTION = 19,
  PD_TEST_ENUM_DETECTION_STATUS = 20,
  PD_TEST_ENUM_TOTAL = 21
} pd_test_enum;

/* Number of members the C++ enum has, straight from the core's kAll* arrays or from a
 * hand-count in the bridge; -1 for an unknown id. */
int pd_test_cpp_enum_count(pd_test_enum which);

/* The core's own spelling of a member, or NULL when the C++ enum has no to_string. */
const char* pd_test_cpp_enum_name(pd_test_enum which, int index);

/* The C++ member's numeric value at that index, or -1. */
int pd_test_cpp_enum_value(pd_test_enum which, int index);

/* The name pd_test_enum ids are printed with in failure messages. */
const char* pd_test_enum_label(pd_test_enum which);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRINTERDRIVER_PD_TEST_SUPPORT_H */
