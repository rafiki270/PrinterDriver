/*
 * ABI-level tests for pd.h (docs/api.md §9), compiled as plain C11 so the header is
 * proven to be a C header rather than C++ that happens to parse. Everything a C test
 * cannot express on its own -- the scripted transport, the enum bridge -- comes from
 * pd_test_support.h.
 */

#include "printerdriver/pd.h"

#include "pd_test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ(actual, expected)                                                    \
  do {                                                                                \
    const long long pd_actual_ = (long long)(actual);                                \
    const long long pd_expected_ = (long long)(expected);                            \
    if (pd_actual_ != pd_expected_) {                                                 \
      fprintf(stderr,                                                                 \
              "%s:%d: CHECK_EQ failed: %s\n    actual:   %lld\n    expected: %lld\n", \
              __FILE__, __LINE__, #actual, pd_actual_, pd_expected_);                 \
      g_failures++;                                                                   \
    }                                                                                 \
  } while (0)

#define CHECK_STREQ(actual, expected)                                             \
  do {                                                                            \
    const char* pd_actual_ = (actual);                                           \
    const char* pd_expected_ = (expected);                                       \
    if (pd_actual_ == NULL || pd_expected_ == NULL ||                            \
        strcmp(pd_actual_, pd_expected_) != 0) {                                 \
      fprintf(stderr,                                                            \
              "%s:%d: CHECK_STREQ failed: %s\n    actual:   %s\n    expected: %s\n", \
              __FILE__, __LINE__, #actual, pd_actual_ ? pd_actual_ : "(null)",    \
              pd_expected_ ? pd_expected_ : "(null)");                           \
      g_failures++;                                                              \
    }                                                                            \
  } while (0)

/* --- a. End-to-end submit through a terminal Done, via the ABI against a scripted
 *        device -------------------------------------------------------------------- */

static void test_submit_reaches_terminal_done(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-ok", "ok");
  CHECK(printer != NULL);

  const char text[] = "END TO END VIA THE ABI";
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)text;
  payload.as.raw.size = sizeof(text) - 1;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-e2e-1";

  pd_job* job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);

  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  CHECK_EQ(result.confidence, PD_CONFIDENCE_CUT_FAULT_FREE);
  CHECK_EQ(result.reason, PD_REASON_NONE);
  /* Never a bare {success:true}: the claim says what class of evidence it rests on,
   * who made it and which command produced it. */
  CHECK_EQ(result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_EQ(result.authority, PD_AUTHORITY_PHYSICAL_PRINTER);
  CHECK_STREQ(result.method, "GS(H) fn48");
  CHECK_STREQ(pd_confidence_grade_letter(result.grade), "A");
  CHECK_EQ(pd_job_is_terminal(job), 1);
  CHECK_EQ(pd_job_current_state(job), PD_JOB_STATE_DONE_SOFTWARE);
  CHECK(pd_test_received_contains(printer, text));
  CHECK_EQ(pd_test_cuts(printer), 1);

  pd_destroy(driver);
}

/* --- a2. Verification identifiers: printed, resolvable, suppressible --------------- */

static void test_verification_identifier_round_trip(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-rvi", "ok");
  CHECK(printer != NULL);

  const char text[] = "VERIFY ME";
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)text;
  payload.as.raw.size = sizeof(text) - 1;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-rvi-1";

  pd_job* job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);

  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);

  const char* print_token = pd_job_print_token(job);
  const char* cut_token = pd_job_cut_token(job);
  CHECK_EQ(strlen(print_token), 4u);
  CHECK_EQ(strlen(cut_token), 4u);
  /* [2-char instance nonce][2-char sequence]: both tokens name this driver. */
  CHECK_EQ(strlen(pd_instance_nonce(driver)), 2u);
  CHECK_EQ(strncmp(print_token, pd_instance_nonce(driver), 2), 0);
  CHECK_EQ(strncmp(cut_token, pd_instance_nonce(driver), 2), 0);
  CHECK(strcmp(print_token, cut_token) != 0);

  /* Printed next to the order id, and resolvable from that paper back to the job. */
  char printed[32];
  snprintf(printed, sizeof(printed), "V:%s", print_token);
  CHECK(pd_test_received_contains(printer, printed));
  CHECK(pd_test_received_contains(printer, "ORDER: capi-rvi-1"));
  CHECK(pd_job_by_token(driver, print_token) == job);
  CHECK(pd_job_by_token(driver, cut_token) == job);
  CHECK(pd_job_by_token(driver, "!!!!") == NULL);

  /* Suppressed per job: the token is still minted and still resolves, but no ink. */
  pd_job_options quiet;
  memset(&quiet, 0, sizeof(quiet));
  quiet.key = "capi-rvi-2";
  quiet.suppress_verification_id = 1;
  pd_job* silent = pd_print(driver, printer, &payload, &quiet);
  CHECK(silent != NULL);
  CHECK_EQ(pd_job_await(driver, silent, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  char quiet_printed[32];
  snprintf(quiet_printed, sizeof(quiet_printed), "V:%s", pd_job_print_token(silent));
  CHECK(!pd_test_received_contains(printer, quiet_printed));
  CHECK(pd_job_by_token(driver, pd_job_print_token(silent)) == silent);

  pd_destroy(driver);
}

/* --- a3. Reprint banner toggle and margins ----------------------------------------- */

static void test_reprint_banner_toggle_and_margins(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-reprint", "ok");
  CHECK(printer != NULL);

  const char text[] = "BANNERLESS COPY";
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)text;
  payload.as.raw.size = sizeof(text) - 1;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-reprint-1";
  /* ESC J 40 before the content; the bottom figure is below the profile's blade
   * clearance, so the engine keeps its own floor. */
  options.top_feed_dots = 40;
  options.bottom_feed_dots = 10;

  pd_job* first = pd_print(driver, printer, &payload, &options);
  CHECK(first != NULL);
  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, first, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  CHECK(pd_test_received_contains(printer, "\x1B\x4A\x28"));  /* ESC J 40 */
  CHECK(pd_test_received_contains(printer, "\x1B\x4A\x78"));  /* ESC J 120, the floor */

  pd_reprint_options reprint;
  memset(&reprint, 0, sizeof(reprint));
  reprint.job = options;
  reprint.suppress_banner = 1;
  pd_job* copy = pd_force_reprint_opts(driver, printer, "capi-reprint-1", &reprint);
  CHECK(copy != NULL);
  CHECK(copy != first);
  CHECK_EQ(pd_job_await(driver, copy, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  CHECK_EQ(pd_job_attempt(copy), 2u);
  CHECK(!pd_test_received_contains(printer, "*** REPRINT / POSSIBLE DUPLICATE ***"));

  /* The default is still a banner: pd_force_reprint is the all-zeroes call. */
  pd_job* loud = pd_force_reprint(driver, printer, "capi-reprint-1", &options);
  CHECK(loud != NULL);
  CHECK_EQ(pd_job_await(driver, loud, 5000, &result), 1);
  CHECK(pd_test_received_contains(printer, "*** REPRINT / POSSIBLE DUPLICATE ***"));
  CHECK(pd_test_received_contains(printer, "PRINT ATTEMPT: 3"));

  pd_destroy(driver);
}

/* --- b. Same-key dedupe returns the same job, without a second print --------------- */

static void test_same_key_dedupe_returns_same_job(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-dedupe", "ok");
  CHECK(printer != NULL);

  const char first_text[] = "ONE COPY ONLY";
  const char second_text[] = "SHOULD NEVER PRINT";

  pd_payload first_payload;
  memset(&first_payload, 0, sizeof(first_payload));
  first_payload.kind = PD_PAYLOAD_RAW;
  first_payload.as.raw.bytes = (const uint8_t*)first_text;
  first_payload.as.raw.size = sizeof(first_text) - 1;

  pd_payload second_payload;
  memset(&second_payload, 0, sizeof(second_payload));
  second_payload.kind = PD_PAYLOAD_RAW;
  second_payload.as.raw.bytes = (const uint8_t*)second_text;
  second_payload.as.raw.size = sizeof(second_text) - 1;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-dedupe-1";

  pd_job* first = pd_print(driver, printer, &first_payload, &options);
  pd_job* second = pd_print(driver, printer, &second_payload, &options);
  CHECK(first != NULL);
  CHECK(second == first);

  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, first, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);

  pd_printer_drain(driver, printer);
  CHECK(pd_test_received_contains(printer, first_text));
  CHECK(!pd_test_received_contains(printer, second_text));
  CHECK_EQ(pd_test_cuts(printer), 1);

  /* The driver's own key index must resolve to the identical interned handle too. */
  pd_job* found = pd_find_job(driver, "capi-dedupe-1");
  CHECK(found == first);

  pd_destroy(driver);
}

/* --- c. An event callback receives the ordered JobState progression ---------------- */

typedef struct {
  pd_job_state states[16];
  size_t count;
} StateLog;

/* By value, which is what lets a consumer keep the event past the call (pd.h). */
static void recordState(pd_job* job, pd_job_event event, void* ctx) {
  (void)job;
  StateLog* log = (StateLog*)ctx;
  if (log->count < sizeof(log->states) / sizeof(log->states[0])) {
    log->states[log->count] = event.state;
  }
  log->count++;
}

static void test_event_callback_receives_ordered_progression(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-events", "ok");
  CHECK(printer != NULL);

  const char text[] = "STATE ORDER";
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)text;
  payload.as.raw.size = sizeof(text) - 1;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-events-1";

  pd_job* job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);

  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);

  /* pd.h documents pd_subscribe_job as replaying every recorded event synchronously on
   * the calling thread before returning, so subscribing after the job is already
   * terminal still yields the whole ordered history -- deterministically, with no
   * cross-thread wait needed in the test. */
  StateLog log;
  memset(&log, 0, sizeof(log));
  pd_subscribe_job(driver, job, recordState, &log);

  const pd_job_state expected[] = {
      PD_JOB_STATE_QUEUED,          PD_JOB_STATE_PREFLIGHT_OK,
      PD_JOB_STATE_SEND_STARTED,    PD_JOB_STATE_BYTES_SENT,
      PD_JOB_STATE_PRINT_CONFIRMED, PD_JOB_STATE_CUT_COMMAND_PROCESSED,
      PD_JOB_STATE_DONE_SOFTWARE};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  CHECK_EQ(log.count, expected_count);
  for (size_t i = 0; i < expected_count && i < log.count; ++i) {
    CHECK_EQ(log.states[i], expected[i]);
  }

  pd_destroy(driver);
}

/* --- d. Enum bridge: C and C++ agree on membership, values and spelling ------------ */

typedef struct {
  pd_test_enum which;
  int c_count;
} EnumExpectation;

static void test_enum_bridge_matches_pd_h(void) {
  const EnumExpectation table[] = {
      {PD_TEST_ENUM_JOB_STATE, PD_JOB_STATE_COUNT},
      {PD_TEST_ENUM_CONFIDENCE, PD_CONFIDENCE_COUNT},
      {PD_TEST_ENUM_DEVICE_EVENT, PD_DEVICE_EVENT_COUNT},
      {PD_TEST_ENUM_FAILURE_REASON, PD_REASON_COUNT},
      {PD_TEST_ENUM_JOB_OUTCOME, PD_OUTCOME_COUNT},
      {PD_TEST_ENUM_CUT, PD_CUT_COUNT},
      {PD_TEST_ENUM_PREFLIGHT, PD_PREFLIGHT_COUNT},
      {PD_TEST_ENUM_PAYLOAD_KIND, PD_PAYLOAD_KIND_COUNT},
      {PD_TEST_ENUM_COMPLETION, PD_COMPLETION_COUNT},
      {PD_TEST_ENUM_CUT_VARIANT, PD_CUT_VARIANT_COUNT},
      {PD_TEST_ENUM_ALIGNMENT, PD_ALIGN_COUNT},
      {PD_TEST_ENUM_CODE_PAGE, PD_CODE_PAGE_COUNT},
      {PD_TEST_ENUM_BINARIZATION, PD_BINARIZATION_COUNT},
      {PD_TEST_ENUM_CONFIDENCE_GRADE, PD_GRADE_COUNT},
      {PD_TEST_ENUM_COMPLETION_AUTHORITY, PD_AUTHORITY_COUNT},
      /* M14 — docs/cash-drawer.md. */
      {PD_TEST_ENUM_DRAWER_STATE, PD_DRAWER_STATE_COUNT},
      {PD_TEST_ENUM_DRAWER_PORT_STANDARD, PD_DRAWER_PORT_STANDARD_COUNT},
      {PD_TEST_ENUM_DRAWER_KICK_METHOD, PD_DRAWER_KICK_METHOD_COUNT},
      {PD_TEST_ENUM_DRAWER_STATUS_METHOD, PD_DRAWER_STATUS_METHOD_COUNT},
      /* M15 — docs/api.md §15. */
      {PD_TEST_ENUM_PROFILE_SELECTION, PD_PROFILE_SELECTION_COUNT},
      {PD_TEST_ENUM_DETECTION_STATUS, PD_DETECTION_STATUS_COUNT},
  };
  const size_t table_size = sizeof(table) / sizeof(table[0]);

  /* The sentinel itself is not a real enum id. */
  CHECK_EQ(pd_test_cpp_enum_count(PD_TEST_ENUM_TOTAL), -1);

  for (size_t i = 0; i < table_size; ++i) {
    const pd_test_enum which = table[i].which;
    const char* label = pd_test_enum_label(which);
    CHECK(label != NULL);

    const int cpp_count = pd_test_cpp_enum_count(which);
    if (cpp_count != table[i].c_count) {
      fprintf(stderr, "%s:%d: enum count mismatch for %s: cpp=%d pd.h=%d\n", __FILE__,
              __LINE__, label, cpp_count, table[i].c_count);
      g_failures++;
      continue;
    }
    for (int index = 0; index < cpp_count; ++index) {
      const int cpp_value = pd_test_cpp_enum_value(which, index);
      /* Every bridged enum lines up value-for-index with pd.h except CodePage, whose
       * values are the non-contiguous ESC t n operand; pd_code_page_at() is pd.h's own
       * iteration hook for exactly that case. */
      const int expected_value =
          which == PD_TEST_ENUM_CODE_PAGE ? (int)pd_code_page_at(index) : index;
      if (cpp_value != expected_value) {
        fprintf(stderr, "%s:%d: enum value mismatch for %s[%d]: cpp=%d pd.h=%d\n",
                __FILE__, __LINE__, label, index, cpp_value, expected_value);
        g_failures++;
      }
    }
  }

  /* Every bridged id has to be in that table, or a new enum would simply never be
   * checked by anything. */
  CHECK_EQ((int)table_size, (int)PD_TEST_ENUM_TOTAL);

  /* The fourteen enums with a spelling on both sides: compare name for name. */
  for (int i = 0; i < PD_JOB_STATE_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_JOB_STATE, i),
                pd_job_state_name((pd_job_state)i));
  }
  for (int i = 0; i < PD_CONFIDENCE_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_CONFIDENCE, i),
                pd_confidence_level_name((pd_confidence_level)i));
  }
  for (int i = 0; i < PD_DEVICE_EVENT_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DEVICE_EVENT, i),
                pd_device_event_name((pd_device_event)i));
  }
  for (int i = 0; i < PD_REASON_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_FAILURE_REASON, i),
                pd_failure_reason_name((pd_failure_reason)i));
  }
  for (int i = 0; i < PD_OUTCOME_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_JOB_OUTCOME, i),
                pd_job_outcome_name((pd_job_outcome)i));
  }
  for (int i = 0; i < PD_PAYLOAD_KIND_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_PAYLOAD_KIND, i),
                pd_payload_kind_name((pd_payload_kind)i));
  }
  for (int i = 0; i < PD_COMPLETION_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_COMPLETION, i),
                pd_completion_mechanism_name((pd_completion_mechanism)i));
  }
  for (int i = 0; i < PD_CUT_VARIANT_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_CUT_VARIANT, i),
                pd_cut_variant_name((pd_cut_variant)i));
  }
  for (int i = 0; i < PD_GRADE_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_CONFIDENCE_GRADE, i),
                pd_confidence_grade_name((pd_confidence_grade)i));
  }
  for (int i = 0; i < PD_AUTHORITY_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_COMPLETION_AUTHORITY, i),
                pd_completion_authority_name((pd_completion_authority)i));
  }
  for (int i = 0; i < PD_DRAWER_STATE_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DRAWER_STATE, i),
                pd_drawer_state_name((pd_drawer_state)i));
  }
  for (int i = 0; i < PD_DRAWER_PORT_STANDARD_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DRAWER_PORT_STANDARD, i),
                pd_drawer_port_standard_name((pd_drawer_port_standard)i));
  }
  for (int i = 0; i < PD_DRAWER_KICK_METHOD_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DRAWER_KICK_METHOD, i),
                pd_drawer_kick_method_name((pd_drawer_kick_method)i));
  }
  for (int i = 0; i < PD_DRAWER_STATUS_METHOD_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DRAWER_STATUS_METHOD, i),
                pd_drawer_status_method_name((pd_drawer_status_method)i));
  }
  for (int i = 0; i < PD_PROFILE_SELECTION_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_PROFILE_SELECTION, i),
                pd_profile_selection_name((pd_profile_selection)i));
  }
  for (int i = 0; i < PD_DETECTION_STATUS_COUNT; ++i) {
    CHECK_STREQ(pd_test_cpp_enum_name(PD_TEST_ENUM_DETECTION_STATUS, i),
                pd_detection_status_name((pd_detection_status)i));
  }

  /* The remaining five have no C++ to_string, so the bridge must say so plainly rather
   * than inventing a spelling. */
  const pd_test_enum unnamed[] = {PD_TEST_ENUM_CUT, PD_TEST_ENUM_PREFLIGHT,
                                  PD_TEST_ENUM_ALIGNMENT, PD_TEST_ENUM_CODE_PAGE,
                                  PD_TEST_ENUM_BINARIZATION};
  for (size_t i = 0; i < sizeof(unnamed) / sizeof(unnamed[0]); ++i) {
    CHECK(pd_test_cpp_enum_name(unnamed[i], 0) == NULL);
  }
}


/* --- g. Custom transport: the platform owns the link, the core owns the protocol ----
 *
 * docs/compatibility-brief.md §25. The vtable below is the thing under test: it is
 * written in C, exactly as a Bluetooth wrapper would write it, and forwards to a
 * scripted printer supplied by pd_test_support (which also owns the reader thread,
 * because a C11 test cannot portably spawn one and pd.h forbids feeding bytes from
 * inside write()).
 *
 * What this has to prove is not that bytes move. It is that a printer reached over a
 * link the core knows nothing about still gets the full ordered fence and still reports
 * the same grade a TCP printer would -- because the moment a transport could weaken the
 * completion story, every wrapper would become part of it. */

static int32_t scripted_connect(void* ctx) {
  return pd_test_link_connect((pd_test_link*)ctx);
}

static int64_t scripted_write(void* ctx, const uint8_t* data, size_t size) {
  return pd_test_link_write((pd_test_link*)ctx, data, size);
}

static void scripted_close(void* ctx) { pd_test_link_close((pd_test_link*)ctx); }

static void test_custom_transport_drives_a_whole_job(void) {
  pd_test_link* link = pd_test_link_create("ok");
  CHECK(link != NULL);
  if (link == NULL) {
    return;
  }
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    pd_test_link_destroy(link);
    return;
  }

  pd_transport_vtable vtable;
  memset(&vtable, 0, sizeof(vtable));
  vtable.connect = scripted_connect;
  vtable.write = scripted_write;
  vtable.close = scripted_close;
  vtable.description = "bt-spp:00:11:22:33:44:55";

  pd_printer* printer =
      pd_add_printer_custom(driver, &vtable, link, "xp-s260m", 576);
  CHECK(printer != NULL);
  if (printer == NULL) {
    fprintf(stderr, "pd_add_printer_custom: %s\n", pd_last_error(driver));
    pd_destroy(driver);
    pd_test_link_destroy(link);
    return;
  }
  /* The id is derived from the vtable's description, so two Bluetooth printers are
   * distinguishable without the caller inventing names. */
  CHECK(strstr(pd_printer_id(printer), "bt-spp") != NULL);
  pd_test_link_bind(link, printer);

  const char* text = "BLUETOOTH TICKET";
  pd_raw raw;
  raw.bytes = (const uint8_t*)text;
  raw.size = strlen(text);
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw = raw;

  pd_job_options options;
  memset(&options, 0, sizeof(options));
  options.key = "bt-job-1";

  pd_job* job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);
  if (job != NULL) {
    pd_job_result result;
    memset(&result, 0, sizeof(result));
    CHECK_EQ(pd_job_await(driver, job, 15000, &result), 1);
    CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
    /* The same claim a TCP-attached GS ( H printer makes. Not weaker for being
     * Bluetooth, and not stronger either. */
    CHECK_EQ(result.confidence, PD_CONFIDENCE_CUT_FAULT_FREE);
    CHECK_EQ(result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
    CHECK_EQ(result.authority, PD_AUTHORITY_PHYSICAL_PRINTER);
    CHECK_STREQ(result.method, "GS(H) fn48");
  }

  CHECK(pd_test_link_connects(link) >= 1);
  CHECK(pd_test_link_bytes_written(link) > 0);
  CHECK_EQ(pd_test_link_cuts(link), 1);
  CHECK_EQ(pd_test_link_received_contains(link, "BLUETOOTH TICKET"), 1);

  pd_destroy(driver);
  /* Closing the driver closes the link. On the Epson portables that matters twice over:
   * the radio stays paired and the single allowed connection slot is freed. */
  CHECK(pd_test_link_closes(link) >= 1);
  pd_test_link_destroy(link);
}

static void test_custom_transport_rejects_a_broken_registration(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  pd_transport_vtable empty;
  memset(&empty, 0, sizeof(empty));
  CHECK(pd_add_printer_custom(driver, &empty, NULL, NULL, 0) == NULL);
  CHECK(strlen(pd_last_error(driver)) > 0);

  pd_transport_vtable vtable;
  memset(&vtable, 0, sizeof(vtable));
  vtable.connect = scripted_connect;
  vtable.write = scripted_write;
  /* An unknown profile id is an error, never a silent downgrade to generic: a caller
   * that asked for a TM-T88VI and got an unknown-device profile would be told a weaker
   * completion story than it asked for, with nothing in the result explaining why. */
  CHECK(pd_add_printer_custom(driver, &vtable, NULL, "epson_tm_t99", 0) == NULL);
  CHECK(strlen(pd_last_error(driver)) > 0);

  /* Feeding a printer that does not exist, or one that is not a custom transport, is
   * answered rather than fatal. */
  CHECK_EQ(pd_transport_feed_bytes(NULL, (const uint8_t*)"x", 1), 0);
  CHECK_EQ(pd_transport_link_dropped(NULL, "gone"), 0);
  pd_printer* scripted = pd_add_printer_scripted(driver, "tcp-ish", "ok");
  CHECK(scripted != NULL);
  CHECK_EQ(pd_transport_feed_bytes(scripted, (const uint8_t*)"x", 1), 0);

  pd_destroy(driver);
}

static void test_zebra_and_brother_are_refused_without_writing_a_byte(void) {
  /* docs/compatibility-brief.md §16, §17. ZPL, CPCL and Brother Raster are not ESC/POS
   * at any level. The refusal must happen before the link is opened -- a refusal that
   * still wasted a roll would be worse than the bug it replaced. */
  const char* const refused[] = {"zebra_zq300_plus", "zebra_zq500", "zebra_zq600_plus",
                                 "brother_rj2000",   "brother_rj3000",
                                 "brother_rj4000"};
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
    pd_test_link* link = pd_test_link_create("ok");
    pd_driver* driver = pd_create(NULL);
    CHECK(link != NULL);
    CHECK(driver != NULL);
    if (link == NULL || driver == NULL) {
      if (driver != NULL) pd_destroy(driver);
      if (link != NULL) pd_test_link_destroy(link);
      continue;
    }
    pd_transport_vtable vtable;
    memset(&vtable, 0, sizeof(vtable));
    vtable.connect = scripted_connect;
    vtable.write = scripted_write;
    vtable.close = scripted_close;
    vtable.description = refused[i];

    pd_printer* printer = pd_add_printer_custom(driver, &vtable, link, refused[i], 576);
    CHECK(printer != NULL);
    if (printer != NULL) {
      pd_test_link_bind(link, printer);
      const char* text = "LABEL";
      pd_payload payload;
      memset(&payload, 0, sizeof(payload));
      payload.kind = PD_PAYLOAD_RAW;
      payload.as.raw.bytes = (const uint8_t*)text;
      payload.as.raw.size = strlen(text);

      pd_job* job = pd_print(driver, printer, &payload, NULL);
      CHECK(job != NULL);
      if (job != NULL) {
        pd_job_result result;
        memset(&result, 0, sizeof(result));
        CHECK_EQ(pd_job_await(driver, job, 15000, &result), 1);
        CHECK_EQ(result.outcome, PD_OUTCOME_FAILED);
        CHECK_EQ(result.reason, PD_REASON_UNSUPPORTED);
        CHECK_EQ(result.grade, PD_GRADE_E_TRANSPORT_ONLY);
        CHECK_EQ(result.authority, PD_AUTHORITY_TRANSPORT_ONLY);
        CHECK_STREQ(result.method, "none");
      }
    }
    /* Zero bytes, zero connections, zero cuts, zero paper. */
    CHECK_EQ(pd_test_link_bytes_written(link), 0);
    CHECK_EQ(pd_test_link_connects(link), 0);
    CHECK_EQ(pd_test_link_cuts(link), 0);

    pd_destroy(driver);
    pd_test_link_destroy(link);
  }
}

/* --- h. A+ and the provenance/language enums ---------------------------------------- */

static void test_grade_hierarchy_gained_a_plus(void) {
  /* docs/compatibility-brief.md §24. A+ is value 0 and A..E shifted by one, so the
   * enum's own ordering still reads strongest-first and a caller can compare grades
   * numerically. */
  CHECK_EQ(PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB, 0);
  CHECK_EQ(PD_GRADE_A_JOB_LEVEL_CONFIRMATION, 1);
  CHECK_EQ(PD_GRADE_E_TRANSPORT_ONLY, 5);
  CHECK_EQ(PD_GRADE_COUNT, 6);
  CHECK_STREQ(pd_confidence_grade_letter(PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB), "A+");
  CHECK_STREQ(pd_confidence_grade_letter(PD_GRADE_A_JOB_LEVEL_CONFIRMATION), "A");
  CHECK_STREQ(pd_confidence_grade_letter(PD_GRADE_B_ORDERED_DEVICE_RESPONSE), "B");
  CHECK_STREQ(pd_confidence_grade_letter(PD_GRADE_E_TRANSPORT_ONLY), "E");
  CHECK_STREQ(pd_confidence_grade_name(PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB),
              "APlus_DurableQueryableJob");

  /* docs/compatibility-brief.md §28 and §1. */
  CHECK_EQ(PD_PROVENANCE_COUNT, 3);
  CHECK_STREQ(pd_provenance_name(PD_PROVENANCE_DOCUMENTED), "Documented");
  CHECK_STREQ(pd_provenance_name(PD_PROVENANCE_PROBED), "Probed");
  CHECK_STREQ(pd_provenance_name(PD_PROVENANCE_UNVERIFIED), "Unverified");
  CHECK_EQ(PD_LANGUAGE_COUNT, 8);
  CHECK_STREQ(pd_command_language_name(PD_LANGUAGE_ESC_POS), "EscPos");
  CHECK_STREQ(pd_command_language_name(PD_LANGUAGE_ZPL), "Zpl");
  CHECK_STREQ(pd_command_language_name(PD_LANGUAGE_CPCL), "Cpcl");
  CHECK_STREQ(pd_command_language_name(PD_LANGUAGE_BROTHER_RASTER), "BrotherRaster");
  CHECK_STREQ(pd_command_language_name(PD_LANGUAGE_ESC_P), "EscP");
}

static void test_provenance_is_readable_per_printer(void) {
  /* docs/compatibility-brief.md §28 through the ABI. The point of the distinction is
   * that it is visible BEFORE anything is printed: an integrator can tell an Epson whose
   * fence is manufacturer-documented from a clone whose fence is a hopeful default, and
   * decide whether running a probe against the site's hardware is worth the trip. */
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  pd_transport_vtable vtable;
  memset(&vtable, 0, sizeof(vtable));
  vtable.connect = scripted_connect;
  vtable.write = scripted_write;
  vtable.close = scripted_close;

  struct {
    const char* profile;
    pd_provenance provenance;
    pd_completion_mechanism completion;
    pd_command_language language;
  } cases[] = {
      /* Epson: GS ( H fn 48 is in Epson's own model command table. */
      {"epson_tm_t88vi", PD_PROVENANCE_DOCUMENTED, PD_COMPLETION_GS_PAREN_H,
       PD_LANGUAGE_ESC_POS},
      {"epson_tm_p20ii", PD_PROVENANCE_DOCUMENTED, PD_COMPLETION_GS_PAREN_H,
       PD_LANGUAGE_ESC_POS},
      /* Xprinter, Rongta, Partner: advertised, never documented. §13, §14, §15. */
      {"xprinter_s_series", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_GS_R1,
       PD_LANGUAGE_ESC_POS},
      {"rongta_rp80", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_GS_R1, PD_LANGUAGE_ESC_POS},
      {"partner_rp110", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_GS_R1,
       PD_LANGUAGE_ESC_POS},
      /* The one interrogated unit: probed, which beats the datasheet either way. */
      {"xp-s260m", PD_PROVENANCE_PROBED, PD_COMPLETION_GS_PAREN_H, PD_LANGUAGE_ESC_POS},
      /* No fence at all, so nothing to have documented. */
      {"generic_unknown", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_NONE,
       PD_LANGUAGE_ESC_POS},
      /* Not ESC/POS at any level, and the language says so before anything is sent. */
      {"zebra_zq600_plus", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_NONE, PD_LANGUAGE_ZPL},
      {"brother_rj4000", PD_PROVENANCE_UNVERIFIED, PD_COMPLETION_NONE,
       PD_LANGUAGE_BROTHER_RASTER},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    vtable.description = cases[i].profile;
    pd_printer* printer =
        pd_add_printer_custom(driver, &vtable, NULL, cases[i].profile, 576);
    CHECK(printer != NULL);
    if (printer == NULL) {
      fprintf(stderr, "%s: %s\n", cases[i].profile, pd_last_error(driver));
      continue;
    }
    CHECK_EQ(pd_printer_completion(printer), cases[i].completion);
    CHECK_EQ(pd_printer_completion_provenance(printer), cases[i].provenance);
    CHECK_EQ(pd_printer_language(printer), cases[i].language);
  }

  /* A null handle claims the least rather than crashing. */
  CHECK_EQ(pd_printer_completion_provenance(NULL), PD_PROVENANCE_UNVERIFIED);
  pd_destroy(driver);
}

static void test_profile_ids_expose_the_whole_catalogue(void) {
  /* docs/compatibility-brief.md §26. A wrapper enumerates this instead of hardcoding
   * names, so an id missing here is a printer nobody can select. */
  const char* const* ids = pd_profile_ids();
  CHECK(ids != NULL);
  size_t count = 0;
  int saw_generic = 0, saw_epson = 0, saw_zebra = 0, saw_unknown = 0, saw_p20ii = 0;
  for (size_t i = 0; ids != NULL && ids[i] != NULL; ++i) {
    ++count;
    if (strcmp(ids[i], "generic") == 0) saw_generic = 1;
    if (strcmp(ids[i], "epson_tm_t88vi") == 0) saw_epson = 1;
    if (strcmp(ids[i], "zebra_zq600_plus") == 0) saw_zebra = 1;
    if (strcmp(ids[i], "generic_unknown") == 0) saw_unknown = 1;
    if (strcmp(ids[i], "epson_tm_p20ii") == 0) saw_p20ii = 1;
  }
  CHECK(count >= 80);
  CHECK_EQ(saw_generic, 1);
  CHECK_EQ(saw_epson, 1);
  CHECK_EQ(saw_zebra, 1);
  CHECK_EQ(saw_unknown, 1);
  CHECK_EQ(saw_p20ii, 1);
}

/* --- m. M13b: the print-queue addon through the ABI (docs/sdk-spec.md §12) ---------
 *
 * One happy path, over the same scripted device the direct-print tests use, because the
 * point being proved is that there is no second engine: a queued job takes the identical
 * path a pd_print job takes and therefore earns the identical claim from the identical
 * fence (§12 rule 3). The dedupe check is rule 2: the key is claimed in the driver's own
 * index at enqueue time, so a direct pd_print of the same key finds the queued job rather
 * than printing a second receipt. */

static void test_queue_addon_drains_through_the_same_engine(void) {
  pd_driver* driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  pd_printer* printer = pd_add_printer_scripted(driver, "capi-queue", "ok");
  CHECK(printer != NULL);

  pd_queue_policy policy;
  memset(&policy, 0, sizeof(policy));
  policy.hold_while_offline = 1;
  policy.max_depth = 64;
  policy.drain_order = PD_DRAIN_FIFO;

  pd_queue* queue = pd_queue_create(driver, &policy);
  CHECK(queue != NULL);
  if (queue == NULL) {
    pd_destroy(driver);
    return;
  }
  CHECK_EQ(pd_queue_is_paused(queue, "capi-queue"), 0);
  CHECK_EQ(pd_queue_is_blocked(queue, "capi-queue"), 0);
  CHECK_STREQ(pd_drain_order_name(PD_DRAIN_FIFO), "Fifo");
  CHECK_STREQ(pd_drain_order_name(PD_DRAIN_PRIORITY), "Priority");

  const char text[] = "QUEUED VIA THE ABI";
  pd_payload payload;
  memset(&payload, 0, sizeof(payload));
  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)text;
  payload.as.raw.size = sizeof(text) - 1;

  pd_queue_options options;
  memset(&options, 0, sizeof(options));
  options.key = "capi-queued-1";

  pd_job* job = pd_queue_enqueue(queue, printer, &payload, &options);
  CHECK(job != NULL);

  pd_job_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  CHECK_EQ(result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_EQ(result.authority, PD_AUTHORITY_PHYSICAL_PRINTER);
  CHECK_STREQ(result.method, "GS(H) fn48");
  CHECK(pd_test_received_contains(printer, text));

  /* Rule 2: pointer equality is how dedupe is visible through this ABI. */
  pd_job_options direct;
  memset(&direct, 0, sizeof(direct));
  direct.key = "capi-queued-1";
  pd_job* deduped = pd_print(driver, printer, &payload, &direct);
  CHECK(deduped == job);
  CHECK_EQ(pd_test_cuts(printer), 1);

  CHECK_EQ((int)pd_queue_pending(queue, NULL), 0);
  CHECK_EQ((int)pd_queue_pending(queue, "capi-queue"), 0);
  CHECK_EQ((int)pd_queue_expired_count(queue), 0);
  CHECK_EQ((int)pd_queue_overflow_count(queue), 0);
  pd_queue_tick(queue);

  pd_queue_pause(queue, "capi-queue");
  CHECK_EQ(pd_queue_is_paused(queue, "capi-queue"), 1);
  pd_queue_resume(queue, "capi-queue");
  CHECK_EQ(pd_queue_is_paused(queue, "capi-queue"), 0);

  /* Destroyed before the driver: the queue is the one handle this ABI hands to the
   * caller to free, because the addon is layered on the driver rather than owned by it. */
  pd_queue_destroy(queue);
  pd_destroy(driver);
}

/* --- M14. The cash drawer (docs/cash-drawer.md) ------------------------------------
 *
 * The ABI's whole claim is that a drawer answer is a state and not a boolean, so these
 * three exercise the three answers a caller actually has to tell apart: the switch
 * moved, the switch did not, and nothing was fired at all. */

static void test_drawer_sequence_over_the_abi(void) {
  pd_config config;
  pd_driver* driver = NULL;
  pd_printer* printer = NULL;
  pd_drawer_capabilities caps;
  pd_drawer_result result;
  pd_drawer_request request;

  memset(&config, 0, sizeof(config));
  driver = pd_create(&config);
  CHECK(driver != NULL);
  printer = pd_add_printer_scripted(driver, "drawer-1", "drawer");
  CHECK(printer != NULL);

  caps = pd_printer_drawer_capabilities(printer);
  CHECK_EQ(caps.present, PD_TRUE);
  CHECK_EQ((int)caps.standard, (int)PD_DRAWER_PORT_EPSON_24V_6P6C);
  CHECK_EQ((int)caps.method, (int)PD_DRAWER_KICK_EPSON_ESC_P);
  CHECK_EQ((int)caps.voltage, 24);
  CHECK_EQ((int)caps.sensor_pin, 3);
  CHECK_EQ((int)caps.channel_count, 2);
  CHECK_EQ(caps.kickable, PD_TRUE);
  CHECK_EQ((int)caps.electrical_provenance, (int)PD_PROVENANCE_DOCUMENTED);

  /* NULL request means the profile's own defaults. */
  result = pd_drawer_open(driver, printer, NULL);
  CHECK_EQ((int)result.previous_state, (int)PD_DRAWER_CLOSED);
  CHECK_EQ((int)result.state, (int)PD_DRAWER_OPEN_VERIFIED);
  CHECK_EQ((int)result.channel, 1);
  CHECK_EQ((int)result.pulse_ms, 200);
  CHECK_EQ(pd_test_drawer_is_open(printer), 1);
  CHECK_EQ((int)pd_test_drawer_kicks(printer), 1);
  CHECK_STREQ(pd_drawer_state_name(result.state), "OpenVerified");

  /* Step 1 of the sequence: a drawer that is already out is never pulsed again. */
  result = pd_drawer_open(driver, printer, NULL);
  CHECK_EQ((int)result.state, (int)PD_DRAWER_OPEN);
  CHECK_EQ((int)result.pulse_ms, 0);
  CHECK_EQ((int)pd_test_drawer_kicks(printer), 1);

  /* A locked drawer is a different printer, and a different answer. */
  {
    pd_printer* locked = pd_add_printer_scripted(driver, "drawer-2", "drawer-locked");
    CHECK(locked != NULL);
    memset(&request, 0, sizeof(request));
    request.channel = 1;
    request.pulse_ms = 120;
    result = pd_drawer_open(driver, locked, &request);
    CHECK_EQ((int)result.state, (int)PD_DRAWER_FAILED_TO_OPEN);
    CHECK_EQ((int)result.pulse_ms, 120);
    CHECK_EQ((int)pd_test_drawer_kicks(locked), 1);
    CHECK_STREQ(pd_drawer_state_name(result.state), "FailedToOpen");
  }

  pd_destroy(driver);
}

static void test_drawer_refusals_write_no_bytes(void) {
  pd_config config;
  pd_driver* driver = NULL;
  pd_printer* unclassified = NULL;
  pd_printer* zebra = NULL;
  pd_tcp_config tcp;
  pd_drawer_result result;

  memset(&config, 0, sizeof(config));
  driver = pd_create(&config);
  CHECK(driver != NULL);

  /* The giant-letters rule: a 6P6C socket nobody has classified gets no current. */
  unclassified = pd_add_printer_scripted(driver, "drawer-3", "drawer-unknown-port");
  CHECK(unclassified != NULL);
  CHECK_EQ(pd_printer_drawer_capabilities(unclassified).kickable, PD_FALSE);
  CHECK_EQ((int)pd_printer_drawer_capabilities(unclassified).standard,
           (int)PD_DRAWER_PORT_UNKNOWN);
  result = pd_drawer_open(driver, unclassified, NULL);
  CHECK_EQ((int)result.state, (int)PD_DRAWER_UNKNOWN);
  CHECK_EQ((int)result.pulse_ms, 0);
  CHECK_EQ((int)pd_test_drawer_kicks(unclassified), 0);

  /* A label printer has no drawer port and does not speak ESC/POS either. */
  memset(&tcp, 0, sizeof(tcp));
  tcp.printer_id = "zebra-1";
  tcp.host = "192.0.2.60";
  tcp.profile_id = "zebra_zq600_plus";
  zebra = pd_add_printer_tcp(driver, &tcp);
  CHECK(zebra != NULL);
  {
    const pd_drawer_capabilities caps = pd_printer_drawer_capabilities(zebra);
    CHECK_EQ(caps.present, PD_FALSE);
    CHECK_EQ((int)caps.method, (int)PD_DRAWER_KICK_UNSUPPORTED);
    CHECK_EQ(caps.kickable, PD_FALSE);
  }
  result = pd_drawer_open(driver, zebra, NULL);
  CHECK_EQ((int)result.state, (int)PD_DRAWER_UNKNOWN);
  CHECK_EQ((int)result.pulse_ms, 0);

  pd_destroy(driver);
}

static void test_drawer_polarity_calibration_over_the_abi(void) {
  pd_config config;
  pd_driver* driver = NULL;
  pd_printer* printer = NULL;
  pd_drawer_reading shut;
  pd_drawer_reading open;
  pd_drawer_reading after;

  memset(&config, 0, sizeof(config));
  driver = pd_create(&config);
  CHECK(driver != NULL);
  printer = pd_add_printer_scripted(driver, "drawer-4", "drawer-uncalibrated");
  CHECK(printer != NULL);
  CHECK_EQ(pd_drawer_polarity_calibrated(driver, printer), 0);

  /* The operator procedure: close it, read, open it, read, record which level meant
   * open. Nothing here pulses anything. */
  pd_test_set_drawer_open(printer, 0);
  shut = pd_drawer_read_sensor(driver, printer, 500);
  CHECK_EQ(shut.available, PD_TRUE);
  CHECK_EQ(shut.answered, PD_TRUE);
  CHECK_EQ(shut.needs_calibration, PD_TRUE);
  /* A level, and deliberately not a state. */
  CHECK_EQ((int)shut.state, (int)PD_DRAWER_UNKNOWN);
  CHECK_EQ(shut.pin_high, PD_FALSE);

  pd_test_set_drawer_open(printer, 1);
  open = pd_drawer_read_sensor(driver, printer, 500);
  CHECK_EQ(open.pin_high, PD_TRUE);
  CHECK_EQ((int)pd_test_drawer_kicks(printer), 0);

  CHECK_EQ(pd_drawer_calibrate_polarity(driver, printer, open.pin_high), 0 /* in-memory */);
  CHECK_EQ(pd_drawer_polarity_calibrated(driver, printer), 1);
  CHECK_EQ(pd_drawer_high_means_open(driver, printer), 1);

  after = pd_drawer_read_sensor(driver, printer, 500);
  CHECK_EQ(after.needs_calibration, PD_FALSE);
  CHECK_EQ((int)after.state, (int)PD_DRAWER_OPEN);

  pd_destroy(driver);
}

/* --- M15. Self-test, auto-detection and discovery through the ABI (docs/api.md §15) - */

static void test_self_test_prints_one_ticket_and_reports_the_detection(void) {
  pd_config config;
  memset(&config, 0, sizeof(config));
  pd_driver* driver = pd_create(&config);
  CHECK(driver != NULL);
  pd_printer* printer = pd_add_printer_scripted(driver, "bench", "ok");
  CHECK(printer != NULL);

  pd_self_test_result result;
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_self_test(driver, printer, NULL, &result), 1);

  /* The proof is the ordinary tri-state result of the ordinary engine. */
  CHECK_EQ(result.result.outcome, PD_OUTCOME_DONE);
  CHECK_EQ(result.result.confidence, PD_CONFIDENCE_CUT_FAULT_FREE);
  CHECK_EQ(result.result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_EQ(result.result.authority, PD_AUTHORITY_PHYSICAL_PRINTER);
  CHECK(result.job != NULL);
  CHECK(strncmp(result.key, "selftest-", 9) == 0);
  CHECK_EQ(strlen(result.print_token), 4u);
  CHECK(strstr(result.ticket_text, "PRINTERDRIVER SELF-TEST") != NULL);
  CHECK(strstr(result.ticket_text, "CHARSET") != NULL);
  CHECK_EQ(pd_test_cuts(printer), 1u);
  CHECK(pd_test_received_contains(printer, "V:") != 0);

  /* The detection summary the paper carries. */
  CHECK_EQ(result.detection.completion, PD_COMPLETION_GS_PAREN_H);
  CHECK_EQ(result.detection.grade_ceiling, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_EQ(result.detection.printable_width_dots, 576u);
  CHECK_EQ(result.detection.chars_per_line, 48u);
  CHECK_STREQ(result.detection.endpoint, "bench");
  CHECK(result.detection.provenance_summary != NULL);
  CHECK(strstr(result.detection.provenance_summary, "GS(H) fn48") != NULL);
  CHECK_EQ(result.detection.degradation_count, 0u);

  /* The same key twice prints once, exactly like every other job. */
  pd_self_test_options options;
  memset(&options, 0, sizeof(options));
  options.key = "selftest-fixed";
  pd_self_test_result again;
  memset(&again, 0, sizeof(again));
  CHECK_EQ(pd_self_test(driver, printer, &options, &again), 1);
  CHECK_EQ(pd_test_cuts(printer), 2u);
  pd_self_test_result third;
  memset(&third, 0, sizeof(third));
  CHECK_EQ(pd_self_test(driver, printer, &options, &third), 1);
  CHECK_EQ(pd_test_cuts(printer), 2u);
  CHECK(again.job == third.job);

  pd_destroy(driver);
}

typedef struct detect_tally {
  int count;
  int answered;
  int silent;
  int unreachable;
  char first_summary[256];
} detect_tally;

static void onDetected(const pd_detected_printer* printer, uint64_t completed,
                       uint64_t total, void* ctx) {
  detect_tally* tally = (detect_tally*)ctx;
  tally->count++;
  CHECK(completed >= 1u && completed <= total);
  if (printer->status == PD_DETECTION_ANSWERED) {
    tally->answered++;
    if (tally->first_summary[0] == '\0' && printer->summary.provenance_summary != NULL) {
      strncpy(tally->first_summary, printer->summary.provenance_summary,
              sizeof(tally->first_summary) - 1);
    }
  } else if (printer->status == PD_DETECTION_SILENT) {
    tally->silent++;
  } else if (printer->status == PD_DETECTION_UNREACHABLE) {
    tally->unreachable++;
  }
}

static void test_auto_detect_classifies_three_listeners_without_printing(void) {
  pd_test_listener* answering = pd_test_listener_start("ok");
  pd_test_listener* silent = pd_test_listener_start("silent");
  pd_test_listener* gone = pd_test_listener_start("ok");
  CHECK(answering != NULL);
  CHECK(silent != NULL);
  CHECK(gone != NULL);

  char answering_endpoint[64];
  char silent_endpoint[64];
  char refused_endpoint[64];
  snprintf(answering_endpoint, sizeof(answering_endpoint), "127.0.0.1:%u",
           (unsigned)pd_test_listener_port(answering));
  snprintf(silent_endpoint, sizeof(silent_endpoint), "127.0.0.1:%u",
           (unsigned)pd_test_listener_port(silent));
  snprintf(refused_endpoint, sizeof(refused_endpoint), "127.0.0.1:%u",
           (unsigned)pd_test_listener_port(gone));
  pd_test_listener_stop(gone); /* the port is now closed: a refusal, deterministically */

  const char* endpoints[4];
  endpoints[0] = answering_endpoint;
  endpoints[1] = silent_endpoint;
  endpoints[2] = refused_endpoint;
  endpoints[3] = NULL;

  pd_config config;
  memset(&config, 0, sizeof(config));
  pd_driver* driver = pd_create(&config);
  CHECK(driver != NULL);

  pd_auto_detect_options options;
  memset(&options, 0, sizeof(options));
  options.endpoints = endpoints;
  options.connect_timeout_ms = 500;
  options.response_timeout_ms = 150;
  options.status_timeout_ms = 120;
  options.identity_timeout_ms = 120;
  options.completion_timeout_ms = 200;

  detect_tally tally;
  memset(&tally, 0, sizeof(tally));
  CHECK_EQ(pd_auto_detect(driver, &options, onDetected, &tally), 3);
  CHECK_EQ(tally.count, 3);
  CHECK_EQ(tally.answered, 1);
  CHECK_EQ(tally.silent, 1);
  CHECK_EQ(tally.unreachable, 1);
  CHECK(strstr(tally.first_summary, "untrusted") != NULL);

  /* The whole point: not one printable byte reached either live device. */
  CHECK_EQ(pd_test_listener_print_data_bytes(answering), 0u);
  CHECK_EQ(pd_test_listener_print_data_bytes(silent), 0u);

  pd_destroy(driver);
  pd_test_listener_stop(answering);
  pd_test_listener_stop(silent);
  pd_test_listener_destroy(answering);
  pd_test_listener_destroy(silent);
  pd_test_listener_destroy(gone);
}

typedef struct discover_tally {
  int count;
  char ip[32];
  char hex[64];
} discover_tally;

static void onDiscovered(const pd_discovered_device* device, uint64_t completed,
                         uint64_t total, void* ctx) {
  discover_tally* tally = (discover_tally*)ctx;
  tally->count++;
  CHECK(completed >= 1u && completed <= total);
  CHECK_EQ(device->port9100_open, 1);
  strncpy(tally->ip, device->ip, sizeof(tally->ip) - 1);
  strncpy(tally->hex, device->dle_eot_hex, sizeof(tally->hex) - 1);
}

static void test_discover_sweeps_a_loopback_address_and_writes_only_dle_eot(void) {
  pd_test_listener* answering = pd_test_listener_start("ok");
  CHECK(answering != NULL);

  pd_config config;
  memset(&config, 0, sizeof(config));
  pd_driver* driver = pd_create(&config);
  CHECK(driver != NULL);

  pd_discover_options options;
  memset(&options, 0, sizeof(options));
  options.subnet_cidr = "127.0.0.1/32";
  options.port = pd_test_listener_port(answering);
  options.connect_timeout_ms = 500;
  options.response_timeout_ms = 300;

  discover_tally tally;
  memset(&tally, 0, sizeof(tally));
  CHECK_EQ(pd_discover(driver, &options, onDiscovered, &tally), 1);
  CHECK_EQ(tally.count, 1);
  CHECK_STREQ(tally.ip, "127.0.0.1");
  /* The scripted device's DLE EOT 1 answer: online, drawer pin high. */
  CHECK_STREQ(tally.hex, "16");
  CHECK_EQ(pd_test_listener_print_data_bytes(answering), 0u);

  /* A CIDR wider than /16 is a mistyped subnet, not a venue, and is refused. */
  pd_discover_options too_wide;
  memset(&too_wide, 0, sizeof(too_wide));
  too_wide.subnet_cidr = "10.0.0.0/8";
  CHECK_EQ(pd_discover(driver, &too_wide, NULL, NULL), -1);
  CHECK(strlen(pd_last_error(driver)) > 0);

  /* And the local subnet is either a CIDR or an honest empty string. */
  CHECK(pd_local_subnet(driver) != NULL);

  pd_destroy(driver);
  pd_test_listener_stop(answering);
  pd_test_listener_destroy(answering);
}

/* --- M16: custom completion method registration (docs/api.md §16) ------------------ */

/*
 * The made-up "acme.x-idle" vendor completion scheme, defined once here on the C side and
 * mirrored by the scripted device in pd_test_support: the host sends ESC 'x' + the job's
 * four-character verification token behind the payload, and an idle device echoes ESC 'y'
 * + the same token. The registered fence_bytes produces the query and the matcher
 * recognises the echo, correlating by the token exactly as GS ( H does.
 */
static size_t acme_fence_bytes(void* ctx, const char* job_token, uint8_t* out,
                               size_t cap) {
  (void)ctx;
  if (job_token == NULL || strlen(job_token) != 4 || cap < 6) {
    return cap + 1; /* over cap / malformed: the core fails the job Unknown, never truncates */
  }
  out[0] = 0x1B; /* ESC */
  out[1] = 0x78; /* 'x' */
  memcpy(out + 2, job_token, 4);
  return 6;
}

static pd_match_result acme_matcher(void* ctx, const uint8_t* data, size_t size) {
  pd_match_result result;
  size_t i;
  memset(&result, 0, sizeof(result));
  (void)ctx;
  for (i = 0; i + 1 < size; ++i) {
    if (data[i] == 0x1B && data[i + 1] == 0x79) { /* ESC 'y' ack */
      if (i + 6 > size) {
        result.kind = PD_MATCH_NEED_MORE; /* the four token bytes are still in flight */
        return result;
      }
      result.kind = PD_MATCH_MATCHED;
      memcpy(result.token, data + i + 2, 4);
      result.token[4] = '\0';
      return result;
    }
  }
  if (size > 0 && data[size - 1] == 0x1B) {
    result.kind = PD_MATCH_NEED_MORE; /* a lone trailing ESC may begin an ack */
    return result;
  }
  result.kind = PD_MATCH_NOT_MINE;
  return result;
}

static void test_custom_completion_method_earns_the_registered_grade(void) {
  pd_config config;
  pd_completion_method method;
  pd_op ops[1];
  pd_document doc;
  pd_payload payload;
  pd_job_options options;
  pd_job* job;
  pd_job_result result;
  const char* text = "ACME IDLE TICKET";
  const char* print_token;
  const char* cut_token;
  pd_printer* printer;
  pd_driver* driver;

  memset(&config, 0, sizeof(config));
  config.fsync_disabled = 1;
  driver = pd_create(&config);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  /* Register a made-up vendor completion method: grade A, physical printer. */
  memset(&method, 0, sizeof(method));
  method.id = "acme.x-idle";
  method.fence_bytes = acme_fence_bytes;
  method.matcher = acme_matcher;
  method.grade = PD_GRADE_A_JOB_LEVEL_CONFIRMATION;
  method.authority = PD_AUTHORITY_PHYSICAL_PRINTER;
  method.method_name = "acme.x-idle";
  CHECK_EQ(pd_register_completion_method(driver, &method), 1);
  /* A duplicate id is refused, and a record missing a matcher is refused. */
  CHECK_EQ(pd_register_completion_method(driver, &method), 0);
  method.id = "acme.broken";
  method.matcher = NULL;
  CHECK_EQ(pd_register_completion_method(driver, &method), 0);

  printer = pd_add_printer_scripted(driver, "capi-acme", "vendor-idle");
  CHECK(printer != NULL);
  if (printer == NULL) {
    pd_destroy(driver);
    return;
  }
  CHECK_EQ(pd_printer_completion(printer), PD_COMPLETION_VENDOR_IDLE);

  ops[0].kind = PD_OP_LINE;
  ops[0].text = text;
  ops[0].value = 0;
  doc.ops = ops;
  doc.count = 1;
  doc.code_page = PD_CODE_PAGE_PC437;
  payload.kind = PD_PAYLOAD_DOCUMENT;
  payload.as.document = doc;
  memset(&options, 0, sizeof(options));
  options.key = "acme-1";

  job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);
  if (job == NULL) {
    pd_destroy(driver);
    return;
  }

  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  /* The registered claim rides the result, attributed by id. */
  CHECK_EQ(result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_EQ(result.authority, PD_AUTHORITY_PHYSICAL_PRINTER);
  CHECK_STREQ(result.method, "acme.x-idle");
  CHECK_STREQ(pd_confidence_grade_letter(result.grade), "A");
  /* A vendor idle fence confirms print completion and the cut command, not a fault-free
   * blade, so the level caps at CutProcessed exactly as GS r 1 does. */
  CHECK_EQ(result.confidence, PD_CONFIDENCE_CUT_PROCESSED);
  CHECK_EQ(pd_job_current_state(job), PD_JOB_STATE_DONE_SOFTWARE);
  CHECK(pd_test_received_contains(printer, text));
  CHECK_EQ(pd_test_cuts(printer), 1);

  /* The custom fence promotes its per-job token to a resolvable RVI, exactly like
   * GS ( H (docs/api.md §14): the ticket resolves by token and `pdctl verify` attributes
   * it to this job the same way. */
  print_token = pd_job_print_token(job);
  cut_token = pd_job_cut_token(job);
  CHECK_EQ(strlen(print_token), 4u);
  CHECK_EQ(strlen(cut_token), 4u);
  CHECK(strcmp(print_token, cut_token) != 0);
  CHECK(pd_job_by_token(driver, print_token) == job);
  CHECK(pd_job_by_token(driver, cut_token) == job);

  pd_destroy(driver);
}

/* A VendorIdle profile whose method is not registered is refused Unsupported before a
 * byte reaches the link: the honest answer when nothing can confirm the job. */
static void test_unregistered_vendor_idle_is_unsupported(void) {
  pd_config config;
  pd_payload payload;
  pd_job_options options;
  pd_job* job;
  pd_job_result result;
  const char* raw = "hello";
  pd_printer* printer;
  pd_driver* driver;

  memset(&config, 0, sizeof(config));
  config.fsync_disabled = 1;
  driver = pd_create(&config);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  /* Deliberately no pd_register_completion_method. */
  printer = pd_add_printer_scripted(driver, "capi-acme-none", "vendor-idle");
  CHECK(printer != NULL);
  if (printer == NULL) {
    pd_destroy(driver);
    return;
  }

  payload.kind = PD_PAYLOAD_RAW;
  payload.as.raw.bytes = (const uint8_t*)raw;
  payload.as.raw.size = 5;
  memset(&options, 0, sizeof(options));
  options.key = "acme-none-1";
  job = pd_print(driver, printer, &payload, &options);
  CHECK(job != NULL);
  if (job == NULL) {
    pd_destroy(driver);
    return;
  }
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_FAILED);
  CHECK_EQ(result.reason, PD_REASON_UNSUPPORTED);
  CHECK_EQ(pd_test_print_data_bytes(printer), 0u);

  pd_destroy(driver);
}

static pd_probe_finding acme_classify(void* ctx, const uint8_t* response, size_t size) {
  pd_probe_finding finding;
  memset(&finding, 0, sizeof(finding));
  (void)ctx;
  (void)response;
  finding.answered = size > 0 ? 1 : 0;
  strcpy(finding.label, "acme-probe");
  return finding;
}

/* The other four pd_register_* entry points are wired and validate their inputs: the
 * probe step's non-printing rule is enforced at registration, and each point refuses a
 * record missing a required callback. */
static void test_other_registration_points_are_wired(void) {
  pd_config config;
  pd_driver* driver;
  pd_probe_step step;
  pd_formatter formatter;
  pd_drawer_kick_reg drawer;
  static const uint8_t printing_request[] = {'H', 'i'};  /* printable -> must be refused */
  static const uint8_t silent_request[] = {0x1B, 0x05};  /* ESC ENQ, all < 0x20 -> accepted */

  memset(&config, 0, sizeof(config));
  config.fsync_disabled = 1;
  driver = pd_create(&config);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  /* (2) probe step: a printing request is refused; the same step with non-printing bytes
   * is accepted. This is the printable-byte lint of §16, enforced at registration. */
  memset(&step, 0, sizeof(step));
  step.id = "acme.printing-probe";
  step.request_bytes = printing_request;
  step.request_size = sizeof(printing_request);
  step.classify = acme_classify;
  CHECK_EQ(pd_register_probe_step(driver, &step), 0);
  step.id = "acme.silent-probe";
  step.request_bytes = silent_request;
  step.request_size = sizeof(silent_request);
  CHECK_EQ(pd_register_probe_step(driver, &step), 1);

  /* (4) formatter: a NULL callback is refused. */
  memset(&formatter, 0, sizeof(formatter));
  formatter.name = "acme.upper";
  formatter.formatter = NULL;
  CHECK_EQ(pd_register_formatter(driver, &formatter), 0);

  /* (5) drawer kick: a NULL kick_bytes is refused. */
  memset(&drawer, 0, sizeof(drawer));
  drawer.id = "acme.kick";
  drawer.kick_bytes = NULL;
  CHECK_EQ(pd_register_drawer_kick(driver, &drawer), 0);

  pd_destroy(driver);
}

/* --- M19: the receipt DSL through the ABI (docs/receipt-dsl.md) -------------------- */

/* The documents below are written once and used by several cases, because the point of
 * each case is what the SAME document does on a different profile or through a different
 * entry point. */

/* A plain (non-template) document: one line of text and one Code 128 symbol. */
static const char kBarcodeDocument[] =
    "{\"v\":1,\"blocks\":["
    "{\"text\":\"WIDGET CO\"},"
    "{\"barcode\":\"12345670\",\"symbology\":\"code128\"}]}";

/* A template: an `each` loop over a model array, and the built-in `upper` formatter on
 * two of the placeholders. Nothing here is renderable without a model. */
static const char kOrderTemplate[] =
    "{\"v\":1,\"template\":true,"
    "\"meta\":{\"cut\":\"full\",\"margins\":{\"topDots\":24}},"
    "\"blocks\":["
    "{\"text\":\"{{venue.name|upper}}\"},"
    "{\"each\":\"order.items\",\"block\":{\"text\":\"{{qty}}x {{name|upper}}\"}}]}";

static const char kOrderModel[] =
    "{\"venue\":{\"name\":\"my restaurant\"},"
    "\"order\":{\"items\":["
    "{\"qty\":2,\"name\":\"pilsner\"},"
    "{\"qty\":1,\"name\":\"goulash\"}]}}";

/* memmem is not C11, so the byte search a render assertion needs lives here. */
static int bytes_contain(const uint8_t* haystack, size_t size, const char* needle) {
  const size_t length = strlen(needle);
  size_t i;
  if (haystack == NULL || length == 0 || length > size) {
    return 0;
  }
  for (i = 0; i + length <= size; ++i) {
    if (memcmp(haystack + i, needle, length) == 0) {
      return 1;
    }
  }
  return 0;
}

/* 1 when the last render reported an entry of this kind, and copies it into `out`. */
static int find_report_entry(pd_driver* driver, pd_report_kind kind,
                            pd_report_entry* out) {
  const size_t count = pd_render_report_count(driver);
  size_t i;
  for (i = 0; i < count; ++i) {
    pd_report_entry entry;
    memset(&entry, 0, sizeof(entry));
    if (pd_render_report_at(driver, (int32_t)i, &entry) == 1 && entry.kind == kind) {
      *out = entry;
      return 1;
    }
  }
  return 0;
}

/* --- m1. A declared degradation crosses the ABI intact ----------------------------
 *
 * docs/receipt-dsl.md's degradation contract: a barcode on a profile with no GS k path
 * is OMITTED AND DECLARED, never emitted as a command the firmware would print as
 * literal text. The rest of the document still renders, which is what makes the report
 * the only way to know. And nothing prints: pd_render_document has no job. */
static void test_render_document_declares_a_barcode_degradation(void) {
  pd_driver* driver = pd_create(NULL);
  pd_printer* printer;
  pd_render_result result;
  pd_report_entry entry;
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  printer = pd_add_printer_scripted(driver, "capi-render-nobarcode", "no-barcode");
  CHECK(printer != NULL);

  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, kBarcodeDocument, NULL, NULL, &result), 1);
  CHECK_STREQ(pd_last_error(driver), "");
  /* The text block still produced bytes: a degradation is not a failure. */
  CHECK(result.size > 0);
  CHECK(bytes_contain(result.bytes, result.size, "WIDGET CO"));
  /* ...and no GS k went out. The digits are not searched for: a Code 128 symbol encodes
   * them, so their absence would prove nothing about the command. */
  CHECK(!bytes_contain(result.bytes, result.size, "\x1D\x6B"));

  CHECK_EQ(result.report_count, 1);
  CHECK_EQ(pd_render_report_count(driver), result.report_count);
  memset(&entry, 0, sizeof(entry));
  CHECK_EQ(find_report_entry(driver, PD_REPORT_UNSUPPORTED_BLOCK, &entry), 1);
  /* Where, what was asked for, what arrived, and by which path — the six facts a
   * support engineer needs, none of them an opaque string. */
  CHECK_STREQ(entry.block, "blocks[1]");
  CHECK(strstr(entry.requested, "code128") != NULL);
  CHECK_STREQ(entry.delivered, "omitted");
  CHECK_EQ(entry.path, PD_RENDER_PATH_NOT_RENDERED);
  CHECK(strlen(entry.note) > 0);
  CHECK_STREQ(pd_report_kind_name(entry.kind), "unsupportedBlock");
  CHECK_STREQ(pd_render_path_name(entry.path), "notRendered");

  /* Out of range is 0, not a crash. */
  CHECK_EQ(pd_render_report_at(driver, (int32_t)result.report_count, &entry), 0);
  CHECK_EQ(pd_render_report_at(driver, -1, &entry), 0);

  /* Rendering is not printing. */
  CHECK_EQ(pd_test_print_data_bytes(printer), 0);
  CHECK_EQ(pd_test_cuts(printer), 0);

  /* The same document on a printer whose profile HAS the barcode path renders it and
   * reports nothing — so the entry above is the profile speaking, not the parser. */
  {
    pd_printer* healthy = pd_add_printer_scripted(driver, "capi-render-ok", "ok");
    CHECK(healthy != NULL);
    memset(&result, 0, sizeof(result));
    CHECK_EQ(pd_render_document(driver, healthy, kBarcodeDocument, NULL, NULL, &result), 1);
    CHECK_EQ(result.report_count, 0);
    CHECK_EQ(pd_render_report_count(driver), 0);
    CHECK(result.size > 0);
    CHECK(bytes_contain(result.bytes, result.size, "\x1D\x6B"));
  }

  pd_destroy(driver);
}

/* --- m2. A template goes through the ordinary engine ------------------------------
 *
 * The `each` loop repeats, the `upper` formatter fires, the document's `meta` reaches the
 * job, and the bytes the loop produced are the bytes the device received. The job is an
 * ordinary pd_job: same states, same tri-state result, same grading as pd_print. */
static void test_print_document_json_prints_a_bound_template(void) {
  pd_driver* driver = pd_create(NULL);
  pd_printer* printer;
  pd_job_options options;
  pd_render_result rendered;
  pd_job* job;
  pd_job_result result;
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }

  printer = pd_add_printer_scripted(driver, "capi-doc-print", "ok");
  CHECK(printer != NULL);

  memset(&options, 0, sizeof(options));
  options.key = "capi-doc-1";

  memset(&rendered, 0, sizeof(rendered));
  job = pd_print_document_json(driver, printer, kOrderTemplate, kOrderModel, &options,
                               &rendered);
  CHECK(job != NULL);
  if (job == NULL) {
    pd_destroy(driver);
    return;
  }
  /* Everything bound and everything rendered. */
  CHECK_EQ(rendered.report_count, 0);
  /* The document's meta came back for the caller to see, and was applied to the job:
   * an all-zeroes pd_job_options means "the document decides". */
  CHECK_EQ(rendered.has_cut, 1);
  CHECK_EQ(rendered.cut, PD_CUT_FULL);
  CHECK_EQ(rendered.top_feed_dots, 24);
  CHECK_EQ(rendered.bottom_feed_dots, 0);

  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_job_await(driver, job, 5000, &result), 1);
  CHECK_EQ(result.outcome, PD_OUTCOME_DONE);
  CHECK_EQ(result.confidence, PD_CONFIDENCE_CUT_FAULT_FREE);
  CHECK_EQ(result.grade, PD_GRADE_A_JOB_LEVEL_CONFIRMATION);
  CHECK_STREQ(result.method, "GS(H) fn48");
  CHECK_STREQ(pd_job_key(job), "capi-doc-1");

  /* The wire carries what the template said it would: the formatter ran on the venue
   * name, and the `each` produced one line per model element, in order. */
  CHECK(pd_test_received_contains(printer, "MY RESTAURANT"));
  CHECK(pd_test_received_contains(printer, "2x PILSNER"));
  CHECK(pd_test_received_contains(printer, "1x GOULASH"));
  /* Unbound placeholders never reach paper. */
  CHECK(!pd_test_received_contains(printer, "{{"));
  CHECK_EQ(pd_test_cuts(printer), 1);

  /* Rule 2 of the idempotency contract holds through this entry point too: the same key
   * does not print twice, it returns the same handle. */
  {
    pd_job* again = pd_print_document_json(driver, printer, kOrderTemplate, kOrderModel,
                                           &options, NULL);
    CHECK(again == job);
    CHECK_EQ(pd_test_cuts(printer), 1);
  }

  pd_destroy(driver);
}

/* --- m3. A registered formatter fires through this path ---------------------------
 *
 * The call site pd_register_formatter was always waiting for (docs/api.md §16 and §17.1):
 * before M19 the core stored the registration and no C-reachable code consulted it. The
 * control is the same template on a driver that never registered it — the placeholder
 * comes back unformatted with an UnknownFormatter entry. */
static size_t acme_stars(void* ctx, const char* value, const char* args,
                        const char* locale, char* out, size_t cap, int32_t* handled) {
  size_t length;
  (void)args;
  (void)locale;
  *(int*)ctx += 1; /* the counter proves the core called us, not that we could be called */
  length = strlen(value) + 6;
  if (length >= cap) {
    return cap + 1; /* over cap is the registration's error, never a silent truncation */
  }
  strcpy(out, "***");
  strcat(out, value);
  strcat(out, "***");
  *handled = 1;
  return length;
}

static void test_registered_formatter_fires_through_the_render_path(void) {
  static const char kStarTemplate[] =
      "{\"v\":1,\"template\":true,\"blocks\":[{\"text\":\"{{item|acme.stars}}\"}]}";
  static const char kStarModel[] = "{\"item\":\"tip\"}";

  pd_driver* driver;
  pd_printer* printer;
  pd_formatter formatter;
  pd_render_result result;
  pd_report_entry entry;
  int calls = 0;

  /* The control first: no registration, so the name is unknown. */
  driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  printer = pd_add_printer_scripted(driver, "capi-fmt-control", "ok");
  CHECK(printer != NULL);
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, kStarTemplate, kStarModel, NULL, &result), 1);
  CHECK_EQ(find_report_entry(driver, PD_REPORT_UNKNOWN_FORMATTER, &entry), 1);
  CHECK(!bytes_contain(result.bytes, result.size, "***tip***"));
  pd_destroy(driver);

  /* Now with the registration in place, on the same template. */
  driver = pd_create(NULL);
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  memset(&formatter, 0, sizeof(formatter));
  formatter.name = "acme.stars";
  formatter.formatter = acme_stars;
  formatter.ctx = &calls;
  CHECK_EQ(pd_register_formatter(driver, &formatter), 1);

  printer = pd_add_printer_scripted(driver, "capi-fmt", "ok");
  CHECK(printer != NULL);

  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, kStarTemplate, kStarModel, NULL, &result), 1);
  CHECK_EQ(result.report_count, 0);
  CHECK(calls > 0);
  CHECK(bytes_contain(result.bytes, result.size, "***tip***"));

  /* And through the printing path, so the registration is not a preview-only effect. */
  {
    pd_job* job = pd_print_document_json(driver, printer, kStarTemplate, kStarModel, NULL,
                                         NULL);
    pd_job_result outcome;
    CHECK(job != NULL);
    memset(&outcome, 0, sizeof(outcome));
    CHECK_EQ(pd_job_await(driver, job, 5000, &outcome), 1);
    CHECK_EQ(outcome.outcome, PD_OUTCOME_DONE);
    CHECK(pd_test_received_contains(printer, "***tip***"));
  }

  pd_destroy(driver);
}

/* --- m4. Failures are explained, and never printed --------------------------------
 *
 * Three things stop bytes being produced, and each of them has to arrive as a clear
 * error PLUS a report entry — never a crash, and never a blank receipt, which on a
 * counter looks like a printer fault and sends somebody to check the paper. */
static void test_malformed_documents_are_refused_with_a_report(void) {
  pd_driver* driver = pd_create(NULL);
  pd_printer* printer;
  pd_render_result result;
  pd_report_entry entry;
  CHECK(driver != NULL);
  if (driver == NULL) {
    return;
  }
  printer = pd_add_printer_scripted(driver, "capi-doc-bad", "ok");
  CHECK(printer != NULL);

  /* (a) not JSON at all. */
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, "this is not json", NULL, NULL, &result), 0);
  CHECK(strlen(pd_last_error(driver)) > 0);
  CHECK_EQ(result.size, 0);
  CHECK(result.bytes == NULL);
  CHECK(result.report_count >= 1);
  CHECK_EQ(find_report_entry(driver, PD_REPORT_MALFORMED_TEMPLATE, &entry), 1);
  CHECK_EQ(entry.path, PD_RENDER_PATH_NOT_RENDERED);

  /* (b) JSON, but not a document: a block with no recognised key. */
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, "{\"v\":1,\"blocks\":[{\"nope\":1}]}", NULL,
                              NULL, &result),
           0);
  CHECK(result.report_count >= 1);

  /* (c) a template with no model. Printing the placeholders would be worse than printing
   * nothing, because it looks like a receipt. */
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, kOrderTemplate, NULL, NULL, &result), 0);
  CHECK(strstr(pd_last_error(driver), "model") != NULL);
  CHECK_EQ(find_report_entry(driver, PD_REPORT_MALFORMED_TEMPLATE, &entry), 1);

  /* (d) a model that is not JSON. */
  memset(&result, 0, sizeof(result));
  CHECK_EQ(pd_render_document(driver, printer, kOrderTemplate, "{oops", NULL, &result), 0);
  CHECK(result.report_count >= 1);

  /* None of the four submitted anything, and neither does the printing entry point. */
  memset(&result, 0, sizeof(result));
  CHECK(pd_print_document_json(driver, printer, "this is not json", NULL, NULL, &result) ==
        NULL);
  CHECK(strlen(pd_last_error(driver)) > 0);
  CHECK(result.report_count >= 1);
  CHECK_EQ(pd_test_print_data_bytes(printer), 0);
  CHECK_EQ(pd_test_cuts(printer), 0);

  /* A null document is refused rather than dereferenced, and so is a null out. */
  CHECK_EQ(pd_render_document(driver, printer, NULL, NULL, NULL, &result), 0);
  CHECK_EQ(pd_render_document(driver, printer, kBarcodeDocument, NULL, NULL, NULL), 0);
  CHECK(pd_print_document_json(driver, printer, NULL, NULL, NULL, NULL) == NULL);

  pd_destroy(driver);
}

int main(void) {
  test_submit_reaches_terminal_done();
  test_verification_identifier_round_trip();
  test_reprint_banner_toggle_and_margins();
  test_same_key_dedupe_returns_same_job();
  test_event_callback_receives_ordered_progression();
  test_enum_bridge_matches_pd_h();
  test_custom_transport_drives_a_whole_job();
  test_custom_transport_rejects_a_broken_registration();
  test_zebra_and_brother_are_refused_without_writing_a_byte();
  test_grade_hierarchy_gained_a_plus();
  test_provenance_is_readable_per_printer();
  test_profile_ids_expose_the_whole_catalogue();
  test_queue_addon_drains_through_the_same_engine();
  test_drawer_sequence_over_the_abi();
  test_drawer_refusals_write_no_bytes();
  test_drawer_polarity_calibration_over_the_abi();
  test_self_test_prints_one_ticket_and_reports_the_detection();
  test_auto_detect_classifies_three_listeners_without_printing();
  test_discover_sweeps_a_loopback_address_and_writes_only_dle_eot();
  test_custom_completion_method_earns_the_registered_grade();
  test_unregistered_vendor_idle_is_unsupported();
  test_other_registration_points_are_wired();
  test_render_document_declares_a_barcode_degradation();
  test_print_document_json_prints_a_bound_template();
  test_registered_formatter_fires_through_the_render_path();
  test_malformed_documents_are_refused_with_a_report();

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  printf("test_capi: all checks passed\n");
  return EXIT_SUCCESS;
}
