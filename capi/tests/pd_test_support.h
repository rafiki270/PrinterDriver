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
 * Returns NULL for an unknown id.
 */
pd_printer* pd_add_printer_scripted(pd_driver* driver, const char* printer_id,
                                    const char* script_id);

/* What the scripted device behind this printer actually saw. */
size_t pd_test_print_data_bytes(pd_printer* printer);
size_t pd_test_cuts(pd_printer* printer);
int pd_test_received_contains(pd_printer* printer, const char* needle);

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
  PD_TEST_ENUM_TOTAL = 15
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
