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
  PD_TEST_ENUM_TOTAL = 13
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
