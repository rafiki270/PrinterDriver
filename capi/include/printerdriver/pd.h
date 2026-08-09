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
   * M13b: this is now produced. The core's ePOS client submits under a printjobid,
   * polls with an empty epos-print body until the job is terminal, and returns the
   * retrieved result — which is the durable, queryable job at the top of the hierarchy.
   * The first success=true from a printer whose spooler is enabled is an *enqueue
   * acknowledgement* and never terminates a job, so this grade is only ever attached to
   * a retrieved result, never to an acceptance.
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
  /* --- M13b: Star raw completion (docs/wire-protocols.md §2) ----------------------
   *
   * Appended after PD_COMPLETION_NONE on purpose. These values are mirrored by four
   * wrappers with explicit numbers, so a new member may only ever go at the end;
   * inserting one beside its conceptual neighbours would renumber every mirror.
   *
   * Unlike PD_COMPLETION_STAR_CHECKED_BLOCK — an SDK call, carried as profile data —
   * both of these are wire primitives this core drives itself, and a job on either
   * reports grade PD_GRADE_A_JOB_LEVEL_CONFIRMATION with the physical printer as the
   * authority. PD_COMPLETION_STAR_ETB is only accepted where the driver holds the
   * printer's session exclusively: its ASB counter is broadcast to every host on
   * TCP 9100, so on a shared port it cannot say whose data finished.
   */
  PD_COMPLETION_STAR_ETB = 6,
  PD_COMPLETION_STAR_ESC_GS_ETX = 7,
  PD_COMPLETION_COUNT = 8
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

/* =================================================================================
 * M13b: the print-queue addon through the ABI (docs/sdk-spec.md §12)
 * =================================================================================
 *
 * The queue has existed as a C++ library since the addon landed; this block is the part
 * every other language can reach. It is deliberately the *whole* addon and not a
 * convenience subset, because the three rules below are the addon, and a wrapper that
 * could only enqueue would quietly reintroduce each one as a bug:
 *
 *   1. **A queue is not a retry engine.** A job that ends PD_OUTCOME_UNKNOWN blocks its
 *      printer's lane, and nothing further drains onto that printer until
 *      pd_queue_unblock is called by somebody who has looked at the paper. There is no
 *      timer that clears it, because no timer can see a receipt.
 *   2. **Idempotency keys flow through.** Enqueuing a key that already has a job — held,
 *      printing, or finished months ago — returns that job and prints nothing. The key
 *      is claimed in the driver's own index at enqueue time, so a direct pd_print of the
 *      same key dedupes against a job that is still parked.
 *   3. **No bypass.** Draining runs the identical engine path a direct pd_print takes:
 *      same worker, same preflight, same fences, same confidence grading. A queued job
 *      is an ordinary pd_job throughout — same handle, same id, same event stream, with
 *      PD_JOB_STATE_HELD_OFFLINE appearing in it while the bytes are parked.
 *
 * Lifetime: a pd_queue is created on a pd_driver and MUST be destroyed before it. It is
 * the one handle in this ABI the caller owns and frees, because it is an addon rather
 * than part of the driver's object graph.
 */

typedef struct pd_queue pd_queue;

/* pd::DrainOrder. */
typedef enum pd_drain_order {
  PD_DRAIN_FIFO = 0,     /* submission order — the safe default for sequenced tickets */
  PD_DRAIN_PRIORITY = 1, /* higher priority first, submission order within a priority */
  PD_DRAIN_ORDER_COUNT = 2
} pd_drain_order;

/*
 * pd::QueuePolicy. All-zeroes is valid and means: hold nothing, never expire, unlimited
 * depth, FIFO. That is a pure serializer, which is a deliberate choice rather than a
 * default — see the field comments for what each zero costs.
 */
typedef struct pd_queue_policy {
  /* Park jobs while the printer is known to be offline, coverless or out of paper
   * instead of failing them one at a time. 0 makes the queue a pure serializer. */
  int32_t hold_while_offline;
  /* Shelf life for a held job, in milliseconds; 0 means it never expires. A kitchen
   * ticket must not print into a recovered kitchen half an hour late. */
  uint32_t default_ttl_ms;
  /* Held jobs per printer. 0 is unlimited, which recreates the printer's own buffer
   * problem one layer up; the C++ default is 64 and is what a caller should copy. */
  uint32_t max_depth;
  pd_drain_order drain_order;
} pd_queue_policy;

/* pd::QueueOptions. */
typedef struct pd_queue_options {
  const char* key;      /* idempotency key; NULL or "" -> generated, no dedupe */
  uint32_t ttl_ms;      /* 0 -> the policy default */
  int32_t priority;     /* orders the waiting set only; in flight is never preempted */
  pd_cut cut;
  int32_t open_drawer;
  pd_preflight preflight;
  uint32_t timeout_ms;  /* 0 -> the profile's completion timeout */
} pd_queue_options;

/* NULL only when `driver` is NULL. `policy` may be NULL for the all-zeroes policy. */
pd_queue* pd_queue_create(pd_driver* driver, const pd_queue_policy* policy);

/* Stops the queue thread and frees the handle. Held jobs stay held and stay
 * non-terminal: the queue does not invent an outcome for a job whose fate it does not
 * know. Must be called before pd_destroy on the owning driver. */
void pd_queue_destroy(pd_queue* queue);

/*
 * Returns an ordinary pd_job, never NULL for a valid call. It is already sent when the
 * printer is usable and its lane is free; otherwise it is in PD_JOB_STATE_HELD_OFFLINE,
 * or already terminal with PD_REASON_QUEUE_OVERFLOW when the lane is full. The handle is
 * owned by the driver exactly like a pd_print handle and is freed by pd_destroy.
 */
pd_job* pd_queue_enqueue(pd_queue* queue, pd_printer* printer, const pd_payload* payload,
                         const pd_queue_options* options);

/* Operator hold, independent of what the device is reporting. */
void pd_queue_pause(pd_queue* queue, const char* printer_id);
void pd_queue_resume(pd_queue* queue, const char* printer_id);
int32_t pd_queue_is_paused(pd_queue* queue, const char* printer_id);

/*
 * 1 once a job on this printer ended PD_OUTCOME_UNKNOWN. The lane drains nothing more
 * until pd_queue_unblock — rule 1 above, and the reason this is a function and not a
 * timeout.
 */
int32_t pd_queue_is_blocked(pd_queue* queue, const char* printer_id);
void pd_queue_unblock(pd_queue* queue, const char* printer_id);

/* Held jobs. `printer_id` NULL or "" counts every lane. */
size_t pd_queue_pending(pd_queue* queue, const char* printer_id);

size_t pd_queue_expired_count(pd_queue* queue);
size_t pd_queue_overflow_count(pd_queue* queue);
size_t pd_queue_drained_count(pd_queue* queue);

/* Runs one expiry-and-drain pass on the calling thread. The queue's own thread does this
 * on every device event and whenever a TTL comes due; this is for hosts that would rather
 * pump it themselves, and for tests. */
void pd_queue_tick(pd_queue* queue);

const char* pd_drain_order_name(pd_drain_order value);

/* ==================================================================================
 * M14 — CASH DRAWER (docs/cash-drawer.md)
 * ==================================================================================
 *
 * The drawer is a separate printer peripheral, not a feature of the printer. It has its
 * own electrical profile, its own command method and its own feedback method, and none
 * of the three follows from anything the printer does with paper. Everything this
 * milestone adds to the ABI is in this one block.
 *
 * Two rules from that document shape the whole surface:
 *
 *   1. RJ11/RJ12-LOOKING DRAWER CONNECTORS ARE NOT A UNIVERSAL ELECTRICAL STANDARD.
 *      Star's identical-looking 6P6C socket puts +24 V on pin 3 and the sense line on
 *      pin 6, exactly where Epson puts sense and ground; 12 V outputs exist alongside
 *      the common 24 V ones. A port whose standard is PD_DRAWER_PORT_UNKNOWN is never
 *      pulsed by this ABI, whatever the caller asks for. pd_drawer_open returns
 *      PD_DRAWER_UNKNOWN and writes zero bytes.
 *   2. NEVER A BOOLEAN. `{sent: true}` cannot distinguish "the pulse went out" from "we
 *      watched the microswitch change", and those are the two facts an operator needs
 *      apart. So the answer is a pd_drawer_state out of a closed set, and
 *      PD_DRAWER_KICK_SENT_UNVERIFIED is a real answer rather than a weak success.
 */

/* pd::DrawerState — docs/cash-drawer.md "API states (never a boolean)". */
typedef enum pd_drawer_state {
  PD_DRAWER_CLOSED = 0,
  PD_DRAWER_OPEN = 1,
  PD_DRAWER_OPENING = 2,
  /* The link accepted the pulse and nothing can confirm what happened next: no switch
   * on this port, or a path whose sensor answer never comes back. Through a cheap USB
   * print server the kick travels forward while the response does not, and that path
   * supports this state and never PD_DRAWER_OPEN_VERIFIED. */
  PD_DRAWER_KICK_SENT_UNVERIFIED = 3,
  /* The only member that claims physical movement. */
  PD_DRAWER_OPEN_VERIFIED = 4,
  PD_DRAWER_FAILED_TO_OPEN = 5,
  /* This port has no switch input wired or documented. Not an error. */
  PD_DRAWER_NO_SENSOR = 6,
  PD_DRAWER_UNKNOWN = 7,
  PD_DRAWER_STATE_COUNT = 8
} pd_drawer_state;

/* pd::DrawerPortStandard — the four buckets that cover the deployed fleet. */
typedef enum pd_drawer_port_standard {
  /* 1 FG, 2 kick 1, 3 sensor, 4 +24 V, 5 kick 2, 6 signal ground. Epson, Citizen, much
   * of Bixolon and many clones: one large interoperable ecosystem. */
  PD_DRAWER_PORT_EPSON_24V_6P6C = 0,
  /* The same plug, +24 V on pin 3 and sense on pin 6. A cable that fits is not
   * electrically correct. */
  PD_DRAWER_PORT_STAR_24V_6P6C = 1,
  PD_DRAWER_PORT_GENERIC_12V_6P6C = 2,
  PD_DRAWER_PORT_UNKNOWN = 3,
  PD_DRAWER_PORT_STANDARD_COUNT = 4
} pd_drawer_port_standard;

/* pd::DrawerKickMethod. The software command and the cable pinout are independent
 * facts: two printers accepting the same "kick drawer 1" command may still wire the
 * modular port differently. */
typedef enum pd_drawer_kick_method {
  PD_DRAWER_KICK_EPSON_ESC_P = 0,
  PD_DRAWER_KICK_EPSON_EPOS = 1,
  PD_DRAWER_KICK_STAR_PRNT = 2,
  PD_DRAWER_KICK_BIXOLON_SDK = 3,
  PD_DRAWER_KICK_CITIZEN_ESC_P = 4,
  PD_DRAWER_KICK_SNBC_ESC_P = 5,
  PD_DRAWER_KICK_VENDOR = 6,
  PD_DRAWER_KICK_UNSUPPORTED = 7,
  PD_DRAWER_KICK_METHOD_COUNT = 8
} pd_drawer_kick_method;

/* pd::DrawerStatusMethod — how the switch is read back. */
typedef enum pd_drawer_status_method {
  PD_DRAWER_STATUS_GS_R2 = 0,
  PD_DRAWER_STATUS_ASB = 1,
  PD_DRAWER_STATUS_STAR_SIGNAL = 2,
  PD_DRAWER_STATUS_VENDOR_SDK = 3,
  PD_DRAWER_STATUS_NONE = 4,
  PD_DRAWER_STATUS_METHOD_COUNT = 5
} pd_drawer_status_method;

/*
 * The drawer facet of a printer's capability profile, flattened. `voltage` and
 * `max_current_ma` are 0 where the manufacturer does not document them, which is not
 * the same as "low". `sensor_pin` is 3 on the Epson arrangement and 6 on Star's.
 *
 * `electrical_provenance` and `commands_provenance` are deliberately separate columns:
 * the XP-S260M's DC 24 V / 1 A drawer output is in Xprinter's own specification while
 * nothing they publish proves the pulse or the status command, and collapsing those
 * into one "documented" flag is what the whole provenance system exists to prevent.
 *
 * `shared_between_drawers` is the "two outputs, ONE switch input" case: both channels
 * kick independently and the only readable fact is that some attached drawer is open.
 * `shared_with_buzzer` is Epson's documented conflict — with the optional external
 * buzzer enabled the pulse sounds the buzzer instead of firing the drawer.
 */
typedef struct pd_drawer_capabilities {
  int32_t present;
  pd_drawer_port_standard standard;
  uint16_t voltage;
  uint16_t max_current_ma;
  uint8_t channel_count;
  uint8_t sensor_pin;
  pd_drawer_kick_method method;
  uint16_t default_pulse_ms;
  uint16_t max_pulse_ms;
  uint16_t cooldown_ms;
  int32_t can_kick_during_print;
  int32_t status_available;
  pd_drawer_status_method status_method;
  int32_t shared_between_drawers;
  int32_t shared_with_buzzer;
  pd_provenance electrical_provenance;
  pd_provenance commands_provenance;
  /* 1 when this engine may put a pulse on the wire for this profile: a drivable method
   * AND an established electrical standard. A caller that reads nothing else should
   * read this. */
  int32_t kickable;
} pd_drawer_capabilities;

/* 0 asks for the profile's own default in both fields. `channel` is 1 for drive 1
 * (Epson pin 2) and 2 for drive 2 (pin 5); out-of-range values are clamped to what the
 * profile documents rather than refused. */
typedef struct pd_drawer_request {
  uint8_t channel;
  uint16_t pulse_ms;
} pd_drawer_request;

/*
 * The answer docs/cash-drawer.md asks for by name. `elapsed_ms` is the interval between
 * the pulse leaving for the link and the verdict — the "sensor changed 143 ms after
 * kick" number — and is 0 when there was nothing to wait for. `pulse_ms` is 0 when no
 * pulse was emitted at all, which happens when the drawer was already open and when the
 * call was refused.
 */
typedef struct pd_drawer_result {
  pd_drawer_state state;
  pd_drawer_state previous_state;
  uint8_t channel;
  uint16_t pulse_ms;
  uint32_t elapsed_ms;
} pd_drawer_result;

/*
 * One non-destructive read of the drawer switch. `pin_high` is PD_UNKNOWN, PD_FALSE or
 * PD_TRUE. `needs_calibration` is 1 until this printer's polarity has been measured,
 * and while it is 1 `state` stays PD_DRAWER_UNKNOWN however clear the level is: Star
 * documents that the meaning of the signal depends on the attached drawer, so this ABI
 * reports a level it cannot interpret rather than guessing a direction.
 */
typedef struct pd_drawer_reading {
  int32_t available;
  int32_t answered;
  int32_t pin_high;
  int32_t needs_calibration;
  pd_drawer_state state;
} pd_drawer_reading;

/* The profile's drawer facet. All-zeroes for a NULL or foreign handle, which reads as
 * "no drawer, unsupported method, unclassified port" — the safe answer. */
pd_drawer_capabilities pd_printer_drawer_capabilities(pd_printer* printer);

/*
 * The verified opening sequence. Blocks until it reaches a verdict.
 *
 *   1. read the switch first — an already-open drawer is never pulsed again;
 *   2. run on the printer's own worker, so the pulse cannot interleave with a fenced
 *      job's bytes;
 *   3. one queued ESC p at the profile's duration on the requested channel;
 *   4. watch the switch for the profile's verification window;
 *   5. it changed -> PD_DRAWER_OPEN_VERIFIED;
 *   6. it did not -> PD_DRAWER_FAILED_TO_OPEN;
 *   7. nothing can answer -> PD_DRAWER_KICK_SENT_UNVERIFIED;
 *   8. hold the manufacturer cooldown before the next pulse.
 *
 * `request` may be NULL for the profile's defaults. Refused — zero bytes written,
 * PD_DRAWER_UNKNOWN — when the profile has no drawer port, when its kick method is one
 * this engine cannot drive (Star's peripheral command, ePOS, a vendor SDK), or when the
 * electrical standard is PD_DRAWER_PORT_UNKNOWN.
 */
pd_drawer_result pd_drawer_open(pd_driver* driver, pd_printer* printer,
                                const pd_drawer_request* request);

/* Never pulses, so it is safe on hardware pd_drawer_open refuses. timeout_ms of 0
 * selects 1500. */
pd_drawer_reading pd_drawer_read_sensor(pd_driver* driver, pd_printer* printer,
                                        uint32_t timeout_ms);

/*
 * Records which pin level means "open" for the drawer physically attached to this
 * printer, and persists it in the storage directory so it outlives the process. The
 * calling sequence is the operator one: ask for the drawer to be closed, read the
 * sensor, ask for it to be opened, read again, and pass the level observed while it was
 * open. Returns 1 when it was persisted, 0 when it applies to this process only (an
 * in-memory driver, or a storage directory that cannot be written).
 */
int32_t pd_drawer_calibrate_polarity(pd_driver* driver, pd_printer* printer,
                                     int32_t high_means_open);

/* 1 once a polarity has been measured for this printer. */
int32_t pd_drawer_polarity_calibrated(pd_driver* driver, pd_printer* printer);
/* Meaningful only while pd_drawer_polarity_calibrated returns 1. */
int32_t pd_drawer_high_means_open(pd_driver* driver, pd_printer* printer);

/* Static storage, never NULL, matching the core's own spelling one-for-one. */
const char* pd_drawer_state_name(pd_drawer_state value);
const char* pd_drawer_port_standard_name(pd_drawer_port_standard value);
const char* pd_drawer_kick_method_name(pd_drawer_kick_method value);
const char* pd_drawer_status_method_name(pd_drawer_status_method value);

/* ============================== end M14 ========================================== */

/* =================================================================================
 * M15: self-test, auto-detection and LAN discovery (docs/api.md §15)
 * =================================================================================
 *
 * Three calls, in increasing order of what they are allowed to do to a device:
 *
 *   pd_discover    sweeps a subnet. The only bytes it ever writes are DLE EOT 1
 *                  (10 04 01), the real-time status query. A port-9100 device prints
 *                  what it receives, so a scanner that wrote anything printable would
 *                  cost a venue a roll of paper per run.
 *   pd_auto_detect discovery + identification + the PRINTLESS subset of the capability
 *                  probe, with the stored-findings cache respected. Still nothing
 *                  prints and nothing fires.
 *   pd_self_test   prints exactly one diagnostic ticket, through the ordinary fenced
 *                  engine, under an ordinary idempotency key.
 *
 * -- What auto-detection may not claim ---------------------------------------------
 *
 * An ordered completion fence only means anything when there is print data ahead of it
 * for the device to finish first. pd_auto_detect has no print data, so the fences go out
 * behind an empty buffer: a device that echoes them has proved that its firmware
 * IMPLEMENTS the command, not that the echo waits for paper to move. The flag is
 * therefore promoted and its provenance is not — `completion_provenance` stays
 * PD_PROVENANCE_UNVERIFIED on a printless answer, and the reason is spelled out in the
 * summary's degradation list. Full promotion still needs the printing probe
 * (`pdctl probe`) or a real job.
 *
 * -- String lifetime ----------------------------------------------------------------
 *
 * Callback structs and every string inside them are valid ONLY for the duration of the
 * callback: copy what you need. pd_self_test_result's strings are owned by the driver
 * and valid until the next pd_self_test call on that same driver.
 */

/* pd::ProfileSelection — how the capability profile in force was arrived at. Not a
 * provenance: that says where a claim about one capability comes from, and this says
 * where the profile came from. */
typedef enum pd_profile_selection {
  /* A device-database entry matched what the device reported about itself. */
  PD_PROFILE_SELECTED_DOCUMENTED = 0,
  /* A probe's first-hand findings promoted whatever was selected. */
  PD_PROFILE_SELECTED_PROBED = 1,
  /* Neither: the shipped default is the whole truth, which means UNKNOWN DEVICE
   * rather than ordinary device. */
  PD_PROFILE_SELECTED_DEFAULT = 2,
  PD_PROFILE_SELECTION_COUNT = 3
} pd_profile_selection;

/* pd::DetectionStatus — what auto-detection established about one address. */
typedef enum pd_detection_status {
  /* The backchannel answered: identification, fences, or both. */
  PD_DETECTION_ANSWERED = 0,
  /* The port accepted the connection and said nothing at all. A real finding — the
   * interface that does not forward status bytes — and never rendered as a failure. */
  PD_DETECTION_SILENT = 1,
  /* Reachable and deliberately not interrogated: leave_unknown_unprobed was set and
   * nothing is cached. Never rendered as "no capabilities": nobody asked. */
  PD_DETECTION_UNVERIFIED = 2,
  PD_DETECTION_UNREACHABLE = 3,
  PD_DETECTION_STATUS_COUNT = 4
} pd_detection_status;

/*
 * The detection report, flattened. Shared by the self-test and by auto-detection so the
 * paper, the CLI, the agent and every wrapper describe the same device the same way.
 */
typedef struct pd_detection_summary {
  const char* endpoint;

  const char* vendor;
  const char* model;
  const char* firmware;
  const char* serial;
  /* 1 only when a signal independent of GS I agrees with GS I. Rongta's own manual
   * documents its printers answering "EPOSN" / "TM-T88V", so this is 0 by default and
   * `confidence_percent` is what it is worth. */
  int32_t identity_trusted;
  uint8_t confidence_percent;
  int32_t impersonation_suspected;
  /* 1 when the identification above came from this call rather than from the cache. */
  int32_t identity_fresh;

  const char* profile_id;
  pd_profile_selection selection;

  /* Roll width and raster width are separate facts, and neither is derived from the
   * other: a 112 mm-media printer prints 104 mm. */
  uint16_t nominal_paper_mm;
  uint32_t printable_width_dots;
  uint32_t chars_per_line;
  uint16_t dpi;

  pd_completion_mechanism completion;
  /* The best grade a job on this printer can ever claim. */
  pd_confidence_grade grade_ceiling;
  pd_completion_authority authority;
  const char* method;
  pd_provenance completion_provenance;

  int32_t drawer_present;
  int32_t drawer_kickable;
  pd_drawer_port_standard drawer_standard;
  uint16_t drawer_voltage;
  pd_provenance drawer_electrical_provenance;
  pd_provenance drawer_commands_provenance;

  /* One line, for a table row:
   * "GS(H) fn48 Probed - profile Probed - identity untrusted (35%)". */
  const char* provenance_summary;

  /* Everything that was requested and not delivered, in the words it is printed in —
   * "BARCODE not supported on this path" and its relatives. `degradation_count`
   * entries; NULL when there are none. */
  const char* const* degradations;
  size_t degradation_count;
} pd_detection_summary;

/* --- pd_self_test ----------------------------------------------------------------- */

/* All-zeroes is the useful call. Every field that inverts a default is spelled as the
 * negative so that memset(0) means "the documented behaviour". */
typedef struct pd_self_test_options {
  /* NULL or "" -> "selftest-<unix ms>". A real idempotency key on a real job: running
   * the same key twice does not print twice. */
  const char* key;
  /* Interrogate the device now instead of using what is already known. Runs the same
   * probe pd_add_printer_tcp schedules, on the printer's own worker thread. */
  int32_t refresh_identity;
  /* Keep that refresh printless, at the cost of asking the ordered fences out of an
   * empty buffer. Only meaningful with refresh_identity. */
  int32_t probe_without_printing;
  /* Omit the Code 128 sample. Note that a profile with no barcode path omits it anyway,
   * with a declared degradation printed on the ticket. */
  int32_t no_barcode;
  const char* barcode_data; /* NULL -> "PD-SELFTEST" */
  /* Suppress the trailer `V:` line and its QR. On a GS ( H printer that QR carries the
   * job's own verification token, which is what makes the paper evidence. */
  int32_t no_verification_id;
  uint32_t timeout_ms; /* 0 -> the profile's completion budget */
} pd_self_test_options;

/*
 * The self-test's answer: the ordinary tri-state job result, plus what the ticket said.
 * `ticket_text` is the ticket exactly as it was laid out, lines separated by '\n' — the
 * same layout that produced the bytes, never a second one. `job` is the ordinary job
 * handle, so the ticket can be resolved by key or by token afterwards.
 *
 * Every string here is owned by the driver and valid until the next pd_self_test call
 * on it.
 */
typedef struct pd_self_test_result {
  pd_job_result result;
  pd_detection_summary detection;
  const char* key;
  /* The four GS ( H characters printed as `V:` and inside the QR. "" on a profile with
   * no wire token to promote. */
  const char* print_token;
  const char* ticket_text;
  pd_job* job;
} pd_self_test_result;

/*
 * Prints ONE diagnostic ticket and blocks until the job is terminal. `options` may be
 * NULL. Returns 1 and fills `out`; 0 on a bad handle or a null `out`, see pd_last_error.
 *
 * A Done at PD_GRADE_A_JOB_LEVEL_CONFIRMATION here is the statement that the whole
 * stack works end to end on this unit, over this interface path — because the ticket
 * went through the ordinary engine, with the ordinary preflight, fence and grading, and
 * the result is what that engine reported.
 */
int32_t pd_self_test(pd_driver* driver, pd_printer* printer,
                     const pd_self_test_options* options, pd_self_test_result* out);

/* --- pd_auto_detect ---------------------------------------------------------------- */

typedef struct pd_auto_detect_options {
  /* NULL or "" -> the local /24. Ignored when `endpoints` is set. */
  const char* subnet_cidr;
  /* A NULL-terminated array of "host" or "host:port" strings. When present the sweep is
   * skipped and exactly these addresses are examined — the path for a caller with a
   * known inventory, and the only one that reports an unreachable address, because it
   * is the only one where somebody named it. */
  const char* const* endpoints;
  uint16_t port;                 /* 0 -> 9100 */
  uint32_t concurrency;          /* 0 -> 16 */
  uint32_t connect_timeout_ms;   /* 0 -> 300 */
  uint32_t response_timeout_ms;  /* 0 -> 400 */
  /* Leave devices nobody has interrogated alone: cached findings still apply, and
   * anything untouched comes back PD_DETECTION_UNVERIFIED instead of being asked. */
  int32_t leave_unknown_unprobed;
  uint32_t status_timeout_ms;      /* 0 -> 700 */
  uint32_t identity_timeout_ms;    /* 0 -> 700 */
  uint32_t completion_timeout_ms;  /* 0 -> 1200 */
} pd_auto_detect_options;

typedef struct pd_detected_printer {
  const char* endpoint; /* "192.168.1.101:9100" */
  const char* host;
  uint16_t port;
  pd_detection_status status;
  int32_t port_open;
  /* 1 when the classification came from stored findings rather than from bytes
   * exchanged in this call. */
  int32_t from_cache;
  /* Whatever DLE EOT 1 answered during the sweep, as uppercase hex. "" when the port
   * accepted the connection and said nothing. */
  const char* dle_eot_hex;
  pd_detection_summary summary;
} pd_detected_printer;

/* Fired as each candidate is finished, from a worker thread, one at a time. The struct
 * and its strings are valid only for the duration of the call. */
typedef void (*pd_detected_cb)(const pd_detected_printer* printer, uint64_t completed,
                              uint64_t total, void* ctx);

/*
 * Sweeps, identifies and classifies. NOTHING PRINTS AND NOTHING FIRES. Blocks until
 * every candidate is finished and returns how many were reported, or -1 on error (see
 * pd_last_error — a malformed CIDR, or one wider than /16, is the usual cause).
 * `options` and `cb` may both be NULL.
 */
int32_t pd_auto_detect(pd_driver* driver, const pd_auto_detect_options* options,
                       pd_detected_cb cb, void* ctx);

/* --- pd_discover ------------------------------------------------------------------- */

/*
 * The raw sweep underneath pd_auto_detect: is something ESC/POS-shaped listening, and
 * does its backchannel reach me? No identification, no capability probe, no profile —
 * deciding what a device *is* costs time and belongs to pd_auto_detect.
 *
 * The whole write side is DLE EOT 1. Every byte of it is below 0x20, so none of it can
 * print, on any device, ever.
 */
typedef struct pd_discover_options {
  const char* subnet_cidr;       /* NULL or "" -> the local /24 */
  uint16_t port;                 /* 0 -> 9100 */
  uint32_t concurrency;          /* 0 -> 32 */
  uint32_t connect_timeout_ms;   /* 0 -> 300 */
  uint32_t response_timeout_ms;  /* 0 -> 400 */
  /* Turns the sweep into a pure port scan: the port state is still reported and not one
   * byte is written. */
  int32_t no_backchannel_probe;
} pd_discover_options;

typedef struct pd_discovered_device {
  const char* ip;
  uint16_t port;
  int32_t port9100_open;
  /* DLE EOT 1's answer as uppercase hex, verbatim and unclassified. "" means the port
   * accepted the connection and said nothing — a LAN module that does not forward
   * status bytes, which is a finding and not a failure. */
  const char* dle_eot_hex;
} pd_discovered_device;

/* Fired for every OPEN port as it is found, from a worker thread, one at a time. Valid
 * only for the duration of the call. */
typedef void (*pd_discovered_cb)(const pd_discovered_device* device, uint64_t completed,
                                uint64_t total, void* ctx);

/*
 * Sweeps and reports the open ports. Blocks. Returns how many were found, or -1 on
 * error (a malformed CIDR, one wider than /16, or no local subnet to guess). `options`
 * and `cb` may both be NULL.
 *
 * `completed` and `total` count every address the sweep finishes, open or not, so a UI
 * can show progress across a whole /24 while only the open ones arrive as devices.
 */
int32_t pd_discover(pd_driver* driver, const pd_discover_options* options,
                    pd_discovered_cb cb, void* ctx);

/*
 * The last sweep's results, by index, for a wrapper that cannot service a callback while
 * it is blocked inside the call that fires it — which is every FFI binding whose runtime
 * owns the calling thread, Dart above all. `pd_auto_detect` and `pd_discover` keep their
 * results on the driver; these read them back.
 *
 * Returns 1 and fills `out`, or 0 when the index is out of range. Every string in `out`
 * is owned by the driver and valid until the next call to the same reader, or to
 * pd_auto_detect / pd_discover, on that driver: copy before asking for the next index.
 */
int32_t pd_detected_at(pd_driver* driver, int32_t index, pd_detected_printer* out);
int32_t pd_discovered_at(pd_driver* driver, int32_t index, pd_discovered_device* out);

/* The /24 around this host's primary address, as "192.168.1.0/24", or "" when it cannot
 * be determined. Found by asking the routing table which local address would be used to
 * reach a remote one; no packet is transmitted. Owned by the driver, valid until the
 * next call on it. */
const char* pd_local_subnet(pd_driver* driver);

/* Static storage, never NULL, matching the core's own spelling one-for-one. */
const char* pd_profile_selection_name(pd_profile_selection value);
const char* pd_detection_status_name(pd_detection_status value);

/* ============================== end M15 ========================================== */

/* =================================================================================
 * M16 — CUSTOM METHOD REGISTRATION (docs/api.md §16)
 * =================================================================================
 *
 * Five runtime extension points that let an integrator extend the SDK without forking it.
 * All are per-driver-instance, all are data-plus-function-pointers (no subclassing across
 * this ABI), all are keyed by namespaced string ids ("acme.x-idle"), all are process-local
 * (never persisted into a shared journal beyond their ids), and everything a registration
 * claims — a completion grade, an authority, a formatter name — is attributed to it by id
 * in the result and in `pdctl verify`, so a custom method's claims are auditable exactly
 * like a built-in's.
 *
 *   pd_register_completion_method — the marquee. A vendor idle/ack scheme becomes a real
 *     graded completion path with no core release: the engine sends fence_bytes(token)
 *     behind the payload and routes the printer's continuous response stream through the
 *     matcher, and a Matched(token) confirms the job EXACTLY like GS ( H — the same
 *     per-job token map, the same journalled RVI, the same jobByToken / `pdctl verify`
 *     resolution. It coexists with the built-in parser: the raw bytes are fed to the
 *     matcher ALONGSIDE the GS ( H / GS r / ASB parser, so ForeignWriterDetected and the
 *     queued/realtime status paths keep working. A profile uses a registered method when
 *     its completion is PD_COMPLETION_VENDOR_IDLE and its vendor id is this id; select
 *     such a profile at attach time with a profile id of the form "vendoridle:<id>"
 *     (e.g. pd_add_printer_custom(..., "vendoridle:acme.x-idle", ...)), which resolves to
 *     a generic ESC/POS profile bound to the registered method.
 *   pd_register_probe_step — extends probe/autoDetect fingerprinting. request_bytes MUST
 *     be non-printing (no 0x20-0x7E run, no line feed); a printing step is refused at
 *     registration, because autoDetect must never cost a venue a roll of paper.
 *   pd_register_block_handler — a new DSL block kind renders through the ordinary pipeline.
 *   pd_register_formatter — backs {{ v | name:args }}, checked before the built-in table.
 *   pd_register_drawer_kick — fills PD_DRAWER_KICK_VENDOR for a profile.
 *
 * -- Callback threads --------------------------------------------------------------
 * Every callback below is invoked on a CORE thread, never the caller's — the same
 * contract the custom-transport vtable documents above:
 *   - a completion method's fence_bytes / matcher run on the owning printer's worker
 *     thread and the transport reader path;
 *   - a probe step's classify runs on the worker thread driving the probe;
 *   - a drawer method's callbacks run on the worker thread;
 *   - a formatter / block handler runs on whatever thread renders the document.
 * A callback must not block and must not call back into any pd_* function on the same
 * driver, for the reason given under "Callback threads" at the top of this header.
 *
 * -- Lifetime and return convention ------------------------------------------------
 * Every pd_register_* returns 1 on success and 0 on failure (a bad or duplicate id, a
 * missing required callback, a printing probe request); pd_last_error explains a 0. The
 * struct is copied before the call returns, exactly like pd_add_printer_custom's vtable;
 * the function pointers it holds and every `ctx` must stay valid until pd_destroy. Strings
 * (`id`, `name`, `method_name`) are copied. Callbacks that hand back a variable-length
 * result write it into a caller-supplied buffer and return its length; a result longer
 * than the buffer is treated as the registration's error and never truncates silently.
 */

/* --- (1) Custom completion method --------------------------------------------------- */

/* pd::CustomMatchKind — a matcher's verdict on the bytes it was handed. */
typedef enum pd_match_kind {
  PD_MATCH_MATCHED = 0,   /* a fence answer carrying `token`; confirm like a GS ( H echo */
  PD_MATCH_NOT_MINE = 1,  /* not this mechanism's bytes; the core drops its matcher buffer */
  PD_MATCH_NEED_MORE = 2, /* an answer may be forming but is incomplete; keep buffering */
  PD_MATCH_KIND_COUNT = 3
} pd_match_kind;

typedef struct pd_match_result {
  pd_match_kind kind;
  /* For PD_MATCH_MATCHED: the four-character correlation token, NUL-terminated. The core
   * copies it out before the matcher returns, so this fixed buffer never escapes. */
  char token[8];
} pd_match_result;

/*
 * Produce the fence bytes to send behind the payload for this job's four-character
 * verification token (NUL-terminated). Writes up to `cap` bytes into `out` and returns the
 * number written; a fence longer than `cap` is a registration the core cannot honour and
 * the job it fences fails Unknown rather than sending a truncated fence.
 */
typedef size_t (*pd_fence_bytes_fn)(void* ctx, const char* job_token, uint8_t* out,
                                    size_t cap);

/* Classify the printer->host bytes accumulated since the matcher last returned Matched or
 * NotMine. Returned by value, so it may be built on the callback's stack. */
typedef pd_match_result (*pd_completion_matcher_fn)(void* ctx, const uint8_t* data,
                                                    size_t size);

typedef struct pd_completion_method {
  const char* id;                    /* namespaced, e.g. "acme.x-idle"; copied */
  pd_fence_bytes_fn fence_bytes;
  pd_completion_matcher_fn matcher;
  void* ctx;                         /* passed to both callbacks */
  pd_confidence_grade grade;         /* what a confirmed completion on this method claims */
  pd_completion_authority authority;
  const char* method_name;           /* shown in the result and `pdctl verify`; copied.
                                      * NULL or "" -> the id. */
} pd_completion_method;

int32_t pd_register_completion_method(pd_driver* driver,
                                      const pd_completion_method* method);

/* --- (2) Custom probe step ---------------------------------------------------------- */

typedef struct pd_probe_finding {
  int32_t answered;  /* 1 when the device replied to this step at all */
  char label[64];    /* short classification, surfaced in the findings summary; copied */
} pd_probe_finding;

typedef pd_probe_finding (*pd_probe_classify_fn)(void* ctx, const uint8_t* response,
                                                 size_t size);

typedef struct pd_probe_step {
  const char* id;
  const uint8_t* request_bytes;  /* MUST be non-printing: no 0x20-0x7E, no 0x0A; copied */
  size_t request_size;
  pd_probe_classify_fn classify;
  void* ctx;
} pd_probe_step;

int32_t pd_register_probe_step(pd_driver* driver, const pd_probe_step* step);

/* --- (3) Custom document block handler ---------------------------------------------- */

/*
 * Renders a new DSL block kind — a handler registered for a kind always owns it (unknown
 * block kinds otherwise degrade; this intercepts first). `block_json` is the block object;
 * `profile_json` is a small JSON of the render profile facts
 * ({"width_dots":576,"barcode":true,"qr":true,...}). Set *ok to 1 and write raw ESC/POS
 * ops into `out` (return the count), or set *ok to 0 and write a one-line degradation
 * reason into `detail` (a degradation entry is reported the same way a built-in block's
 * is). A result longer than `cap` is an error.
 */
typedef size_t (*pd_block_handler_fn)(void* ctx, const char* block_json,
                                      const char* profile_json, uint8_t* out, size_t cap,
                                      int32_t* ok, char* detail, size_t detail_cap);

typedef struct pd_block_handler {
  const char* kind;  /* the block object key that selects this handler; copied */
  pd_block_handler_fn handler;
  void* ctx;
} pd_block_handler;

int32_t pd_register_block_handler(pd_driver* driver, const pd_block_handler* handler);

/* --- (4) Custom formatter ----------------------------------------------------------- */

/*
 * Backs {{ v | name:args }} in the template layer, checked before the built-in formatter
 * table. Writes the formatted text into `out` and returns its length with *handled 1; set
 * *handled 0 to decline (fall through to the built-ins). A result longer than `cap` is an
 * error. `value` and `args` are the placeholder's value and the text after the first ':';
 * `locale` is the effective locale tag.
 */
typedef size_t (*pd_formatter_fn)(void* ctx, const char* value, const char* args,
                                  const char* locale, char* out, size_t cap,
                                  int32_t* handled);

typedef struct pd_formatter {
  const char* name;  /* the formatter name used in templates; copied */
  pd_formatter_fn formatter;
  void* ctx;
} pd_formatter;

int32_t pd_register_formatter(pd_driver* driver, const pd_formatter* formatter);

/* --- (5) Custom drawer kick method -------------------------------------------------- */

/* Writes the pulse bytes for (channel, pulse_ms) into `out`; returns the count. */
typedef size_t (*pd_drawer_kick_bytes_fn)(void* ctx, uint8_t channel, uint16_t pulse_ms,
                                          uint8_t* out, size_t cap);
/* Optional. Writes the bytes that ask for the switch state; returns the count. */
typedef size_t (*pd_drawer_status_request_fn)(void* ctx, uint8_t* out, size_t cap);
/* Optional. Parses a status reply to a pin level: PD_UNKNOWN, PD_FALSE or PD_TRUE. */
typedef int32_t (*pd_drawer_status_parse_fn)(void* ctx, const uint8_t* response,
                                             size_t size);

typedef struct pd_drawer_kick_reg {
  const char* id;
  pd_drawer_kick_bytes_fn kick_bytes;
  /* status_request and status_parse are optional and go together: both NULL means the
   * vendor method has no readable switch, so a kick reports PD_DRAWER_KICK_SENT_UNVERIFIED
   * rather than a verified open. */
  pd_drawer_status_request_fn status_request;
  pd_drawer_status_parse_fn status_parse;
  void* ctx;
} pd_drawer_kick_reg;

int32_t pd_register_drawer_kick(pd_driver* driver, const pd_drawer_kick_reg* reg);

/* Static storage, never NULL, matching the core's own spelling one-for-one. */
const char* pd_match_kind_name(pd_match_kind value);

/* ============================== end M16 ========================================== */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* PRINTERDRIVER_PD_H */
