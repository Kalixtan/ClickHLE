#include "clickhle/video/classic_graphics.h"
#include "clickhle/video/classic_rotation.h"

int main() {
    clickhle::classic::GraphicsContext context;
    // Cross both initial capacity and the next growth boundary.
    for (clickhle::u32 name = 1; name < 260; ++name)
        if (context.allocate_name() != name) return 1;
    for (clickhle::u32 name = 1; name < 130; ++name)
        if (context.allocate_other_name() != name) return 1;
    if (context.error() != 0) return 1;
    context.invalid_value();
    if (context.error() != 0x501) return 1;
    context.set_alpha_function(0x208, 0);
    if (context.error() != 0x501) return 1;
    clickhle::classic::GraphicsContext fresh;
    if (fresh.allocate_name() != 1 || fresh.error() != 0) return 1;
    clickhle::classic::GraphicsContext binding;
    if (!binding.bind_texture(5) || !binding.bind_texture(1) ||
        !binding.bind_texture(5) || !binding.bind_texture(0)) return 1;
    for (clickhle::u32 name : {2u, 3u, 4u, 6u})
        if (binding.allocate_name() != name) return 1;
    if (fresh.texture_environment(0) != 0x2100 ||
        !fresh.set_texture_environment(0, 1) ||
        fresh.texture_environment(0) != 0x1e01) return 1;
    if (!fresh.load_identity()) return 1;
    std::array<clickhle::i32, 16> identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 65536;
    if (!fresh.load_matrix(identity)) return 1;
    identity[0] = 0;
    if (fresh.load_matrix(identity)) return 1;
    // 7G firmware's 180-degree X rotation is not an ideal integer matrix.
    const auto rotation = clickhle::classic::rotation_matrix(180 * 65536, 65536, 0, 0);
    if (rotation[0] != 65536 || rotation[5] != -65534 || rotation[6] != 100 ||
        rotation[9] != -100 || rotation[10] != -65534 || rotation[15] != 65536)
        return 1;
    fresh.set_matrix_mode(0x1701);
    if (!fresh.load_identity() || !fresh.rotate(180 * 65536, 65536, 0, 0) ||
        !fresh.load_identity()) return 1;
    fresh.set_matrix_mode(0x1702);
    if (!fresh.load_identity()) return 1;
    fresh.set_active_texture(1);
    fresh.invalid_enum();
    if (fresh.active_texture() != 1 || fresh.error() != 0x500) return 1;
    clickhle::classic::GraphicsContext blend;
    blend.set_blend_factors(0x302, 0x303);
    if (blend.error() != 0) return 1;
    blend.set_blend_factors(0x300, 0x303);
    if (blend.error() != 0x500) return 1;
    if (blend.take_error() != 0x500 || blend.take_error() != 0) return 1;
    clickhle::classic::GraphicsContext alpha;
    for (clickhle::u32 i = 0; i < 8; ++i) {
        alpha.set_alpha_function(0x200 + i, 0x87654321);
        if (alpha.error() || alpha.alpha_function() != i ||
            alpha.alpha_reference() != 0x87654321) return 1;
    }
    alpha.set_alpha_function(0x208, 0);
    if (alpha.error() != 0x500 || alpha.alpha_function() != 7 ||
        alpha.alpha_reference() != 0x87654321) return 1;
    clickhle::classic::GraphicsContext vertex;
    vertex.set_vertex_pointer(3, 0x1402, 0, 0x18045678);
    if (!vertex.vertex_array() || vertex.vertex_array()->size != 3 ||
        vertex.vertex_array()->type != 0x1402 || vertex.vertex_array()->stride != 6 ||
        vertex.vertex_array()->pointer != 0x18045678 || vertex.vertex_array()->buffer) return 1;
    vertex.set_vertex_pointer(1, 0x1402, 0, 0);
    if (vertex.take_error() != 0x501 || vertex.vertex_array()->stride != 6) return 1;
    vertex.set_vertex_pointer(3, 0x1401, 0, 0);
    if (vertex.take_error() != 0x500 || vertex.vertex_array()->stride != 6) return 1;
    if (!vertex.set_texture_pointer(3, 0x1402, 0, 0x12345678) ||
        !vertex.texture_array() || !*vertex.texture_array() ||
        (*vertex.texture_array())->stride != 6 ||
        (*vertex.texture_array())->pointer != 0x12345678) return 1;
    vertex.set_client_texture(1);
    if (!vertex.set_texture_pointer(4, 0x1406, 24, 0x87654321) ||
        !*vertex.texture_array() || (*vertex.texture_array())->stride != 24) return 1;
    vertex.set_client_texture(0);
    if ((*vertex.texture_array())->pointer != 0x12345678) return 1;
    if (!vertex.set_texture_pointer(1, 0x1401, 0, 0) || vertex.take_error() != 0x500 ||
        (*vertex.texture_array())->pointer != 0x12345678) return 1;
}
