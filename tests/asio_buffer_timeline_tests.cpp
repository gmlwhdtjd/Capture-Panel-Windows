#include "asio_buffer_timeline.hpp"
#include "test_framework.hpp"

namespace capture_panel::test {

CP_TEST_CASE("ASIO timeline handles a capture shorter than one driver buffer") {
    asio::AsioBufferTimeline timeline(100, 256);

    const auto primed_output = timeline.next_output_block();
    CP_REQUIRE(primed_output.start_frame == 0);
    CP_REQUIRE(primed_output.frame_count == 100);
    CP_REQUIRE(timeline.next_output_block().frame_count == 0);
    CP_REQUIRE(!timeline.next_input_block().has_value());

    const auto input = timeline.next_input_block();
    CP_REQUIRE(input.has_value());
    CP_REQUIRE(input->start_frame == 0);
    CP_REQUIRE(input->frame_count == 100);
    CP_REQUIRE(timeline.recording_complete());
}

CP_TEST_CASE("ASIO timeline drains exact driver-buffer multiples") {
    asio::AsioBufferTimeline timeline(512, 256);

    CP_REQUIRE(timeline.next_output_block().frame_count == 256);
    CP_REQUIRE(timeline.next_output_block().frame_count == 256);
    CP_REQUIRE(!timeline.next_input_block().has_value());

    const auto first_input = timeline.next_input_block();
    CP_REQUIRE(first_input.has_value());
    CP_REQUIRE(first_input->start_frame == 0);
    CP_REQUIRE(first_input->frame_count == 256);
    CP_REQUIRE(!timeline.recording_complete());

    CP_REQUIRE(timeline.next_output_block().frame_count == 0);
    const auto second_input = timeline.next_input_block();
    CP_REQUIRE(second_input.has_value());
    CP_REQUIRE(second_input->start_frame == 256);
    CP_REQUIRE(second_input->frame_count == 256);
    CP_REQUIRE(timeline.recording_complete());
}

CP_TEST_CASE("ASIO timeline preserves a final partial input and output block") {
    asio::AsioBufferTimeline timeline(522, 256);

    CP_REQUIRE(timeline.next_output_block().frame_count == 256);
    CP_REQUIRE(timeline.next_output_block().frame_count == 256);
    CP_REQUIRE(!timeline.next_input_block().has_value());
    CP_REQUIRE(timeline.next_input_block()->frame_count == 256);

    const auto partial_output = timeline.next_output_block();
    CP_REQUIRE(partial_output.start_frame == 512);
    CP_REQUIRE(partial_output.frame_count == 10);
    CP_REQUIRE(timeline.next_input_block()->frame_count == 256);

    CP_REQUIRE(timeline.next_output_block().frame_count == 0);
    const auto partial_input = timeline.next_input_block();
    CP_REQUIRE(partial_input.has_value());
    CP_REQUIRE(partial_input->start_frame == 512);
    CP_REQUIRE(partial_input->frame_count == 10);
    CP_REQUIRE(timeline.recording_complete());
}

} // namespace capture_panel::test
