#include <string>
#include <vector>

#include "printerdriver/response_parser.hpp"
#include "test_harness.hpp"

using namespace pd;
using namespace pd::escpos;

namespace {

// The exact frame this project's XP-S260M echoed for token P001
// (docs/testing-plan.md, probe run 2026-08-08).
const std::vector<uint8_t> kProbeFrame{0x37, 0x22, 0x50, 0x30, 0x30, 0x31, 0x00};
// The real-time DLE EOT 1 answer from the same probe: online, no error bits.
constexpr uint8_t kProbeRealtimeByte = 0x16;
// The queued GS r 1 answer from the same probe: paper present.
constexpr uint8_t kProbeQueuedByte = 0x00;

// Healthy ASB frame: bit 4 set on every byte, bits 7/1/0 clear; byte 0 bit 2 is the
// drawer connector level.
const std::vector<uint8_t> kAsbHealthy{0x14, 0x10, 0x10, 0x10};
// Offline, cover open, error flagged, cutter error, paper out.
const std::vector<uint8_t> kAsbFaulty{0x18, 0x54, 0x18, 0x30};

std::vector<uint8_t> frameFor(const std::string& token) {
  std::vector<uint8_t> out{0x37, 0x22};
  out.insert(out.end(), token.begin(), token.end());
  out.push_back(0x00);
  return out;
}

void appendAll(std::vector<uint8_t>& out, const std::vector<uint8_t>& more) {
  out.insert(out.end(), more.begin(), more.end());
}

std::vector<ParsedEvent> feedByteByByte(ResponseParser& parser,
                                        const std::vector<uint8_t>& stream) {
  std::vector<ParsedEvent> all;
  for (const uint8_t byte : stream) {
    const std::vector<ParsedEvent> events = parser.feed(&byte, 1);
    all.insert(all.end(), events.begin(), events.end());
  }
  return all;
}

std::vector<ParsedEventKind> kindsOf(const std::vector<ParsedEvent>& events) {
  std::vector<ParsedEventKind> kinds;
  kinds.reserve(events.size());
  for (const ParsedEvent& event : events) {
    kinds.push_back(event.kind);
  }
  return kinds;
}

}  // namespace

PD_TEST(gs_h_frame_from_the_real_probe) {
  ResponseParser parser;
  const std::vector<ParsedEvent> events = parser.feed(kProbeFrame);
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  if (events.size() == 1) {
    CHECK_EQ(events[0].kind, ParsedEventKind::GsHAck);
    CHECK_EQ(events[0].token, std::string("P001"));
  }
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(0));
}

PD_TEST(gs_h_frame_split_at_every_boundary) {
  for (size_t split = 1; split < kProbeFrame.size(); ++split) {
    ResponseParser parser;
    std::vector<ParsedEvent> all = parser.feed(kProbeFrame.data(), split);
    CHECK_EQ(all.size(), static_cast<size_t>(0));
    const std::vector<ParsedEvent> rest =
        parser.feed(kProbeFrame.data() + split, kProbeFrame.size() - split);
    all.insert(all.end(), rest.begin(), rest.end());
    CHECK_EQ(all.size(), static_cast<size_t>(1));
    if (all.size() == 1) {
      CHECK_EQ(all[0].kind, ParsedEventKind::GsHAck);
      CHECK_EQ(all[0].token, std::string("P001"));
    }
    CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(0));
  }
}

PD_TEST(gs_h_frame_fed_one_byte_at_a_time) {
  ResponseParser parser;
  const std::vector<ParsedEvent> events = feedByteByByte(parser, kProbeFrame);
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  if (events.size() == 1) {
    CHECK_EQ(events[0].token, std::string("P001"));
  }
}

PD_TEST(asb_frame_recognised_between_two_gs_h_frames) {
  ResponseParser parser;
  std::vector<uint8_t> stream;
  appendAll(stream, frameFor("P7K2"));
  appendAll(stream, kAsbHealthy);
  appendAll(stream, frameFor("C7K2"));

  const std::vector<ParsedEvent> events = parser.feed(stream);
  CHECK_EQ(events.size(), static_cast<size_t>(3));
  if (events.size() != 3) {
    return;
  }
  CHECK_EQ(events[0].kind, ParsedEventKind::GsHAck);
  CHECK_EQ(events[0].token, std::string("P7K2"));
  CHECK_EQ(events[1].kind, ParsedEventKind::AsbStatus);
  CHECK_EQ(events[2].kind, ParsedEventKind::GsHAck);
  CHECK_EQ(events[2].token, std::string("C7K2"));

  CHECK(events[1].flags.online.value_or(false));
  CHECK(!events[1].flags.cover_open.value_or(true));
  CHECK(!events[1].flags.paper_out.value_or(true));
  CHECK(!events[1].flags.cutter_error.value_or(true));

  const std::vector<DeviceEvent> device = toDeviceEvents(events[1].flags);
  CHECK_EQ(device.size(), static_cast<size_t>(3));
  if (device.size() == 3) {
    CHECK_EQ(device[0], DeviceEvent::Online);
    CHECK_EQ(device[1], DeviceEvent::CoverClosed);
    CHECK_EQ(device[2], DeviceEvent::PaperOk);
  }

  // Same result when the whole thing arrives one byte at a time.
  ResponseParser drip;
  const std::vector<ParsedEvent> dripped = feedByteByByte(drip, stream);
  CHECK(kindsOf(dripped) == kindsOf(events));
}

PD_TEST(asb_faults_decode_to_device_events) {
  ResponseParser parser;
  const std::vector<ParsedEvent> events = parser.feed(kAsbFaulty);
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  if (events.size() != 1) {
    return;
  }
  CHECK_EQ(events[0].kind, ParsedEventKind::AsbStatus);
  CHECK(!events[0].flags.online.value_or(true));
  CHECK(events[0].flags.cover_open.value_or(false));
  CHECK(events[0].flags.error.value_or(false));
  CHECK(events[0].flags.cutter_error.value_or(false));
  CHECK(events[0].flags.paper_out.value_or(false));

  const std::vector<DeviceEvent> device = toDeviceEvents(events[0].flags);
  CHECK_EQ(device.size(), static_cast<size_t>(4));
  if (device.size() == 4) {
    CHECK_EQ(device[0], DeviceEvent::Offline);
    CHECK_EQ(device[1], DeviceEvent::CoverOpen);
    CHECK_EQ(device[2], DeviceEvent::PaperOut);
    CHECK_EQ(device[3], DeviceEvent::CutterError);
  }
}

PD_TEST(realtime_answer_wins_over_an_outstanding_queued_expectation) {
  // The whole point of the precedence rule: a DLE EOT answer can overtake queued
  // print data (docs/techspec.md §3.3), so it must not be mistaken for the GS r
  // completion fence the caller is still waiting for.
  ResponseParser parser;
  parser.expectQueued();
  parser.expectRealtime(DleEotKind::PrinterStatus);

  const std::vector<uint8_t> stream{kProbeRealtimeByte};
  const std::vector<ParsedEvent> events = parser.feed(stream);
  CHECK_EQ(events.size(), static_cast<size_t>(1));
  if (events.size() == 1) {
    CHECK_EQ(events[0].kind, ParsedEventKind::RealtimeStatus);
    CHECK_EQ(static_cast<unsigned>(events[0].byte), 0x16u);
    CHECK_EQ(events[0].realtime_kind, DleEotKind::PrinterStatus);
    CHECK(events[0].flags.online.value_or(false));
    CHECK(events[0].flags.drawer_pin_high.value_or(false));
  }
  CHECK_EQ(parser.outstandingRealtime(), static_cast<size_t>(0));
  CHECK_EQ(parser.outstandingQueued(), static_cast<size_t>(1));
}

PD_TEST(queued_answer_goes_to_the_oldest_queued_expectation) {
  ResponseParser parser;
  parser.expectQueued();
  parser.expectQueued();

  const std::vector<uint8_t> stream{kProbeQueuedByte, 0x0C};
  const std::vector<ParsedEvent> events = parser.feed(stream);
  CHECK_EQ(events.size(), static_cast<size_t>(2));
  if (events.size() == 2) {
    CHECK_EQ(events[0].kind, ParsedEventKind::QueuedStatus);
    CHECK_EQ(static_cast<unsigned>(events[0].byte), 0x00u);
    CHECK_EQ(events[1].kind, ParsedEventKind::QueuedStatus);
    CHECK_EQ(static_cast<unsigned>(events[1].byte), 0x0Cu);
  }
  CHECK_EQ(parser.outstandingQueued(), static_cast<size_t>(0));
}

PD_TEST(realtime_pattern_falls_through_when_no_realtime_is_expected) {
  // Documented consequence of the precedence order: without an outstanding
  // real-time expectation, a byte carrying the DLE EOT signature is not evidence of
  // anything the caller asked for, so a waiting queued expectation takes it.
  ResponseParser withQueued;
  withQueued.expectQueued();
  const std::vector<uint8_t> stream{kProbeRealtimeByte};
  const std::vector<ParsedEvent> queued = withQueued.feed(stream);
  CHECK_EQ(queued.size(), static_cast<size_t>(1));
  if (queued.size() == 1) {
    CHECK_EQ(queued[0].kind, ParsedEventKind::QueuedStatus);
  }

  // With no expectations at all it is simply unclassified.
  ResponseParser bare;
  const std::vector<ParsedEvent> unknown = bare.feed(stream);
  CHECK_EQ(unknown.size(), static_cast<size_t>(1));
  if (unknown.size() == 1) {
    CHECK_EQ(unknown[0].kind, ParsedEventKind::UnknownByte);
    CHECK_EQ(static_cast<unsigned>(unknown[0].byte), 0x16u);
  }
}

PD_TEST(garbage_produces_unknown_bytes_and_the_parser_keeps_working) {
  ResponseParser parser;
  const std::vector<uint8_t> garbage{0xFF, 0x01, 0x99, 0x7F};
  const std::vector<ParsedEvent> events = parser.feed(garbage);
  CHECK_EQ(events.size(), static_cast<size_t>(4));
  for (const ParsedEvent& event : events) {
    CHECK_EQ(event.kind, ParsedEventKind::UnknownByte);
  }

  // A real frame immediately afterwards still parses.
  const std::vector<ParsedEvent> after = parser.feed(kProbeFrame);
  CHECK_EQ(after.size(), static_cast<size_t>(1));
  if (after.size() == 1) {
    CHECK_EQ(after[0].kind, ParsedEventKind::GsHAck);
    CHECK_EQ(after[0].token, std::string("P001"));
  }
}

PD_TEST(false_frame_starts_resynchronise_without_losing_bytes) {
  // 0x37 not followed by 0x22.
  ResponseParser parser;
  std::vector<uint8_t> stream{0x37, 0x99};
  appendAll(stream, kProbeFrame);
  const std::vector<ParsedEvent> events = parser.feed(stream);
  CHECK_EQ(events.size(), static_cast<size_t>(3));
  if (events.size() == 3) {
    CHECK_EQ(events[0].kind, ParsedEventKind::UnknownByte);
    CHECK_EQ(static_cast<unsigned>(events[0].byte), 0x37u);
    CHECK_EQ(events[1].kind, ParsedEventKind::UnknownByte);
    CHECK_EQ(static_cast<unsigned>(events[1].byte), 0x99u);
    CHECK_EQ(events[2].kind, ParsedEventKind::GsHAck);
  }

  // Correct header, wrong terminator: still no lost bytes, and the trailing real
  // frame is still found.
  ResponseParser second;
  std::vector<uint8_t> bad{0x37, 0x22, 0x50, 0x30, 0x30, 0x31, 0x7E};
  appendAll(bad, kProbeFrame);
  const std::vector<ParsedEvent> badEvents = second.feed(bad);
  CHECK_EQ(badEvents.size(), static_cast<size_t>(8));
  if (badEvents.size() == 8) {
    for (size_t i = 0; i < 7; ++i) {
      CHECK_EQ(badEvents[i].kind, ParsedEventKind::UnknownByte);
    }
    CHECK_EQ(badEvents[7].kind, ParsedEventKind::GsHAck);
    CHECK_EQ(badEvents[7].token, std::string("P001"));
  }
}

PD_TEST(incomplete_asb_prefix_is_reclassified_not_lost) {
  // A lone byte carrying the ASB signature is indistinguishable from the first byte
  // of a frame that has not fully arrived. The parser holds it; flush() forces the
  // decision when the read side has gone idle.
  ResponseParser parser;
  parser.expectQueued();
  const std::vector<uint8_t> stream{0x10};
  const std::vector<ParsedEvent> held = parser.feed(stream);
  CHECK_EQ(held.size(), static_cast<size_t>(0));
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(1));

  const std::vector<ParsedEvent> flushed = parser.flush();
  CHECK_EQ(flushed.size(), static_cast<size_t>(1));
  if (flushed.size() == 1) {
    CHECK_EQ(flushed[0].kind, ParsedEventKind::QueuedStatus);
    CHECK_EQ(static_cast<unsigned>(flushed[0].byte), 0x10u);
  }
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(0));

  // Three ASB-looking bytes followed by one that is not: no frame, four events.
  ResponseParser mixed;
  const std::vector<uint8_t> notAFrame{0x10, 0x10, 0x10, 0x01};
  const std::vector<ParsedEvent> events = mixed.feed(notAFrame);
  CHECK_EQ(events.size(), static_cast<size_t>(4));
  for (const ParsedEvent& event : events) {
    CHECK_EQ(event.kind, ParsedEventKind::UnknownByte);
  }
}

PD_TEST(reset_drops_pending_bytes_and_expectations) {
  ResponseParser parser;
  parser.expectRealtime(DleEotKind::ErrorCause);
  parser.expectQueued();
  parser.feed(kProbeFrame.data(), 3);
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(3));

  parser.reset();
  CHECK_EQ(parser.pendingBytes(), static_cast<size_t>(0));
  CHECK_EQ(parser.outstandingRealtime(), static_cast<size_t>(0));
  CHECK_EQ(parser.outstandingQueued(), static_cast<size_t>(0));
}

PD_TEST(interleaving_torture_test) {
  // One stream carrying every frame kind, plus garbage, in an order that exercises
  // the full precedence chain. Fed twice: as one blob and one byte at a time. The
  // event sequence must be identical.
  std::vector<uint8_t> stream;
  appendAll(stream, frameFor("P001"));       // 1 GsHAck
  appendAll(stream, kAsbHealthy);            // 2 AsbStatus
  stream.push_back(kProbeRealtimeByte);      // 3 RealtimeStatus (expectation outstanding)
  stream.push_back(kProbeQueuedByte);        // 4 QueuedStatus
  stream.push_back(0x12);                    // 5 QueuedStatus (realtime pattern, none expected)
  stream.push_back(0xFF);                    // 6 UnknownByte
  appendAll(stream, frameFor("C001"));       // 7 GsHAck
  appendAll(stream, kAsbFaulty);             // 8 AsbStatus
  stream.push_back(0x01);                    // 9 UnknownByte

  const std::vector<ParsedEventKind> expected{
      ParsedEventKind::GsHAck,        ParsedEventKind::AsbStatus,
      ParsedEventKind::RealtimeStatus, ParsedEventKind::QueuedStatus,
      ParsedEventKind::QueuedStatus,  ParsedEventKind::UnknownByte,
      ParsedEventKind::GsHAck,        ParsedEventKind::AsbStatus,
      ParsedEventKind::UnknownByte,
  };

  ResponseParser blob;
  blob.expectRealtime(DleEotKind::ErrorCause);
  blob.expectQueued();
  blob.expectQueued();
  const std::vector<ParsedEvent> blobEvents = blob.feed(stream);
  CHECK(kindsOf(blobEvents) == expected);
  CHECK_EQ(blob.pendingBytes(), static_cast<size_t>(0));

  ResponseParser drip;
  drip.expectRealtime(DleEotKind::ErrorCause);
  drip.expectQueued();
  drip.expectQueued();
  const std::vector<ParsedEvent> dripEvents = feedByteByByte(drip, stream);
  CHECK(kindsOf(dripEvents) == expected);
  CHECK_EQ(drip.pendingBytes(), static_cast<size_t>(0));

  if (blobEvents.size() == expected.size() && dripEvents.size() == expected.size()) {
    CHECK_EQ(blobEvents[0].token, std::string("P001"));
    CHECK_EQ(dripEvents[0].token, std::string("P001"));
    CHECK_EQ(blobEvents[6].token, std::string("C001"));
    CHECK_EQ(dripEvents[6].token, std::string("C001"));
    CHECK_EQ(blobEvents[2].realtime_kind, DleEotKind::ErrorCause);
    CHECK(blobEvents[2].flags.cutter_error.has_value());
    CHECK_EQ(static_cast<unsigned>(blobEvents[3].byte), 0x00u);
    CHECK_EQ(static_cast<unsigned>(blobEvents[4].byte), 0x12u);
    CHECK_EQ(static_cast<unsigned>(blobEvents[5].byte), 0xFFu);
    CHECK_EQ(static_cast<unsigned>(blobEvents[8].byte), 0x01u);
  }
}

PD_TEST(fixed_bit_signatures) {
  CHECK(looksLikeRealtimeByte(0x16));
  CHECK(!looksLikeAsbByte(0x16));
  CHECK(looksLikeAsbByte(0x10));
  CHECK(!looksLikeRealtimeByte(0x10));
  CHECK(!looksLikeAsbByte(0x00));
  CHECK(!looksLikeRealtimeByte(0x00));
  // Bit 7 set disqualifies both families.
  CHECK(!looksLikeAsbByte(0x90));
  CHECK(!looksLikeRealtimeByte(0x92));
  // Bit 4 clear disqualifies both.
  CHECK(!looksLikeAsbByte(0x00));
  CHECK(!looksLikeRealtimeByte(0x02));
}
