#ifndef PRINTERDRIVER_PD_H
#define PRINTERDRIVER_PD_H

/*
 * The C ABI every other wrapper binds (docs/api.md §9).
 *
 * This is the whole surface: three opaque handle types, one submit call, a tri-state
 * terminal result. Swift, Kotlin and Dart wrappers are generated-thin over it — enum
 * bridging plus async adapters, no logic — so anything that needs a decision belongs
 * in the core behind this header, never in a wrapper.
 *
 * -- Closed enums ------------------------------------------------------------------
 * Every enum below mirrors a C++ enum in core/include/printerdriver one-for-one, with
 * explicit values and a trailing _COUNT giving the number of members. The mirroring is
 * enforced by static_assert in the library's own translation unit, so a value added to
 * the core without regenerating this header fails the build rather than silently
 * turning into "not implemented on platform X" (docs/api.md §1.3).
 *
 * -- Callback threads --------------------------------------------------------------
 * Callbacks are invoked on the core's own threads, not on the caller's:
 *   - job event callbacks run on the owning printer's worker thread, except for the
 *     replay of already-recorded events, which runs on the thread calling
 *     pd_subscribe_job before that call returns;
 *   - device event callbacks run on whichever thread decoded the status — the
 *     transport's reader thread, or the worker thread when a status query answers
 *     inside a job.
 * A callback must not block, and must not call back into any pd_* function on the same
 * driver: the worker thread it is running on is the thread that would have to service
 * that call. Copy what you need and signal your own loop.
 *
 * -- String ownership --------------------------------------------------------------
 * Every `const char*` passed in is copied before the call returns; the caller may free
 * or reuse its buffer immediately afterwards. Every `const char*` returned is owned by
 * the handle it came from:
 *   - pd_printer_id / pd_job_id / pd_job_key are stable for the life of the handle,
 *     i.e. until pd_destroy;
 *   - pd_last_error is valid until the next pd_* call on that same driver;
 *   - the pd_*_name helpers return static storage that is valid forever.
 * Nothing returned by this ABI is ever freed by the caller.
 *
 * -- Handle lifetime ---------------------------------------------------------------
 * pd_printer and pd_job handles are owned by their pd_driver and are freed by
 * pd_destroy. They are never freed individually, and the same underlying job always
 * maps to the same pd_job pointer, so idempotency-key dedupe is visible as pointer
 * equality.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Opaque handles ------------------------------------------------------------- */

typedef struct pd_driver pd_driver;
typedef struct pd_printer pd_printer;
typedef struct pd_job pd_job;

/* --- Enums (mirrors of core/include/printerdriver) ------------------------------- */

/* pd::JobState — docs/sdk-spec.md §5, mirroring docs/techspec.md §5.1. */
typedef enum pd_job_state {
  PD_JOB_STATE_QUEUED = 0,
  PD_JOB_STATE_PREFLIGHT_OK = 1,
  PD_JOB_STATE_SEND_STARTED = 2,
  PD_JOB_STATE_BYTES_SENT = 3,
  PD_JOB_STATE_PRINT_CONFIRMED = 4,
  PD_JOB_STATE_CUT_COMMAND_PROCESSED = 5,
  PD_JOB_STATE_DONE_SOFTWARE = 6,
  PD_JOB_STATE_PHYSICALLY_VERIFIED = 7,
  PD_JOB_STATE_FAILED_KNOWN = 8,
  PD_JOB_STATE_UNKNOWN = 9,
  /* Produced only by the print-queue addon (docs/sdk-spec.md §12). */
  PD_JOB_STATE_HELD_OFFLINE = 10,
  PD_JOB_STATE_COUNT = 11
} pd_job_state;

/* pd::ConfidenceLevel — what evidence backs the current claim. Never inflated. */
typedef enum pd_confidence_level {
  PD_CONFIDENCE_TRANSPORT_ACCEPTED = 0,
  PD_CONFIDENCE_PRINTER_HEALTHY = 1,
  PD_CONFIDENCE_PRINT_CONFIRMED = 2,
  PD_CONFIDENCE_CUT_PROCESSED = 3,
  PD_CONFIDENCE_CUT_FAULT_FREE = 4,
  PD_CONFIDENCE_PHYSICALLY_VERIFIED = 5,
  PD_CONFIDENCE_COUNT = 6
} pd_confidence_level;

/* pd::DeviceEvent — the per-printer stream that replaces availability polling. */
typedef enum pd_device_event {
  PD_DEVICE_ONLINE = 0,
  PD_DEVICE_OFFLINE = 1,
  PD_DEVICE_COVER_OPEN = 2,
  PD_DEVICE_COVER_CLOSED = 3,
  PD_DEVICE_PAPER_OUT = 4,
  PD_DEVICE_PAPER_NEAR_END = 5,
  PD_DEVICE_PAPER_OK = 6,
  PD_DEVICE_CUTTER_ERROR = 7,
  PD_DEVICE_RECOVERABLE_ERROR = 8,
  PD_DEVICE_UNRECOVERABLE_ERROR = 9,
  PD_DEVICE_CONNECTION_LOST = 10,
  PD_DEVICE_CONNECTION_RESTORED = 11,
  PD_DEVICE_EVENT_COUNT = 12
} pd_device_event;

/* pd::FailureReason. */
typedef enum pd_failure_reason {
  PD_REASON_NONE = 0,
  PD_REASON_TRANSPORT_UNREACHABLE = 1,
  PD_REASON_PREFLIGHT_COVER_OPEN = 2,
  PD_REASON_PREFLIGHT_PAPER_OUT = 3,
  PD_REASON_PREFLIGHT_HARDWARE_ERROR = 4,
  PD_REASON_TIMEOUT_AWAITING_COMPLETION = 5,
  PD_REASON_CUTTER_FAULT = 6,
  PD_REASON_UNSUPPORTED = 7,
  PD_REASON_UNKNOWN = 8,
  /* Print-queue addon only (docs/sdk-spec.md §12). */
  PD_REASON_EXPIRED = 9,
  PD_REASON_QUEUE_OVERFLOW = 10,
  PD_REASON_COUNT = 11
} pd_failure_reason;

/*
 * pd::JobOutcome — deliberately tri-state (docs/api.md §1.4). There is no success
 * boolean anywhere in this ABI: collapsing PD_OUTCOME_UNKNOWN into either bucket is
 * the bug that produces duplicate kitchen tickets. Bind it as a sealed type.
 */
typedef enum pd_job_outcome {
  PD_OUTCOME_DONE = 0,
  PD_OUTCOME_FAILED = 1,
  PD_OUTCOME_UNKNOWN = 2,
  PD_OUTCOME_COUNT = 3
} pd_job_outcome;

/* pd::CutSetting — PD_CUT_PROFILE means "whatever this printer's cutter does". */
typedef enum pd_cut {
  PD_CUT_PROFILE = 0,
  PD_CUT_PARTIAL = 1,
  PD_CUT_FULL = 2,
  PD_CUT_NONE = 3,
  PD_CUT_COUNT = 4
} pd_cut;

/* pd::PreflightMode. */
typedef enum pd_preflight {
  PD_PREFLIGHT_STRICT = 0,
  PD_PREFLIGHT_SKIP = 1,
  PD_PREFLIGHT_COUNT = 2
} pd_preflight;

/* pd::PayloadKind — the tier a job was submitted as (docs/api.md §3). */
typedef enum pd_payload_kind {
  PD_PAYLOAD_RASTER_RGBA8 = 0,
  PD_PAYLOAD_DOCUMENT = 1,
  PD_PAYLOAD_RAW = 2,
  PD_PAYLOAD_KIND_COUNT = 3
} pd_payload_kind;

/* pd::CompletionMechanism — which ordered fence a profile answers. */
typedef enum pd_completion_mechanism {
  PD_COMPLETION_GS_PAREN_H = 0,
  PD_COMPLETION_GS_R1 = 1,
  PD_COMPLETION_NONE = 2,
  PD_COMPLETION_COUNT = 3
} pd_completion_mechanism;

/* pd::CutVariant — the cut a profile's mechanism actually performs. */
typedef enum pd_cut_variant {
  PD_CUT_VARIANT_PARTIAL = 0,
  PD_CUT_VARIANT_FULL = 1,
  PD_CUT_VARIANT_NONE = 2,
  PD_CUT_VARIANT_COUNT = 3
} pd_cut_variant;

/* pd::escpos::Alignment — values are the `n` operand of ESC a n. */
typedef enum pd_alignment {
  PD_ALIGN_LEFT = 0,
  PD_ALIGN_CENTER = 1,
  PD_ALIGN_RIGHT = 2,
  PD_ALIGN_COUNT = 3
} pd_alignment;

/*
 * pd::escpos::CodePage. Values are the `n` operand of ESC t n, so they are not
 * contiguous: PD_CODE_PAGE_COUNT is the number of members this SDK supports, not a
 * past-the-end sentinel. Iterate with pd_code_page_at().
 */
typedef enum pd_code_page {
  PD_CODE_PAGE_PC437 = 0,
  PD_CODE_PAGE_PC850 = 2,
  PD_CODE_PAGE_WPC1252 = 16,
  PD_CODE_PAGE_PC852 = 18,
  PD_CODE_PAGE_PC858 = 19,
  PD_CODE_PAGE_COUNT = 5
} pd_code_page;

/* pd::escpos::Binarization — how the raster tier turns grey into dots. */
typedef enum pd_binarization {
  PD_BINARIZATION_FIXED_THRESHOLD = 0,
  PD_BINARIZATION_FLOYD_STEINBERG = 1,
  PD_BINARIZATION_COUNT = 2
} pd_binarization;

/* Tri-state for the optional bits in pd_device_status. */
#define PD_UNKNOWN (-1)
#define PD_FALSE 0
#define PD_TRUE 1

/* --- Structs -------------------------------------------------------------------- */

/*
 * One step of a job's history. `has_reason` is 0 for every non-failure transition;
 * `reason` is then PD_REASON_NONE. `monotonic_ms` comes from a steady clock, because
 * the wall clock may step and job timing must not.
 */
typedef struct pd_job_event {
  pd_job_state state;
  pd_confidence_level confidence;
  int32_t has_reason;
  pd_failure_reason reason;
  uint64_t monotonic_ms;
} pd_job_event;

/*
 * The terminal answer. `confidence` is carried on all three outcomes, not only on
 * Done: on Failed and Unknown it records how far up the evidence ladder the job got
 * before it stopped, which is what an operator needs to decide about a reprint.
 */
typedef struct pd_job_result {
  pd_job_outcome outcome;
  pd_confidence_level confidence;
  pd_failure_reason reason;
} pd_job_result;

/*
 * Last known device state — never a live query, so it cannot block behind a print.
 * `observed` is 0 until a status frame has actually been decoded: a snapshot that has
 * never heard from the device says so rather than reporting healthy. Every tri-state
 * field is PD_UNKNOWN, PD_FALSE or PD_TRUE.
 */
typedef struct pd_device_status {
  int32_t connected;
  int32_t observed;
  int32_t online;
  int32_t cover_open;
  int32_t paper_out;
  int32_t paper_near_end;
  int32_t cutter_error;
  int32_t unrecoverable_error;
  int32_t recoverable_error;
} pd_device_status;

/* --- Payload tiers (docs/api.md §3) ---------------------------------------------- */

/*
 * Tier 1. Straight RGBA8 as every platform's bitmap hands it over. The core composites
 * alpha over white paper, converts to grey with the ITU-R 601 luma weights, scales to
 * the printer's dot width, binarizes and bands — all in integer arithmetic, so the
 * same pixels always produce the same bytes. `stride_bytes` is 0 for tightly packed
 * rows.
 */
typedef struct pd_raster_rgba8 {
  const uint8_t* pixels;
  uint32_t width;
  uint32_t height;
  uint32_t stride_bytes;
  pd_binarization binarization;
  uint8_t threshold;         /* only read for PD_BINARIZATION_FIXED_THRESHOLD; 0 -> 128 */
  uint32_t max_rows_per_band; /* 0 -> 1024, the Epson tall-image split */
} pd_raster_rgba8;

/* Tier 2 operations. Deliberately minimal: everything a receipt needs and nothing a
 * layout engine would need. Richer documents belong in the raster tier. */
typedef enum pd_op_kind {
  PD_OP_TEXT = 0,  /* `text`, no line break */
  PD_OP_LINE = 1,  /* `text` followed by LF; NULL text emits a bare LF */
  PD_OP_ALIGN = 2, /* `value` is a pd_alignment */
  PD_OP_BOLD = 3,  /* `value` is 0 or 1 */
  PD_OP_FEED = 4,  /* `value` is a line count, 1..255 */
  PD_OP_KIND_COUNT = 5
} pd_op_kind;

typedef struct pd_op {
  pd_op_kind kind;
  const char* text; /* UTF-8; transliterated to the document's code page, lossily */
  int32_t value;
} pd_op;

typedef struct pd_document {
  const pd_op* ops;
  size_t count;
  pd_code_page code_page;
} pd_document;

/*
 * Tier 3. Passed through verbatim. Must not embed its own cuts or realtime status
 * tricks: the core owns job termination, and its trailing fence assumes that.
 */
typedef struct pd_raw {
  const uint8_t* bytes;
  size_t size;
} pd_raw;

typedef struct pd_payload {
  pd_payload_kind kind;
  union {
    pd_raster_rgba8 raster;
    pd_document document;
    pd_raw raw;
  } as;
} pd_payload;

/* --- Configuration --------------------------------------------------------------- */

/*
 * All-zeroes is a valid, safe configuration everywhere in this ABI: the zero value of
 * every field is its documented default, and no default trades durability for speed.
 */
typedef void (*pd_log_cb)(const char* message, void* ctx);

typedef struct pd_config {
  const char* storage_directory; /* NULL or "" -> in-memory: no journal, no recovery */
  int32_t fsync_disabled;        /* 0 keeps the durability rule; 1 is for tests only */
  pd_log_cb log;                 /* called with ABI-level diagnostics; may be NULL */
  void* log_ctx;
} pd_config;

typedef struct pd_tcp_config {
  const char* printer_id;  /* NULL or "" -> derived from the endpoint */
  const char* host;
  uint16_t port;           /* 0 -> 9100 */
  uint32_t width_dots;     /* 0 -> 576; 384, 504 and 576 are the deployed widths */
  const char* profile_id;  /* NULL or "" -> "generic"; see pd_profile_ids() */
  uint32_t connect_timeout_ms; /* 0 -> 3000 */
} pd_tcp_config;

typedef struct pd_job_options {
  const char* key;      /* NULL or "" -> generated; no dedupe protection then */
  pd_cut cut;
  int32_t open_drawer;
  pd_preflight preflight;
  uint32_t timeout_ms;  /* 0 -> the profile's completion timeout */
} pd_job_options;

/* --- Callbacks ------------------------------------------------------------------- */

typedef void (*pd_job_event_cb)(pd_job* job, const pd_job_event* event, void* ctx);
typedef void (*pd_device_event_cb)(pd_printer* printer, pd_device_event event, void* ctx);

/* --- Driver ---------------------------------------------------------------------- */

/* NULL on failure; the reason is not retrievable, since there is no handle to hang it
 * on. Only a storage directory that cannot be created can cause it. */
pd_driver* pd_create(const pd_config* config);

/* Stops every printer worker, waits for in-flight jobs to reach a terminal state, and
 * frees every pd_printer and pd_job handle this driver ever returned. */
void pd_destroy(pd_driver* driver);

/* Why the last call on this driver returned NULL or 0. Never NULL; "" when the last
 * call succeeded. Owned by the driver until the next call on it. */
const char* pd_last_error(pd_driver* driver);

/* The profile ids pd_tcp_config accepts, as a NULL-terminated array of static
 * strings. A wrapper enumerates this rather than hardcoding names. */
const char* const* pd_profile_ids(void);

/* --- Printers -------------------------------------------------------------------- */

pd_printer* pd_add_printer_tcp(pd_driver* driver, const pd_tcp_config* config);

const char* pd_printer_id(pd_printer* printer);
uint32_t pd_printer_width_dots(pd_printer* printer);
pd_completion_mechanism pd_printer_completion(pd_printer* printer);

pd_device_status pd_printer_status(pd_driver* driver, pd_printer* printer);

/* Queues a status round trip behind any active job and waits for the answer. Blocks. */
pd_device_status pd_printer_refresh_status(pd_driver* driver, pd_printer* printer,
                                           uint32_t timeout_ms);

void pd_open_cash_drawer(pd_driver* driver, pd_printer* printer);

/* Blocks until this printer's queue is empty and its active job is terminal. */
void pd_printer_drain(pd_driver* driver, pd_printer* printer);

void pd_subscribe_device(pd_driver* driver, pd_printer* printer, pd_device_event_cb cb,
                         void* ctx);

/* --- Jobs ------------------------------------------------------------------------ */

/*
 * The one submit call. Re-submitting a key that already has a job does not print: it
 * returns that job's handle, whatever state it is in (docs/api.md §3). NULL means the
 * payload was malformed or the handles did not match; see pd_last_error.
 */
pd_job* pd_print(pd_driver* driver, pd_printer* printer, const pd_payload* payload,
                 const pd_job_options* options);

/*
 * Deliberate duplicate of an already-submitted key: reuses the original payload and
 * prepends the reprint banner and attempt counter. NULL when the key is unknown, or
 * when its job was reconstructed from the journal — those records carry what happened
 * to a job, never what it contained.
 */
pd_job* pd_force_reprint(pd_driver* driver, pd_printer* printer, const char* key,
                         const pd_job_options* options);

/* Any job this driver knows about, including ones reloaded from the journal after a
 * restart. NULL when the key is unknown. */
pd_job* pd_find_job(pd_driver* driver, const char* key);

const char* pd_job_id(pd_job* job);
const char* pd_job_key(pd_job* job);
uint32_t pd_job_attempt(pd_job* job);
pd_job_state pd_job_current_state(pd_job* job);
pd_confidence_level pd_job_confidence(pd_job* job);
int32_t pd_job_is_terminal(pd_job* job);

/*
 * Replays every event recorded so far on the calling thread, then streams the rest on
 * the printer's worker thread. The last event a job ever emits is always a terminal
 * one.
 */
void pd_subscribe_job(pd_driver* driver, pd_job* job, pd_job_event_cb cb, void* ctx);

/*
 * Waits for the terminal result. timeout_ms == 0 waits indefinitely. Returns 1 and
 * fills `out` when the job is terminal, 0 on timeout (`out` untouched).
 */
int32_t pd_job_await(pd_driver* driver, pd_job* job, uint32_t timeout_ms,
                     pd_job_result* out);

/* --- Enum names ------------------------------------------------------------------ */

/* Static storage, never NULL, matching the core's own spelling one-for-one. */
const char* pd_job_state_name(pd_job_state value);
const char* pd_confidence_level_name(pd_confidence_level value);
const char* pd_device_event_name(pd_device_event value);
const char* pd_failure_reason_name(pd_failure_reason value);
const char* pd_job_outcome_name(pd_job_outcome value);
const char* pd_payload_kind_name(pd_payload_kind value);
const char* pd_completion_mechanism_name(pd_completion_mechanism value);
const char* pd_cut_variant_name(pd_cut_variant value);

/* The non-contiguous code page enum, by member index 0..PD_CODE_PAGE_COUNT-1. */
pd_code_page pd_code_page_at(int32_t index);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* PRINTERDRIVER_PD_H */
