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
  /* A GS ( H echo arrived carrying a token this driver never issued: something else is
   * writing to the same printer (docs/api.md §14). The echo is attributed to no job. */
  PD_DEVICE_FOREIGN_WRITER_DETECTED = 12,
  PD_DEVICE_EVENT_COUNT = 13
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

/*
 * pd::ConfidenceGrade — what *class* of evidence a claim rests on
 * (docs/device-database.md "Confidence grades for every route"). Orthogonal to
 * pd_confidence_level: the level says how far up the evidence ladder a job climbed, the
 * grade says what the claim is made of. Done at PD_CONFIDENCE_CUT_PROCESSED on grade A
 * and Done at PD_CONFIDENCE_CUT_PROCESSED on grade D are not the same claim.
 */
typedef enum pd_confidence_grade {
  /*
   * A durable, queryable printer-side job: ePOS submits, returns a JobID, and the
   * result is retrievable afterwards — the only mechanism here that survives the
   * application losing its connection between submission and answer.
   *
   * NOTHING PRODUCES THIS GRADE YET: the ePOS transport does not exist in this core, so
   * no pd_job_result can carry it. It is defined now because these enums are closed and
   * mirrored by four wrappers, and adding a member later would renumber every mirror a
   * second time. A profile that reports epos_job_id is describing hardware, not making
   * a claim, and is graded A.
   */
  PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB = 0,
  PD_GRADE_A_JOB_LEVEL_CONFIRMATION = 1,  /* GS ( H, Star checked block */
  PD_GRADE_B_ORDERED_DEVICE_RESPONSE = 2, /* GS r, vendor idle query */
  PD_GRADE_C_DEVICE_STATUS_AROUND = 3,    /* DLE EOT / ASB / SNMP around the send */
  PD_GRADE_D_SPOOLER_COMPLETED = 4,       /* a spooler or IPP gateway said completed */
  PD_GRADE_E_TRANSPORT_ONLY = 5,          /* the write succeeded, nothing else is known */
  PD_GRADE_COUNT = 6
} pd_confidence_grade;

/*
 * pd::Provenance — where the claim that a printer has a capability comes from
 * (docs/compatibility-brief.md §28). Recognising ESC/POS print commands does not prove
 * the Epson feedback extensions, so "the manufacturer's manual says so", "we asked the
 * hardware and it answered" and "nobody has checked" are three answers, not one
 * boolean. The three are independent rather than ordered: a probe can contradict
 * documentation when the interface path swallows responses, and documentation can cover
 * a model no probe has reached.
 */
typedef enum pd_provenance {
  PD_PROVENANCE_DOCUMENTED = 0, /* the manufacturer's command documentation lists it */
  PD_PROVENANCE_PROBED = 1,     /* this driver asked the installed hardware */
  PD_PROVENANCE_UNVERIFIED = 2, /* neither — a default nobody has confirmed */
  PD_PROVENANCE_COUNT = 3
} pd_provenance;

/*
 * pd::CommandLanguage — what the device actually speaks (docs/compatibility-brief.md
 * §1). Only PD_LANGUAGE_ESC_POS is implemented; the rest exist so a fleet containing
 * them can be described rather than misdriven. A profile naming ZPL, CPCL, Brother
 * raster or ESC/P is refused with PD_REASON_UNSUPPORTED before a byte is written.
 */
typedef enum pd_command_language {
  PD_LANGUAGE_ESC_POS = 0,
  PD_LANGUAGE_STAR_PRNT = 1,
  PD_LANGUAGE_STAR_LINE = 2,
  PD_LANGUAGE_EPOS_XML = 3,
  PD_LANGUAGE_ZPL = 4,
  PD_LANGUAGE_CPCL = 5,
  PD_LANGUAGE_BROTHER_RASTER = 6,
  PD_LANGUAGE_ESC_P = 7, /* Brother ESC/P — a different language from Epson ESC/POS */
  PD_LANGUAGE_COUNT = 8
} pd_command_language;

/*
 * pd::CompletionAuthority — who is actually making the claim. "Completed" from a print
 * server and "completed" from the mechanism that moved the paper are different facts,
 * which is the entire reason this is recorded separately from the grade.
 */
typedef enum pd_completion_authority {
  PD_AUTHORITY_PHYSICAL_PRINTER = 0,
  PD_AUTHORITY_VENDOR_SPOOLER = 1,
  PD_AUTHORITY_PD_AGENT = 2,
  PD_AUTHORITY_PRINT_SERVER = 3,
  PD_AUTHORITY_TRANSPORT_ONLY = 4,
  PD_AUTHORITY_COUNT = 5
} pd_completion_authority;

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
  PD_COMPLETION_VENDOR_IDLE = 2,
  PD_COMPLETION_EPOS_JOB_ID = 3,
  PD_COMPLETION_STAR_CHECKED_BLOCK = 4,
  PD_COMPLETION_NONE = 5,
  PD_COMPLETION_COUNT = 6
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
 *
 * `grade`, `authority` and `method` say what that claim is made of and who made it
 * (docs/device-database.md: never a bare {success:true}). `method` names the actual
 * command, e.g. "GS(H) fn48" — the string a support engineer needs six months later.
 * It is owned by the pd_job that produced it and valid until pd_destroy; it is never
 * NULL, and it is "none" when nothing was confirmed.
 */
typedef struct pd_job_result {
  pd_job_outcome outcome;
  pd_confidence_level confidence;
  pd_failure_reason reason;
  pd_confidence_grade grade;
  pd_completion_authority authority;
  const char* method;
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

/* --- Custom transports: Bluetooth and anything else the platform owns -------------- */

/*
 * docs/compatibility-brief.md §25. Bluetooth cannot live in this core and should not:
 * on Apple it is CoreBluetooth or ExternalAccessory (MFi), on Android a BluetoothSocket
 * over RFCOMM, on Linux a BlueZ AF_BLUETOOTH socket. Each is a platform framework with
 * its own permissions model, its own pairing UI and its own threading, and linking any
 * of them would make a portable C++17 core unportable in order to buy one transport.
 *
 * So the split is: THE PLATFORM OWNS THE SOCKET, THE CORE OWNS THE PROTOCOL. A wrapper
 * implements three operations and pushes received bytes in. Everything that makes this
 * SDK worth using — the ordered fence, the GS ( H correlation token, preflight, the
 * journal, the confidence grading, the refusal to overclaim — stays on this side of the
 * boundary and behaves identically over Bluetooth, TCP or a test double. A wrapper
 * cannot weaken a completion guarantee by accident, because a wrapper never makes one.
 *
 * -- Thread contract -----------------------------------------------------------------
 *
 *   - connect/write/close are invoked on the core's worker thread for this printer, one
 *     at a time, never concurrently with each other.
 *   - pd_transport_feed_bytes may be called from ANY thread — typically the wrapper's
 *     own RX thread, delegate queue or coroutine — INCLUDING while write is in flight.
 *     That is the normal case: a status answer arrives while the next chunk goes out.
 *   - pd_transport_feed_bytes must NOT be called from inside connect/write/close: those
 *     run on the thread that would have to service the delivery.
 *   - No callback may call back into any pd_* function on the same driver, for the
 *     reason given under "Callback threads" above.
 *   - The registration outlives individual connections. The core reconnects after a
 *     link drop by calling connect again; the wrapper keeps feeding the same printer
 *     handle and never has to learn about it.
 */

/* Open the link. Non-zero for success, 0 for failure. */
typedef int32_t (*pd_transport_connect_fn)(void* ctx);

/*
 * Hand `size` bytes to the link. Returns the number of bytes actually transferred, or a
 * negative value for a hard failure. A short write is reported honestly rather than
 * rounded up: zero bytes out is a known failure and one byte out is Unknown
 * (docs/api.md §4), and that difference decides whether an operator should reprint.
 */
typedef int64_t (*pd_transport_write_fn)(void* ctx, const uint8_t* data, size_t size);

/* Close the link. Called once per successful connect, and again is harmless. */
typedef void (*pd_transport_close_fn)(void* ctx);

typedef struct pd_transport_vtable {
  pd_transport_connect_fn connect;
  pd_transport_write_fn write;
  pd_transport_close_fn close;
  /*
   * What the printer id and the diagnostics derive from when no id is supplied, e.g.
   * "bt-spp:00:11:22:33:44:55". Copied before pd_add_printer_custom returns. NULL or ""
   * becomes "custom", which is fine for one printer and ambiguous for two.
   */
  const char* description;
} pd_transport_vtable;

typedef struct pd_job_options {
  const char* key;      /* NULL or "" -> generated; no dedupe protection then */
  pd_cut cut;
  int32_t open_drawer;
  pd_preflight preflight;
  uint32_t timeout_ms;  /* 0 -> the profile's completion timeout */

  /* Margins (docs/receipt-dsl.md). `top_feed_dots` is fed before the first content
   * byte. `bottom_feed_dots` is the *total* clearance between the last content and the
   * cut: the engine feeds max(profile blade clearance, this), so it can only ever add
   * whitespace and can never reintroduce a clipped trailing QR. Both default to 0. */
  uint32_t top_feed_dots;
  uint32_t bottom_feed_dots;

  /* Suppresses the printed verification identifier — the trailer ORDER line and QR
   * (docs/api.md §14). 0, the default, prints them. Inverted so that an all-zeroes
   * pd_job_options still means "print the evidence"; the token stays journaled and
   * pd_job_by_token still resolves it either way. */
  int32_t suppress_verification_id;
} pd_job_options;

/*
 * forceReprint's own options (docs/api.md §3). `suppress_banner` is inverted for the
 * same reason as above: all-zeroes must mean the banner prints. Disabling it is a
 * per-call, deliberate act for a receipt where the banner is inappropriate — a kitchen
 * ticket should never disable it, because the banner is what lets staff bin the
 * duplicate instead of cooking it twice.
 */
typedef struct pd_reprint_options {
  pd_job_options job;
  int32_t suppress_banner;
} pd_reprint_options;

/* --- Callbacks ------------------------------------------------------------------- */

/*
 * The event arrives **by value**, so a callback may keep it. That is deliberate: an FFI
 * consumer whose callback runs after the native call has returned — a Dart
 * NativeCallable.listener, a Java upcall, anything that hops to its own loop — cannot
 * read through a pointer into the emitting worker's stack frame, and a by-value copy is
 * what lets those consumers observe a job transition by transition instead of only
 * replaying its history once it settles.
 */
typedef void (*pd_job_event_cb)(pd_job* job, pd_job_event event, void* ctx);
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

/*
 * A printer reached over a link the caller owns — Bluetooth Classic SPP, BLE, MFi, a
 * vendor SDK channel, a USB bulk pipe, a test double. See the thread contract above
 * pd_transport_vtable.
 *
 * `vtable` is copied before this returns; the function pointers it holds must remain
 * valid, and `ctx` must remain alive, until pd_destroy. `profile_id` accepts the same
 * ids as pd_tcp_config (see pd_profile_ids()); NULL or "" selects "generic".
 * `width_dots` of 0 selects 576.
 *
 * NULL on failure; see pd_last_error.
 */
pd_printer* pd_add_printer_custom(pd_driver* driver, const pd_transport_vtable* vtable,
                                  void* ctx, const char* profile_id, uint32_t width_dots);

/*
 * Deliver bytes the link received. Safe from any thread; see the contract above.
 * Returns 1 when the bytes reached the core's response parser, and 0 when nothing was
 * listening — the core has not connected yet, or has already closed. That is
 * information rather than an error: bytes arriving with no reader are dropped, exactly
 * as they would be on a socket nobody is reading.
 */
int32_t pd_transport_feed_bytes(pd_printer* printer, const uint8_t* data, size_t size);

/*
 * Report that the link dropped for a reason other than an explicit close — the peer
 * went out of range, the OS tore the channel down, pairing was revoked. Surfaces as
 * PD_DEVICE_CONNECTION_LOST and fails any job waiting on a fence, instead of leaving it
 * to time out. Returns 1 when a live transport was notified, 0 when there was none.
 */
int32_t pd_transport_link_dropped(pd_printer* printer, const char* message);

const char* pd_printer_id(pd_printer* printer);
uint32_t pd_printer_width_dots(pd_printer* printer);
pd_completion_mechanism pd_printer_completion(pd_printer* printer);

/*
 * What the fence pd_printer_completion reports is actually worth
 * (docs/compatibility-brief.md §28).
 *
 * PD_PROVENANCE_DOCUMENTED means the manufacturer's own command documentation lists the
 * mechanism for this model — in the shipped database that is Epson and nobody else.
 * PD_PROVENANCE_PROBED means this driver asked the installed hardware over the installed
 * interface path and it answered, which is a stronger claim than any datasheet and is
 * specific to that path. PD_PROVENANCE_UNVERIFIED means neither: a default nobody has
 * confirmed, which is what "ESC/POS compatible" on a datasheet amounts to.
 *
 * This is not a confidence level and does not change what a job reports. It answers a
 * different question — "should I trust this printer's fence before I have printed
 * anything?" — and it is the answer that tells an integrator whether running
 * `pdctl probe` against a site's hardware is worth doing.
 */
pd_provenance pd_printer_completion_provenance(pd_printer* printer);

/* The language this printer's profile is driven in. Anything but PD_LANGUAGE_ESC_POS is
 * refused with PD_REASON_UNSUPPORTED, before a byte is written. */
pd_command_language pd_printer_language(pd_printer* printer);

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

/* Same, with control over the banner. pd_force_reprint is exactly this call with an
 * all-zeroes pd_reprint_options, i.e. with the banner on. */
pd_job* pd_force_reprint_opts(pd_driver* driver, pd_printer* printer, const char* key,
                              const pd_reprint_options* options);

/* Any job this driver knows about, including ones reloaded from the journal after a
 * restart. NULL when the key is unknown. */
pd_job* pd_find_job(pd_driver* driver, const char* key);

/*
 * Paper -> job (docs/api.md §14). Resolves either of a job's four-character
 * verification identifiers, most-recent-first, including jobs reloaded from the
 * journal. NULL when no job on this driver ever carried that token.
 */
pd_job* pd_job_by_token(pd_driver* driver, const char* token);

const char* pd_job_id(pd_job* job);
const char* pd_job_key(pd_job* job);

/*
 * The job's verification identifiers: the token the ticket prints as `V:` and the one
 * its cut fence carried. "" on a printer whose completion mechanism is not GS ( H —
 * there is no wire token to promote — and until the job reaches a worker. Owned by the
 * handle, stable once non-empty.
 */
const char* pd_job_print_token(pd_job* job);
const char* pd_job_cut_token(pd_job* job);

/* The two characters every token this driver issues starts with: which driver instance
 * owns the echo. Persisted in the storage directory, so it survives a restart. */
const char* pd_instance_nonce(pd_driver* driver);

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
const char* pd_confidence_grade_name(pd_confidence_grade value);
const char* pd_completion_authority_name(pd_completion_authority value);
const char* pd_provenance_name(pd_provenance value);
const char* pd_command_language_name(pd_command_language value);
/* "A+", "A".."E" — the letter a report tabulates, where the member name is too long. */
const char* pd_confidence_grade_letter(pd_confidence_grade value);
const char* pd_payload_kind_name(pd_payload_kind value);
const char* pd_completion_mechanism_name(pd_completion_mechanism value);
const char* pd_cut_variant_name(pd_cut_variant value);

/* The non-contiguous code page enum, by member index 0..PD_CODE_PAGE_COUNT-1. */
pd_code_page pd_code_page_at(int32_t index);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* PRINTERDRIVER_PD_H */
