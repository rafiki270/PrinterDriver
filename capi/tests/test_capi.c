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

  /* The ten enums with a spelling on both sides: compare name for name. */
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

  /* The remaining five have no C++ to_string, so the bridge must say so plainly rather
   * than inventing a spelling. */
  const pd_test_enum unnamed[] = {PD_TEST_ENUM_CUT, PD_TEST_ENUM_PREFLIGHT,
                                  PD_TEST_ENUM_ALIGNMENT, PD_TEST_ENUM_CODE_PAGE,
                                  PD_TEST_ENUM_BINARIZATION};
  for (size_t i = 0; i < sizeof(unnamed) / sizeof(unnamed[0]); ++i) {
    CHECK(pd_test_cpp_enum_name(unnamed[i], 0) == NULL);
  }
}

int main(void) {
  test_submit_reaches_terminal_done();
  test_verification_identifier_round_trip();
  test_reprint_banner_toggle_and_margins();
  test_same_key_dedupe_returns_same_job();
  test_event_callback_receives_ordered_progression();
  test_enum_bridge_matches_pd_h();

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  printf("test_capi: all checks passed\n");
  return EXIT_SUCCESS;
}
