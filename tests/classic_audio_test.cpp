#include "clickhle/audio/classic_audio.h"

int main() {
    clickhle::classic::AudioService audio;
    if (audio.current_volume() != 127 || audio.volume_limit() != 255) return 1;
    audio.enqueue_stream_reset();
    if (audio.pending_count() != 1) return 1;
    audio.service();
    audio.enqueue_control(0x12345678);
    audio.enqueue_control(0x100);
    if (audio.control() != 0 || audio.pending_count() != 2) return 1;
    audio.service();
    if (audio.control() != 0 || audio.pending_count() != 0) return 1;
    audio.enqueue_control(0x12345678);
    audio.service();
    if (audio.control() != 0x78 || audio.pending_count() != 0) return 1;
    clickhle::GuestHeap heap;
    heap.initialise(0x40000000, 0x1000);
    const auto name_copy = heap.allocate(32);
    if (!name_copy) return 1;
    audio.enqueue_track("music.m4a", 34000, name_copy);
    if (audio.stream_value() != 1 || audio.pending_count() != 1 ||
        !audio.tracks().empty() || !heap.size_of(name_copy)) return 1;
    audio.service(&heap);
    if (audio.control() != 1 || audio.pending_count() != 0 ||
        audio.tracks().size() != 1 || audio.tracks()[0].path != "music.m4a" ||
        audio.tracks()[0].parameter != 34000 || heap.size_of(name_copy)) return 1;
    audio.enqueue_track({}, 99, 0);
    audio.service(&heap);
    if (audio.tracks().size() != 1) return 1;
    audio.enqueue_track_selection(0);
    audio.service(&heap);
    const auto selected = audio.take_pending_playback();
    if (!selected || selected->path != "music.m4a" || audio.device_state() != 1) return 1;
    audio.enqueue_track_selection(0);
    audio.service(&heap);
    if (audio.take_pending_playback()) return 1;
    audio.enqueue_track_selection(8);
    audio.service(&heap);
    if (audio.take_pending_playback()) return 1;
    if (audio.create_effect() != 0 || audio.create_effect() != 1) return 1;
    if (audio.set_effect_buffer(0, 0) || audio.set_effect_buffer(2, 0x1000) ||
        !audio.set_effect_buffer(0, 0x1000)) return 1;
    if (audio.set_effect_field_08(2, 4) || !audio.set_effect_field_08(0, 0)) return 1;
    if (!audio.set_effect_field(0, 0x3e, 0x123, true) ||
        audio.set_effect_field(0, 0x50, 1, false)) return 1;
    if (audio.effect_field(0, 0x3e, true) != 0x23 ||
        audio.effect_field(0, 0x10, false) != 44100 ||
        audio.effect_field(3, 0x10, false)) return 1;
}
