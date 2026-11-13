#pragma once

#include "clickhle/common/types.h"
#include <array>
#include <optional>

namespace clickhle::classic {

// The verified 7G descriptor-to-register subset. Format IDs are raw hardware
// values; pixel channel order and blending are not yet established.
struct LayerDescriptor {
    u32 format = 0, source = 0, source_width = 0, source_height = 0;
    u32 width = 0, height = 0, x = 0, y = 0, crop_x = 0, crop_y = 0;
};

class Compositor {
public:
    static constexpr u32 base = 0x38900000;
    bool set_layer(unsigned slot, const LayerDescriptor& layer);
    bool set_enabled(unsigned slot, bool enabled);
    // Source pointer setter at 0x08144078. For slot 5, format and the
    // secondary-plane offset come from the display object.
    bool set_sources(unsigned slot, u8 format, const std::array<u32, 4>& planes,
                     u32 secondary_offset);
    // OSOS 0x08088a20 copies three 256-word arrays to these registers.
    void write_lookup_tables(const std::array<u32, 256>& first,
                             const std::array<u32, 256>& second,
                             const std::array<u32, 256>& third);
    // The display setup routine constructs this particular set of tables.
    void initialize_lookup_tables();
    // Raw bit fields written by OSOS 0x080bf1a0; their hardware meanings are
    // not established. Unrelated control bits are preserved.
    void set_output_control(u32 first, u32 second, const std::array<u8, 3>& fields,
                            u32 last);
    // Register encoding from OSOS 0x081441d8; this does not implement blending.
    bool set_blend_mode(unsigned slot, unsigned mode, unsigned ordering);
    bool set_blend_parameter(unsigned slot, u32 value, unsigned ordering);
    [[nodiscard]] std::optional<u32> read(u32 address) const;
    bool write(u32 address, u32 value);

private:
    // 7G OSOS writes three 256-word tables at +0x400, +0x800 and +0xc00.
    // Retaining their words does not yet model their pixel-processing effect.
    std::array<u32, 0x1000 / 4> registers_{};
};

} // namespace clickhle::classic
