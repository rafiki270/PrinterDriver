#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_printer.hpp"
#include "printerdriver/agent/agent.hpp"
#include "printerdriver/dsl/json.hpp"
#include "test_harness.hpp"

// The agent's suite drives the real HTTP server over a real loopback socket against the
// same scripted device the engine tests use. Two things are being proved: that the
// evidence document says what techspec §5 says it must, and that the fleet journal is
// one journal — the same key from two HTTP clients prints once.

using pd::agent::Agent;
using pd::agent::AgentConfig;
using pd::agent::HttpClientResult;
using pd::agent::PrinterSpec;
using pd::dsl::Json;

namespace {

// One agent on an ephemeral loopback port, with every printer backed by a scripted
// device instead of a socket.
struct Fixture {
  std::shared_ptr<pdfake::FakePrinter> device = std::make_shared<pdfake::FakePrinter>();
  std::shared_ptr<pdfake::MockTransport::Stats> stats =
      std::make_shared<pdfake::MockTransport::Stats>();
  std::unique_ptr<Agent> agent;
  std::string error;
  // M14. Applied to the scripted profile before the printer is attached, so a drawer
  // test can pick a port classification without a second fixture.
  std::function<void(pd::CapabilityProfile*)> adjust_profile;

  bool start(pd::CompletionMechanism mechanism = pd::CompletionMechanism::GsParenH,
             const std::string& store = {}) {
    AgentConfig config;
    config.store = store;
    config.bind = "127.0.0.1";
    config.port = 0;  // ephemeral
    config.workers = 3;
    config.wait_ms = 4000;

    PrinterSpec kitchen;
    kitchen.id = "kitchen";
    kitchen.host = "192.0.2.10";  // never dialled: the resolver replaces the transport
    kitchen.width_dots = 576;
    config.printers.push_back(kitchen);

    agent = std::make_unique<Agent>(std::move(config));
    auto device_copy = device;
    auto stats_copy = stats;
    auto adjust = adjust_profile;
    agent->setPrinterConfigHook(
        [device_copy, stats_copy, mechanism, adjust](const PrinterSpec&,
                                                     pd::PrinterConfig* printer) {
          // The database profiles carry production timeouts; the scripted device
          // answers from inside write(), so the suite uses budgets it can wait out.
          printer->profile = pdfake::fastProfile(mechanism);
          if (adjust) {
            adjust(&printer->profile);
          }
          printer->transport = [device_copy, stats_copy]() {
            return std::unique_ptr<pd::Transport>(new pdfake::MockTransport(
                device_copy, pdfake::MockBehaviour{}, stats_copy));
          };
        });
    return agent->start(&error);
  }

  ~Fixture() {
    if (agent) {
      agent->stop();
    }
  }

  uint16_t port() const { return agent->port(); }

  HttpClientResult get(const std::string& target) const {
    return pd::agent::httpRequest("127.0.0.1", port(), "GET", target);
  }
  HttpClientResult post(const std::string& target, const std::string& body) const {
    return pd::agent::httpRequest("127.0.0.1", port(), "POST", target, body);
  }
};

// A verification token is base 94 over printable ASCII, so it reaches a URL path only
// percent-encoded — exactly what a client library would do with it.
std::string urlEncode(const std::string& text) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(digits[(c >> 4) & 0x0F]);
      out.push_back(digits[c & 0x0F]);
    }
  }
  return out;
}

std::string base64(const std::vector<uint8_t>& data) {
  static const char* alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = i + 1 < data.size() ? data[i + 1] : 0;
    const uint32_t c = i + 2 < data.size() ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(alphabet[(triple >> 18) & 0x3F]);
    out.push_back(alphabet[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < data.size() ? alphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < data.size() ? alphabet[triple & 0x3F] : '=');
  }
  return out;
}

Json parse(const std::string& text) {
  Json json;
  std::string error;
  if (!pd::dsl::tryParseJson(text, &json, &error)) {
    return Json::object({});
  }
  return json;
}

std::string field(const Json& json, const char* key) {
  const Json* value = json.find(key);
  return value != nullptr && value->isString() ? value->asString() : std::string();
}

bool flag(const Json& json, const char* key) {
  const Json* value = json.find(key);
  return value != nullptr && value->truthy();
}

std::string jobBody(const std::string& key, const std::string& text = "HELLO") {
  Json payload = Json::object({});
  payload.set("text", Json::string(text));
  Json body = Json::object({});
  body.set("printerId", Json::string("kitchen"));
  body.set("key", Json::string(key));
  body.set("payload", std::move(payload));
  return pd::dsl::toJson(body);
}

}  // namespace

PD_TEST(healthz_reports_the_fleet_journal_and_the_instance_nonce) {
  Fixture fixture;
  CHECK(fixture.start());
  const HttpClientResult response = fixture.get("/healthz");
  CHECK(response.ok);
  CHECK_EQ(response.status, 200);

  const Json body = parse(response.body);
  CHECK(flag(body, "ok"));
  CHECK_EQ(body.find("printers")->asInt(), 1LL);
  // docs/api.md §14: the nonce names this owner on the paper itself.
  CHECK_EQ(field(body, "instanceNonce").size(), static_cast<size_t>(2));
  CHECK(!flag(body, "foreignWriterDetected"));
}

PD_TEST(a_submitted_job_comes_back_as_an_evidence_document) {
  Fixture fixture;
  CHECK(fixture.start(pd::CompletionMechanism::GsParenH));

  const HttpClientResult response = fixture.post("/jobs", jobBody("order-7F3A#kitchen"));
  CHECK(response.ok);
  CHECK_EQ(response.status, 201);  // terminal before the wait budget expired

  const Json body = parse(response.body);
  CHECK_EQ(field(body, "state"), std::string("DoneSoftware"));
  CHECK_EQ(field(body, "outcome"), std::string("Done"));
  CHECK_EQ(field(body, "key"), std::string("order-7F3A#kitchen"));
  CHECK_EQ(field(body, "printerId"), std::string("kitchen"));
  CHECK(!field(body, "job").empty());

  // techspec §5: the evidence, not a bare {success:true}.
  const Json* evidence = body.find("evidence");
  CHECK(evidence != nullptr && evidence->isObject());
  if (evidence != nullptr) {
    CHECK(flag(*evidence, "transportAccepted"));
    CHECK(flag(*evidence, "fenceResponse"));
    CHECK_EQ(field(*evidence, "printFence"), std::string("GS(H) fn48"));
    CHECK_EQ(field(*evidence, "cutterStatus"), std::string("clear"));
    CHECK_EQ(field(*evidence, "paperStatus"), std::string("ok"));
  }
  // docs/compatibility-brief.md §24: grade A is an explicit device completion.
  CHECK_EQ(field(body, "grade"), std::string("A"));
  CHECK_EQ(field(body, "authority"), std::string("PhysicalPrinter"));
  CHECK_EQ(field(body, "method"), std::string("GS(H) fn48"));
  // The GS ( H token that went on the wire, and onto the paper as `V:`.
  CHECK_EQ(field(body, "token").size(), static_cast<size_t>(4));

  CHECK_EQ(fixture.device->cuts(), static_cast<size_t>(1));
  CHECK(fixture.device->receivedContains("HELLO"));
}

PD_TEST(a_printer_without_a_fence_is_graded_honestly_not_upgraded) {
  Fixture fixture;
  CHECK(fixture.start(pd::CompletionMechanism::None));
  const HttpClientResult response = fixture.post("/jobs", jobBody("order-none"));
  CHECK(response.ok);

  const Json body = parse(response.body);
  const Json* evidence = body.find("evidence");
  CHECK(evidence != nullptr);
  if (evidence != nullptr) {
    CHECK(flag(*evidence, "transportAccepted"));
    // No ordered fence exists on this profile, so none came back and the agent says so.
    CHECK(!flag(*evidence, "fenceResponse"));
  }
  CHECK_EQ(field(body, "grade"), std::string("E"));
  CHECK_EQ(field(body, "authority"), std::string("TransportOnly"));
}

PD_TEST(the_same_key_from_two_http_clients_prints_once) {
  Fixture fixture;
  CHECK(fixture.start());

  const HttpClientResult first = fixture.post("/jobs", jobBody("order-9001#kitchen"));
  CHECK_EQ(first.status, 201);
  const Json first_body = parse(first.body);
  CHECK(!flag(first_body, "deduped"));

  const size_t printed_once = fixture.device->printDataBytes();
  const size_t cuts_once = fixture.device->cuts();

  // A second till, a second socket, the same order key. docs/sdk-spec.md §6: the
  // existing job comes back and nothing prints again.
  const HttpClientResult second = fixture.post("/jobs", jobBody("order-9001#kitchen"));
  CHECK_EQ(second.status, 200);
  const Json second_body = parse(second.body);
  CHECK(flag(second_body, "deduped"));
  CHECK_EQ(field(second_body, "job"), field(first_body, "job"));
  CHECK_EQ(field(second_body, "token"), field(first_body, "token"));

  CHECK_EQ(fixture.device->printDataBytes(), printed_once);
  CHECK_EQ(fixture.device->cuts(), cuts_once);
  CHECK_EQ(fixture.agent->driver().store().size(), static_cast<size_t>(1));
}

PD_TEST(a_job_resolves_by_id_by_key_and_by_verification_token) {
  Fixture fixture;
  CHECK(fixture.start());
  const Json submitted = parse(fixture.post("/jobs", jobBody("order-4242")).body);
  const std::string id = field(submitted, "job");
  const std::string token = field(submitted, "token");
  CHECK(!id.empty());
  CHECK(!token.empty());

  const HttpClientResult by_key = fixture.get("/jobs/order-4242");
  CHECK_EQ(by_key.status, 200);
  CHECK_EQ(field(parse(by_key.body), "job"), id);

  const HttpClientResult by_id = fixture.get("/jobs/" + id);
  CHECK_EQ(by_id.status, 200);
  CHECK_EQ(field(parse(by_id.body), "key"), std::string("order-4242"));

  // docs/api.md §14, paper → job: an operator holding the receipt has the V: code, and
  // that code is base 94 — it can contain '/', '#' or '%', so it reaches the path
  // percent-encoded and must survive the round trip whole.
  const HttpClientResult by_token = fixture.get("/jobs/" + urlEncode(token));
  CHECK_EQ(by_token.status, 200);
  CHECK_EQ(field(parse(by_token.body), "job"), id);

  const HttpClientResult missing = fixture.get("/jobs/never-printed");
  CHECK_EQ(missing.status, 404);
  CHECK_EQ(field(parse(missing.body), "error"), std::string("unknown job"));
}

PD_TEST(a_key_containing_url_punctuation_survives_the_path) {
  Fixture fixture;
  CHECK(fixture.start());
  // Keys are caller-chosen text: "order-7F3A#kitchen-1" is the shape docs/api.md uses.
  fixture.post("/jobs", jobBody("order-7F3A#kitchen-1"));
  const HttpClientResult found = fixture.get("/jobs/order-7F3A%23kitchen-1");
  CHECK_EQ(found.status, 200);
  CHECK_EQ(field(parse(found.body), "key"), std::string("order-7F3A#kitchen-1"));
}

PD_TEST(an_unknown_printer_is_404_and_nothing_is_printed) {
  Fixture fixture;
  CHECK(fixture.start());
  const std::string body =
      R"({"printerId":"bar","key":"order-1","payload":{"text":"x"}})";
  const HttpClientResult response = fixture.post("/jobs", body);
  CHECK_EQ(response.status, 404);
  CHECK_EQ(field(parse(response.body), "error"), std::string("unknown printer"));
  CHECK_EQ(fixture.device->printDataBytes(), static_cast<size_t>(0));
  CHECK_EQ(fixture.agent->driver().store().size(), static_cast<size_t>(0));
}

PD_TEST(malformed_and_incomplete_requests_are_400_not_500) {
  Fixture fixture;
  CHECK(fixture.start());

  const HttpClientResult broken = fixture.post("/jobs", "{\"printerId\":");
  CHECK_EQ(broken.status, 400);
  CHECK_EQ(field(parse(broken.body), "error"), std::string("malformed JSON"));

  const HttpClientResult not_object = fixture.post("/jobs", "[1,2,3]");
  CHECK_EQ(not_object.status, 400);

  const HttpClientResult no_printer = fixture.post("/jobs", R"({"payload":{"text":"x"}})");
  CHECK_EQ(no_printer.status, 400);

  const HttpClientResult no_payload = fixture.post("/jobs", R"({"printerId":"kitchen"})");
  CHECK_EQ(no_payload.status, 400);

  const HttpClientResult empty_payload =
      fixture.post("/jobs", R"({"printerId":"kitchen","payload":{}})");
  CHECK_EQ(empty_payload.status, 400);

  const HttpClientResult bad_cut = fixture.post(
      "/jobs", R"({"printerId":"kitchen","payload":{"text":"x"},"options":{"cut":"maybe"}})");
  CHECK_EQ(bad_cut.status, 400);

  CHECK_EQ(fixture.device->printDataBytes(), static_cast<size_t>(0));
}

PD_TEST(unknown_routes_and_methods_are_declared_not_guessed) {
  Fixture fixture;
  CHECK(fixture.start());
  CHECK_EQ(fixture.get("/").status, 404);
  CHECK_EQ(fixture.get("/nope").status, 404);
  CHECK_EQ(fixture.get("/jobs").status, 405);
  CHECK_EQ(fixture.post("/healthz", "{}").status, 405);
  CHECK_EQ(fixture.post("/jobs/abc", "{}").status, 405);
  // M14. The drawer route exists on POST and on nothing else.
  CHECK_EQ(fixture.get("/printers/kitchen/drawer").status, 405);
  CHECK_EQ(fixture.post("/printers/kitchen/nonsense", "{}").status, 404);
  CHECK_EQ(fixture.post("/printers/nosuch/drawer", "{}").status, 404);
}

PD_TEST(printers_are_listed_and_added_at_runtime_under_one_owner) {
  Fixture fixture;
  CHECK(fixture.start());

  const HttpClientResult listed = fixture.get("/printers");
  CHECK_EQ(listed.status, 200);
  const Json list = parse(listed.body);
  const Json* printers = list.find("printers");
  CHECK(printers != nullptr && printers->isArray());
  CHECK_EQ(printers->size(), static_cast<size_t>(1));
  if (printers != nullptr && printers->size() == 1) {
    const Json& entry = printers->asArray()[0];
    CHECK_EQ(field(entry, "id"), std::string("kitchen"));
    CHECK_EQ(entry.find("widthDots")->asInt(), 576LL);
  }

  const std::string bar =
      R"({"id":"bar","tcp":{"host":"192.0.2.20","port":9100},"widthDots":384,)"
      R"("profile":"xprinter_pos58"})";
  CHECK_EQ(fixture.post("/printers", bar).status, 201);
  CHECK_EQ(parse(fixture.get("/printers").body).find("printers")->size(),
           static_cast<size_t>(2));

  // The single-owner invariant, enforced inside the process too: one id, one lane.
  CHECK_EQ(fixture.post("/printers", bar).status, 409);
  const std::string same_socket =
      R"({"id":"bar2","tcp":{"host":"192.0.2.20","port":9100}})";
  CHECK_EQ(fixture.post("/printers", same_socket).status, 409);

  const std::string bad_profile =
      R"({"id":"x","tcp":{"host":"192.0.2.30"},"profile":"not-a-printer"})";
  CHECK_EQ(fixture.post("/printers", bad_profile).status, 400);
  const std::string no_host = R"({"id":"y","tcp":{}})";
  CHECK_EQ(fixture.post("/printers", no_host).status, 400);
}

PD_TEST(printer_status_is_reported_as_three_states_never_two) {
  Fixture fixture;
  CHECK(fixture.start());
  const HttpClientResult response = fixture.get("/printers/kitchen/status");
  CHECK_EQ(response.status, 200);
  const Json body = parse(response.body);
  const Json* status = body.find("status");
  CHECK(status != nullptr && status->isObject());
  if (status != nullptr) {
    // Nothing has been asked of the device yet, so every flag is null rather than
    // false: a capability nobody established is not the same as one that is absent.
    CHECK(!flag(*status, "observed"));
    CHECK(status->find("paperOut") != nullptr);
    CHECK(status->find("paperOut")->isNull());
  }

  // ?refresh=1 queues a DLE EOT round trip behind any active job.
  const HttpClientResult refreshed = fixture.get("/printers/kitchen/status?refresh=1");
  CHECK_EQ(refreshed.status, 200);
  const Json refreshed_body = parse(refreshed.body);
  const Json* live = refreshed_body.find("status");
  CHECK(live != nullptr);
  if (live != nullptr) {
    CHECK(flag(*live, "observed"));
    CHECK(live->find("paperOut")->isBool());
  }

  CHECK_EQ(fixture.get("/printers/nope/status").status, 404);
  CHECK_EQ(fixture.get("/printers/nope").status, 404);
}

PD_TEST(a_dsl_template_is_bound_rendered_and_reported) {
  Fixture fixture;
  CHECK(fixture.start());

  const std::string body =
      R"({"printerId":"kitchen","key":"order-dsl","payload":{)"
      R"("dslTemplate":{"v":1,"template":true,"meta":{"cut":"full"},)"
      R"("blocks":[{"text":"{{venue}}"},{"barcode":"4006381333931","symbology":"ean13"},)"
      R"({"text":"{{missing.path}}"}]},)"
      R"("model":{"venue":"KITCHEN 1"}}})";
  const HttpClientResult response = fixture.post("/jobs", body);
  CHECK_EQ(response.status, 201);
  const Json evidence = parse(response.body);
  CHECK_EQ(field(evidence, "state"), std::string("DoneSoftware"));

  // The render report travels with the job: a missing model path is declared, never
  // silent (docs/receipt-dsl.md degradation rules).
  const Json* report = evidence.find("render");
  CHECK(report != nullptr && report->isArray());
  bool declared_missing = false;
  if (report != nullptr) {
    for (const Json& entry : report->asArray()) {
      declared_missing = declared_missing || field(entry, "kind") == "missingPath";
    }
  }
  CHECK(declared_missing);

  CHECK(fixture.device->receivedContains("KITCHEN 1"));
  // The EAN-13 reached the wire as GS k 67.
  CHECK(fixture.device->receivedContains("4006381333931"));
  CHECK_EQ(fixture.device->cuts(), static_cast<size_t>(1));
}

PD_TEST(a_raster_payload_is_decoded_and_its_dimensions_are_checked) {
  Fixture fixture;
  CHECK(fixture.start());

  // A 576 x 4 all-black band: the width the printer already prints, so nothing is
  // scaled and the assertion is about the decode, not about the raster pipeline.
  const std::vector<uint8_t> gray(576 * 4, 0x00);
  Json payload = Json::object({});
  payload.set("rasterBase64", Json::string(base64(gray)));
  payload.set("width", Json::number(576));
  payload.set("height", Json::number(4));
  Json body = Json::object({});
  body.set("printerId", Json::string("kitchen"));
  body.set("key", Json::string("order-raster"));
  body.set("payload", std::move(payload));
  CHECK_EQ(fixture.post("/jobs", pd::dsl::toJson(body)).status, 201);
  CHECK_EQ(fixture.device->rasterBlocks(), static_cast<size_t>(1));

  const std::string wrong_size =
      R"({"printerId":"kitchen","key":"order-raster-2","payload":)"
      R"({"rasterBase64":"AAAA","width":8,"height":2}})";
  const HttpClientResult refused = fixture.post("/jobs", wrong_size);
  CHECK_EQ(refused.status, 400);

  const std::string no_dimensions =
      R"({"printerId":"kitchen","key":"order-raster-3","payload":)"
      R"({"rasterBase64":"AAAA"}})";
  CHECK_EQ(fixture.post("/jobs", no_dimensions).status, 400);

  const std::string not_base64 =
      R"({"printerId":"kitchen","key":"order-raster-4","payload":)"
      R"({"rasterBase64":"not base64!!","width":8,"height":2}})";
  CHECK_EQ(fixture.post("/jobs", not_base64).status, 400);
}

PD_TEST(a_preflight_refusal_is_failed_known_and_nothing_is_printed) {
  Fixture fixture;
  pdfake::Script script;
  script.offline_cause = pdfake::paperOutByte();
  script.paper_sensor = pdfake::paperOutByte();
  fixture.device->setScript(script);
  CHECK(fixture.start());

  const HttpClientResult response = fixture.post("/jobs", jobBody("order-paper-out"));
  CHECK(response.ok);
  const Json body = parse(response.body);
  CHECK_EQ(field(body, "state"), std::string("FailedKnown"));
  CHECK_EQ(field(body, "outcome"), std::string("Failed"));
  CHECK_EQ(field(body, "reason"), std::string("PreflightPaperOut"));
  const Json* evidence = body.find("evidence");
  CHECK(evidence != nullptr);
  if (evidence != nullptr) {
    // docs/api.md §4: FailedKnown means no byte of the receipt left the host, which is
    // exactly what makes it safe to retry.
    CHECK(!flag(*evidence, "transportAccepted"));
    CHECK_EQ(field(*evidence, "paperStatus"), std::string("out"));
  }
  CHECK_EQ(fixture.device->cuts(), static_cast<size_t>(0));
}

PD_TEST(the_journal_survives_a_restart_and_still_answers_by_key) {
  pdfake::TempDir directory("agent");
  const std::string id = [&] {
    Fixture first;
    CHECK(first.start(pd::CompletionMechanism::GsParenH, directory.path()));
    const Json body = parse(first.post("/jobs", jobBody("order-restart")).body);
    CHECK_EQ(field(body, "state"), std::string("DoneSoftware"));
    return field(body, "job");
  }();
  CHECK(!id.empty());

  Fixture second;
  CHECK(second.start(pd::CompletionMechanism::GsParenH, directory.path()));
  const HttpClientResult found = second.get("/jobs/order-restart");
  CHECK_EQ(found.status, 200);
  const Json body = parse(found.body);
  CHECK_EQ(field(body, "job"), id);
  CHECK_EQ(field(body, "state"), std::string("DoneSoftware"));

  // And the key still dedupes across the restart: this is the whole point of the
  // journal (docs/techspec.md §7.1).
  const HttpClientResult again = second.post("/jobs", jobBody("order-restart"));
  CHECK_EQ(again.status, 200);
  CHECK(flag(parse(again.body), "deduped"));
  CHECK_EQ(second.device->printDataBytes(), static_cast<size_t>(0));
}

PD_TEST(config_json_is_parsed_and_bad_config_is_refused) {
  AgentConfig config;
  std::string error;
  const char* source =
      R"({"bind":"0.0.0.0","port":9123,"store":"/tmp/pd-agent","workers":8,)"
      R"("printers":[{"id":"kitchen","tcp":{"host":"192.168.1.101","port":9100},)"
      R"("widthDots":576,"profile":"xprinter_s_series"}]})";
  CHECK(pd::agent::parseAgentConfig(pd::dsl::parseJson(source), &config, &error));
  CHECK_EQ(config.bind, std::string("0.0.0.0"));
  CHECK_EQ(config.port, static_cast<uint16_t>(9123));
  CHECK_EQ(config.store, std::string("/tmp/pd-agent"));
  CHECK_EQ(config.printers.size(), static_cast<size_t>(1));
  if (!config.printers.empty()) {
    CHECK_EQ(config.printers[0].id, std::string("kitchen"));
    CHECK_EQ(config.printers[0].endpoint(), std::string("tcp://192.168.1.101:9100"));
    CHECK_EQ(config.printers[0].width_dots, 576u);
  }

  AgentConfig rejected;
  CHECK(!pd::agent::parseAgentConfig(
      pd::dsl::parseJson(R"({"printers":[{"id":"a"}]})"), &rejected, &error));
  CHECK(error.find("tcp") != std::string::npos);
  CHECK(!pd::agent::parseAgentConfig(
      pd::dsl::parseJson(R"({"printers":[{"tcp":{"host":"1.2.3.4"},"profile":"nope"}]})"),
      &rejected, &error));
}

PD_TEST(percent_decoding_and_query_parsing_do_what_the_routes_assume) {
  CHECK_EQ(pd::agent::percentDecode("order-7F3A%23kitchen"),
           std::string("order-7F3A#kitchen"));
  CHECK_EQ(pd::agent::percentDecode("a%2Fb"), std::string("a/b"));
  CHECK_EQ(pd::agent::percentDecode("a%zz"), std::string("a%zz"));
  // '+' is a plus in a path and a space in a form-encoded query value. A verification
  // token can contain '+', so getting this wrong loses the paper-to-job lookup.
  CHECK_EQ(pd::agent::percentDecode("a+b"), std::string("a+b"));
  CHECK_EQ(pd::agent::formDecode("a+b"), std::string("a b"));

  pd::agent::HttpRequest request;
  request.query = "refresh=1&mode=full";
  bool found = false;
  CHECK_EQ(request.queryValue("refresh", &found), std::string("1"));
  CHECK(found);
  request.queryValue("absent", &found);
  CHECK(!found);
}

// --- M14: the cash drawer (docs/cash-drawer.md) ---------------------------------------

PD_TEST(the_drawer_endpoint_reports_a_state_and_never_a_boolean) {
  Fixture fixture;
  fixture.adjust_profile = [](pd::CapabilityProfile* profile) {
    *profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    profile->drawer.status.polarity.calibrated = true;
    profile->drawer.status.polarity.high_means_open = true;
  };
  CHECK(fixture.start());

  const HttpClientResult response = fixture.post("/printers/kitchen/drawer", "{}");
  CHECK(response.ok);
  CHECK_EQ(response.status, 200);

  const Json body = parse(response.body);
  // The document's own spelling of the states, not a success flag.
  CHECK_EQ(field(body, "state"), std::string("OPEN_VERIFIED"));
  CHECK_EQ(field(body, "previousState"), std::string("CLOSED"));
  CHECK_EQ(field(body, "printerId"), std::string("kitchen"));
  CHECK(flag(body, "verified"));
  CHECK(!flag(body, "needsCalibration"));
  CHECK_EQ(body.find("channel")->asInt(), 1LL);
  CHECK_EQ(body.find("pulseMs")->asInt(), 200LL);
  CHECK(body.find("elapsedMs") != nullptr);
  CHECK_EQ(fixture.device->drawerKicks(), size_t{1});
  CHECK(fixture.device->drawerOpen());

  // The port and the commands are reported as two separate evidence columns.
  const Json* drawer = body.find("drawer");
  CHECK(drawer != nullptr && drawer->isObject());
  if (drawer != nullptr) {
    CHECK(flag(*drawer, "kickable"));
    const Json* electrical = drawer->find("electrical");
    CHECK(electrical != nullptr);
    if (electrical != nullptr) {
      CHECK_EQ(field(*electrical, "standard"), std::string("Epson24V6P6C"));
      CHECK_EQ(electrical->find("voltage")->asInt(), 24LL);
      CHECK_EQ(electrical->find("sensorPin")->asInt(), 3LL);
    }
    const Json* evidence = drawer->find("evidence");
    CHECK(evidence != nullptr);
    if (evidence != nullptr) {
      CHECK_EQ(field(*evidence, "electrical"), std::string("Documented"));
      CHECK_EQ(field(*evidence, "commands"), std::string("Documented"));
    }
  }

  // A body of its own: an explicit channel and pulse are honoured, and a drawer that
  // is already open is not pulsed a second time.
  const HttpClientResult again =
      fixture.post("/printers/kitchen/drawer", R"({"channel":1,"pulseMs":120})");
  CHECK_EQ(again.status, 200);
  const Json second = parse(again.body);
  CHECK_EQ(field(second, "state"), std::string("OPEN"));
  CHECK_EQ(second.find("pulseMs")->asInt(), 0LL);
  CHECK_EQ(fixture.device->drawerKicks(), size_t{1});
}

PD_TEST(the_drawer_endpoint_refuses_an_unclassified_port_without_writing_a_byte) {
  Fixture fixture;
  fixture.adjust_profile = [](pd::CapabilityProfile* profile) {
    *profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    // Everything else is healthy; nobody has classified the socket.
    profile->drawer.electrical.standard = pd::DrawerPortStandard::Unknown;
  };
  CHECK(fixture.start());

  const HttpClientResult response = fixture.post("/printers/kitchen/drawer", "{}");
  // Nothing is wrong with the request: the hardware, or what is known about it, is
  // what forbids this.
  CHECK_EQ(response.status, 409);
  const Json body = parse(response.body);
  CHECK(field(body, "error").find("electrical standard is unknown") != std::string::npos);
  CHECK_EQ(fixture.device->drawerKicks(), size_t{0});
  const Json* drawer = body.find("drawer");
  CHECK(drawer != nullptr);
  if (drawer != nullptr) {
    CHECK(!flag(*drawer, "kickable"));
  }
}

PD_TEST(the_drawer_endpoint_says_unverified_rather_than_claiming_an_open) {
  Fixture fixture;
  fixture.adjust_profile = [](pd::CapabilityProfile* profile) {
    *profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
    // A port with no switch input: the pulse is real and the confirmation does not
    // exist. 202, because nothing here settled.
    profile->drawer.status.available = false;
    profile->drawer.status.method = pd::DrawerStatusMethod::None;
  };
  CHECK(fixture.start());

  const HttpClientResult response = fixture.post("/printers/kitchen/drawer", "{}");
  CHECK_EQ(response.status, 202);
  const Json body = parse(response.body);
  CHECK_EQ(field(body, "state"), std::string("KICK_SENT_UNVERIFIED"));
  CHECK_EQ(field(body, "previousState"), std::string("NO_SENSOR"));
  CHECK(!flag(body, "verified"));
  CHECK(flag(body, "needsCalibration"));
  CHECK_EQ(fixture.device->drawerKicks(), size_t{1});
}

PD_TEST(the_drawer_endpoint_refuses_a_malformed_channel_or_pulse) {
  Fixture fixture;
  fixture.adjust_profile = [](pd::CapabilityProfile* profile) {
    *profile = pdfake::drawerProfile(pd::CompletionMechanism::GsParenH);
  };
  CHECK(fixture.start());
  CHECK_EQ(fixture.post("/printers/kitchen/drawer", "{").status, 400);
  CHECK_EQ(fixture.post("/printers/kitchen/drawer", "[]").status, 400);
  CHECK_EQ(fixture.post("/printers/kitchen/drawer", R"({"channel":0})").status, 400);
  CHECK_EQ(fixture.post("/printers/kitchen/drawer", R"({"pulseMs":0})").status, 400);
  CHECK_EQ(fixture.post("/printers/kitchen/drawer", R"({"pulseMs":90000})").status, 400);
  CHECK_EQ(fixture.device->drawerKicks(), size_t{0});
}
