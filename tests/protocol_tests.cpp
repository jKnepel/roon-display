#include "roon_display/moo.hpp"
#include "roon_display/sood.hpp"
#include "roon_display/display_state.hpp"

#include <cstdlib>
#include <iostream>
#include <clocale>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

} // namespace

int main() {
    using namespace roon_display;
    const auto query = encode_sood_query({{"_tid", "test-id"}, {"query_service_id", "service"}});
    const auto decoded = parse_sood(query, "192.0.2.1", 9003);
    check(decoded.has_value(), "SOOD query parses");
    check(decoded->type == 'Q', "SOOD type round trips");
    check(decoded->properties.at("_tid") == "test-id", "SOOD property round trips");

    const std::string body = R"({"hello":"world"})";
    const auto wire = make_moo_request(42, "com.example:1/test", body);
    std::string error;
    const auto message = parse_moo(wire, &error);
    check(message.has_value(), error.c_str());
    check(message->verb == "REQUEST", "MOO verb parses");
    check(message->service == "com.example:1", "MOO service parses");
    check(message->name == "test", "MOO method parses");
    check(message->request_id == "42", "MOO request id parses");
    check(message->body_text() == body, "MOO body round trips");

    const auto response = make_moo_response("CONTINUE", "Subscribed", "7", "{}");
    const auto parsed_response = parse_moo(response, &error);
    check(parsed_response && parsed_response->name == "Subscribed", "MOO response parses");
    check(json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n", "JSON escaping works");
    check(json_string_field(R"({"core_id":"abc-123"})", "core_id") == "abc-123",
          "JSON string field extraction works");
    DisplayStateStore escaped;
    escaped.apply_transport("Subscribed", R"({"zones":[{"zone_id":"z\u0031","display_name":"Caf\u00e9","state":"playing","now_playing":{"seek_position":12.5,"length":240,"two_line":{"line1":"Artiste","line2":"Piste"}}}]})");
    check(escaped.snapshot().zones.contains("z1"), "Unicode JSON escapes parse");
    check(escaped.snapshot().zones.at("z1").display_name == "Café", "Unicode JSON escapes decode");
    check(escaped.snapshot().zones.at("z1").now_playing->seek_position == 12.5, "JSON decimal number parses");
    DisplayStateStore store;
    store.apply_transport("Subscribed", R"({"zones":[{"zone_id":"z1","display_name":"Living Room","state":"playing","now_playing":{"seek_position":12.5,"length":240,"image_key":"art","two_line":{"line1":"Artist","line2":"Track"}}}]})");
    check(store.snapshot().zones.at("z1").now_playing.has_value(), "zone now-playing parses");
    check(store.snapshot().zones.at("z1").now_playing->two_line.line2 == "Track", "now-playing text parses");
    store.set_active_zone("z1");
    check(store.snapshot().active_zone_id == "z1", "active zone updates");
    check(to_json(store.snapshot()).find("artworkUrl") != std::string::npos, "state JSON includes artwork URL");
    const char* saved_locale = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved_numeric_locale = saved_locale ? saved_locale : "C";
    if (std::setlocale(LC_NUMERIC, "de_DE.utf8")) {
        check(to_json(store.snapshot()).find("seekPosition\":12.5") != std::string::npos, "JSON output ignores process locale");
        std::setlocale(LC_NUMERIC, saved_numeric_locale.c_str());
    }
    check(to_json(store.snapshot()).find("seekPosition\":12.5") != std::string::npos, "JSON numbers use dot decimal format");
    std::cout << "All protocol tests passed\n";
}

