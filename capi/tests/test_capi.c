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

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  printf("test_capi: all checks passed\n");
  return EXIT_SUCCESS;
}
