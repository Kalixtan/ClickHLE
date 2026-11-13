#include "clickhle/core/emulator.h"
#include "clickhle/common/log.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"
#include "clickhle/video/classic_pixel.h"
#include "clickhle/video/classic_surface.h"
#include <bit>

namespace clickhle {

void register_classic_graphics(HleRegistry& registry) {
    registry.add("OpenGLES", 4, "classic_bind_texture", 2, [](HleCallFrame& frame) {
        const u32 target = frame.args.next_u32();
        const u32 name = frame.args.next_u32();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        if (target != 0x0de1) { context->invalid_enum(); return; }
        if (!context->bind_texture(name))
            frame.emulator.stop_with_error("7G texture binding exceeds modeled name pool");
    });
    registry.add("OpenGLES", 0, "classic_active_texture", 1, [](HleCallFrame& frame) {
        const u32 unit = frame.args.next_u32() - 0x84c0u;
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        // 0x08244c4c sets GL_INVALID_ENUM for an invalid unit but still
        // stores the computed value. Texture-matrix selection is not modeled;
        // its entry points remain unsupported until the matrix stack exists.
        if (unit >= 2) context->invalid_enum();
        context->set_active_texture(unit);
    });
    registry.add("OpenGLES", 14, "classic_client_active_texture", 1, [](HleCallFrame& frame) {
        const u32 unit = frame.args.next_u32() - 0x84c0u;
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        // 0x08248258 stores the unit even after reporting an invalid enum.
        if (unit >= 2) context->invalid_enum();
        context->set_client_texture(unit);
    });
    for (u32 ordinal : {38u, 39u}) {
        registry.add("OpenGLES", ordinal, ordinal == 38 ? "classic_finish" : "classic_flush",
                     0, [](HleCallFrame& frame) {
            // 7G's 0x08249088 and 0x08248f0c both return immediately.
            // Submission/wait behavior must be revisited when asynchronous
            // rasterization exists; these exports have void API results.
            frame.ret_void();
        });
    }
    registry.add("OpenGLES", 5, "classic_blend_factors", 2, [](HleCallFrame& frame) {
        const u32 source = frame.args.next_u32();
        const u32 destination = frame.args.next_u32();
        if (auto* context = frame.emulator.classic_graphics())
            context->set_blend_factors(source, destination);
    });
    registry.add("OpenGLES", 2, "classic_alpha_function_fixed", 2, [](HleCallFrame& frame) {
        const u32 function = frame.args.next_u32();
        const u32 reference = frame.args.next_u32();
        if (auto* context = frame.emulator.classic_graphics())
            context->set_alpha_function(function, reference);
    });
    registry.add("OpenGLES", 18, "classic_current_color", 4, [](HleCallFrame& frame) {
        std::array<u32, 4> color{};
        for (auto& component : color) component = frame.args.next_u32();
        if (auto* context = frame.emulator.classic_graphics())
            context->set_current_color(color); // 0x082495b8 stores four words at +0x21c
    });
    for (u32 ordinal : {32u, 36u}) {
        registry.add("OpenGLES", ordinal, "classic_set_capability", 1, [ordinal](HleCallFrame& frame) {
            const u32 capability = frame.args.next_u32();
            auto* context = frame.emulator.classic_graphics();
            if (!context) return;
            const bool enabled = ordinal == 36;
            // 0x082492c0 updates the corresponding context byte.
            if (capability == 0x0be2) context->set_blend(enabled);
            else if (capability == 0x0b71) context->set_depth_test(enabled);
            else if (capability == 0x0de1) {
                if (!context->set_texture_enabled(context->active_texture(), enabled))
                    frame.cpu.request_stop();
            } else {
                CLICKHLE_ERROR("classic") << "unsupported capability 0x"
                                          << std::hex << capability;
                frame.cpu.request_stop();
            }
        });
    }
    for (u32 ordinal : {33u, 37u}) {
        registry.add("OpenGLES", ordinal, "classic_client_array", 1, [ordinal](HleCallFrame& frame) {
            const u32 capability = frame.args.next_u32();
            auto* context = frame.emulator.classic_graphics();
            if (!context) return;
            // 0x08247eb8 also keeps texture arrays per client texture unit.
            if (!context->set_client_array(capability, ordinal == 37)) {
                CLICKHLE_ERROR("classic") << "unsupported client array";
                frame.cpu.request_stop();
            }
        });
    }
    registry.add("OpenGLES", 56, "classic_get_integer", 2, [](HleCallFrame& frame) {
        const u32 parameter = frame.args.next_u32();
        const GuestAddr output = frame.args.next_ptr();
        const auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        if (parameter == 0x84e1) {
            if (!frame.store<u32>(output, context->active_texture() + 0x84c0u))
                frame.cpu.request_stop();
        } else if (parameter == 0x0b00 && context->current_color()) {
            // 0x08243cbc multiplies each signed fixed-point component by
            // 0x7fffffff and returns the low word of the product shifted 16.
            for (u32 i = 0; i < 4; ++i) {
                const i64 component = std::bit_cast<i32>((*context->current_color())[i]);
                const u32 value = static_cast<u32>((component * i64{0x7fffffff}) >> 16);
                if (!frame.store<u32>(output + i * 4, value)) {
                    frame.cpu.request_stop(); return;
                }
            }
        } else {
            CLICKHLE_ERROR("classic") << "OpenGLES#56: unsupported query";
            frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 53, "classic_get_error", 0, [](HleCallFrame& frame) {
        auto* context = frame.emulator.classic_graphics();
        frame.ret(context ? context->take_error() : 0);
    });
    registry.add("OpenGLES", 136, "classic_vertex_pointer", 4, [](HleCallFrame& frame) {
        const u32 size = frame.args.next_u32();
        const u32 type = frame.args.next_u32();
        const u32 stride = frame.args.next_u32();
        const GuestAddr pointer = frame.args.next_ptr();
        if (auto* context = frame.emulator.classic_graphics())
            context->set_vertex_pointer(size, type, stride, pointer);
    });
    registry.add("OpenGLES", 124, "classic_texture_pointer", 4, [](HleCallFrame& frame) {
        const u32 size = frame.args.next_u32();
        const u32 type = frame.args.next_u32();
        const u32 stride = frame.args.next_u32();
        const GuestAddr pointer = frame.args.next_ptr();
        if (auto* context = frame.emulator.classic_graphics()) {
            if (!context->set_texture_pointer(size, type, stride, pointer))
                frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 61, "classic_get_pointer", 2, [](HleCallFrame& frame) {
        const u32 parameter = frame.args.next_u32();
        const GuestAddr output = frame.args.next_ptr();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        // 0x08243d98 reads the pointer retained by the corresponding array.
        if (parameter == 0x808e && context->vertex_array()) {
            if (!frame.store<u32>(output, context->vertex_array()->pointer))
                frame.cpu.request_stop();
        } else if (parameter == 0x8092 && context->texture_array() && *context->texture_array()) {
            if (!frame.store<u32>(output, (*context->texture_array())->pointer))
                frame.cpu.request_stop();
        } else if (parameter == 0x808e || parameter == 0x808f ||
                   parameter == 0x8090 || parameter == 0x8092 || parameter == 0x898c) {
            CLICKHLE_ERROR("classic") << "OpenGLES#61: array pointer not modeled for 0x"
                                      << std::hex << parameter;
            frame.cpu.request_stop();
        } else {
            context->invalid_enum();
        }
    });
    registry.add("OpenGLES", 63, "classic_get_texture_environment", 3, [](HleCallFrame& frame) {
        const u32 target = frame.args.next_u32();
        const u32 parameter = frame.args.next_u32();
        const GuestAddr output = frame.args.next_ptr();
        const auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        const auto mode = context->texture_environment(context->active_texture());
        if (target != 0x2300 || parameter != 0x2200 || !mode ||
            !frame.store<u32>(output, *mode)) {
            CLICKHLE_ERROR("classic") << "OpenGLES#63: unsupported texture environment query";
            frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 71, "classic_is_enabled", 1, [](HleCallFrame& frame) {
        const u32 capability = frame.args.next_u32();
        const auto* context = frame.emulator.classic_graphics();
        if (!context) { frame.ret(0); return; }
        // 0x0824ac48 reads the scissor enable byte at context+0x130.
        if (capability == 0x0c11) frame.ret(context->scissor_enabled());
        else {
            CLICKHLE_ERROR("classic") << "OpenGLES#71: unsupported capability";
            frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 91, "classic_matrix_mode", 1, [](HleCallFrame& frame) {
        const u32 mode = frame.args.next_u32();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        // 0x08242628 selects modelview, projection or the active texture stack.
        if (mode < 0x1700 || mode > 0x1702) context->invalid_enum();
        else context->set_matrix_mode(mode);
    });
    registry.add("OpenGLES", 83, "classic_load_identity", 0, [](HleCallFrame& frame) {
        auto* context = frame.emulator.classic_graphics();
        if (context && !context->load_identity()) {
            CLICKHLE_ERROR("classic") << "OpenGLES#83: unsupported matrix state";
            frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 114, "classic_rotate_fixed", 4, [](HleCallFrame& frame) {
        const i32 angle = std::bit_cast<i32>(frame.args.next_u32());
        const i32 x = std::bit_cast<i32>(frame.args.next_u32());
        const i32 y = std::bit_cast<i32>(frame.args.next_u32());
        const i32 z = std::bit_cast<i32>(frame.args.next_u32());
        auto* context = frame.emulator.classic_graphics();
        if (context && !context->rotate(angle, x, y, z)) {
            CLICKHLE_ERROR("classic") << "OpenGLES#114: unsupported matrix state";
            frame.cpu.request_stop();
        }
    });
    for (u32 ordinal : {85u, 93u}) {
        registry.add("OpenGLES", ordinal, "classic_matrix_fixed", 1, [ordinal](HleCallFrame& frame) {
        const GuestAddr address = frame.args.next_ptr();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        std::array<i32, 16> matrix{};
        for (u32 i = 0; i < matrix.size(); ++i) {
            const auto value = frame.memory.read<u32>(address + i * 4);
            if (!value) { frame.cpu.request_stop(); return; }
            matrix[i] = std::bit_cast<i32>(*value);
        }
        if (!(ordinal == 85 ? context->load_matrix(matrix) : context->multiply_matrix(matrix))) {
            CLICKHLE_ERROR("classic") << "unsupported matrix state";
            frame.cpu.request_stop();
        }
        });
    }
    registry.add("OpenGLES", 127, "classic_texture_environment", 3, [](HleCallFrame& frame) {
        const u32 target = frame.args.next_u32();
        const u32 parameter = frame.args.next_u32();
        const u32 value = frame.args.next_u32();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        // The original 0x0824994c maps these constants to internal bytes.
        std::optional<u8> mode;
        if (target == 0x2300 && parameter == 0x2200) {
            if (value == 0x1e01) mode = 1;
            else if (value == 0x2100) mode = 4;
            else if (value == 0x2101) mode = 0;
            else if (value == 0x0be2) mode = 2;
            else if (value == 0x0104) mode = 3;
            else if (value == 0x8570) mode = 5;
        }
        if (!mode || !context->set_texture_environment(context->active_texture(), *mode)) {
            CLICKHLE_ERROR("classic") << "OpenGLES#127: unsupported texture environment";
            frame.cpu.request_stop();
        }
    });
    registry.add("OpenGLES", 160, "classic_current_surface", 1, [](HleCallFrame& frame) {
        const u32 selector = frame.args.next_u32();
        const auto* context = frame.emulator.classic_graphics();
        // 0x082c00f0 returns null without a current context or known selector.
        frame.ret(context ? context->surface_handle(selector) : 0);
    });
    registry.add("OpenGLES", 165, "classic_swap_buffers", 2, [](HleCallFrame& frame) {
        (void)frame.args.next_ptr(); // display argument is unused by 7G wrapper
        const GuestAddr handle = frame.args.next_ptr();
        auto* context = frame.emulator.classic_graphics();
        if (!context || !context->draw_surface() || !handle ||
            handle != context->surface_handle(0x3059)) {
            CLICKHLE_ERROR("classic") << "OpenGLES#165: unsupported surface";
            frame.cpu.request_stop(); return;
        }
        const auto front = frame.memory.read<u32>(handle + 0x7c);
        const auto back = frame.memory.read<u32>(handle + 0x80);
        if (!front || !back || !*front || !*back) { frame.cpu.request_stop(); return; }
        const auto front_type = frame.memory.read<u8>(*front + 8);
        const auto back_type = frame.memory.read<u8>(*back + 8);
        const auto front_pixels = frame.memory.read<u32>(*front + 0x24);
        const auto back_pixels = frame.memory.read<u32>(*back + 0x24);
        if (!front_type || !back_type || !front_pixels || !back_pixels ||
            *front_type != 2 || *back_type != 2) { frame.cpu.request_stop(); return; }
        auto presented = *context->draw_surface();
        presented.pixels = *back_pixels;
        const auto rgb = classic::read_surface_rgb(presented, frame.memory);
        if (!rgb) { frame.cpu.request_stop(); return; }
        classic::SurfaceBuffers buffers{{*front, *front_pixels, *front_type},
                                        {*back, *back_pixels, *back_type},
                                        presented.pixels};
        (void)classic::begin_surface_swap(buffers);
        // Provisional: submission and the old-front wait are synchronous here.
        classic::finish_surface_swap(buffers);
        if (!frame.store<u32>(handle + 0x7c, buffers.front.handle) ||
            !frame.store<u32>(handle + 0x80, buffers.back.handle) ||
            !frame.store<u32>(handle + 0x68, buffers.draw_pixels)) {
            frame.cpu.request_stop(); return;
        }
        presented.pixels = buffers.draw_pixels;
        context->bind_draw_surface(presented);
        auto& display = frame.emulator.display();
        display.set_frame_rgb(*rgb, static_cast<int>(presented.bounds.width),
                              static_cast<int>(presented.bounds.height));
        if (display.is_open() || display.open(static_cast<int>(presented.bounds.width),
                                              static_cast<int>(presented.bounds.height), "ClickHLE"))
            display.present();
        if (!frame.emulator.screenshot_path().empty())
            (void)display.screenshot(frame.emulator.screenshot_path());
        context->note_present();
        frame.ret(1);
    });
    registry.add("OpenGLES", 8, "classic_clear", 1, [](HleCallFrame& frame) {
        const u32 mask = frame.args.next_u32();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        const auto result = classic::clear_bound_surface(*context, frame.memory, mask);
        if (result != classic::ClearResult::complete) {
            CLICKHLE_ERROR("classic") << "OpenGLES#8 cannot clear: "
                << (result == classic::ClearResult::missing_surface ? "draw surface is not initialized" :
                    result == classic::ClearResult::unsupported_buffers ? "depth, stencil, or surface format is unsupported" :
                    "surface memory is invalid");
            frame.cpu.request_stop();
        }
    });
    // Both setters store four 16.16 components in the current context.
    for (u32 ordinal : {9u, 10u}) {
        registry.add("OpenGLES", ordinal, ordinal == 9 ? "classic_clear_color" : "classic_clear_color_fixed", 4,
                     [ordinal](HleCallFrame& frame) {
            std::array<u32, 4> color{};
            for (u32& component : color) {
                const u32 value = frame.args.next_u32();
                component = ordinal == 9 ? classic::float_to_fixed(value) : value;
            }
            if (auto* context = frame.emulator.classic_graphics()) context->set_clear_color(color);
        });
    }
    // 0x0824a4cc normalizes each input word to a boolean byte.
    registry.add("OpenGLES", 19, "classic_color_mask", 4, [](HleCallFrame& frame) {
        std::array<u8, 4> mask{};
        for (u8& component : mask) component = frame.args.next_u32() != 0 ? 1 : 0;
        if (auto* context = frame.emulator.classic_graphics()) context->set_color_mask(mask);
    });
    registry.add("OpenGLES", 51, "classic_gen_other_names", 2, [](HleCallFrame& frame) {
        const i32 count = frame.args.next_i32();
        GuestAddr out = frame.args.next_ptr();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        if (count < 0) {
            context->invalid_value();
            return;
        }
        for (i32 i = 0; i < count; ++i, out += 4) {
            if (!frame.store<u32>(out, context->allocate_other_name())) {
                frame.cpu.request_stop();
                return;
            }
        }
    });
    registry.add("OpenGLES", 52, "classic_gen_names", 2, [](HleCallFrame& frame) {
        const i32 count = frame.args.next_i32();
        GuestAddr out = frame.args.next_ptr();
        auto* context = frame.emulator.classic_graphics();
        if (!context) return;
        if (count < 0) {
            context->invalid_value();
            return;
        }
        for (i32 i = 0; i < count; ++i, out += 4) {
            const u32 name = context->allocate_name();
            if (name == 0xffffffffu) {
                frame.emulator.stop_with_error("7G texture name free list exhausted unexpectedly");
                return;
            }
            if (!frame.store<u32>(out, name)) {
                frame.cpu.request_stop();
                return;
            }
        }
    });
    // 0x082c012c is a bare return and preserves r0.
    registry.add("OpenGLES", 140, "classic_egl_return_void", 0,
                 [](HleCallFrame&) {});
    // The 7G EGL initialization wrapper writes version 1.0 and returns true.
    registry.add("OpenGLES", 141, "classic_egl_initialize", 3, [](HleCallFrame& frame) {
        (void)frame.args.next_ptr();
        const GuestAddr major = frame.args.next_ptr();
        const GuestAddr minor = frame.args.next_ptr();
        if ((major && !frame.store<u32>(major, 1)) ||
            (minor && !frame.store<u32>(minor, 0))) {
            frame.cpu.request_stop();
            return;
        }
        frame.ret(1);
    });
    for (u32 ordinal : {142u, 163u, 164u}) {
        registry.add("OpenGLES", ordinal, "classic_egl_return_true", 0,
                     [](HleCallFrame& frame) { frame.ret(1); });
    }
    for (u32 ordinal : {152u, 153u, 154u, 155u}) {
        registry.add("OpenGLES", ordinal, "classic_egl_return_zero", 0,
                     [](HleCallFrame& frame) { frame.ret(0); });
    }
}

} // namespace clickhle
