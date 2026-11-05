// SDL3 windowing backend.
//
// The only file in the project that includes SDL. Everything else talks to
// Display, so swapping SDL for something else is one file plus a CMake branch.

#include <algorithm>
#include <deque>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "clickhle/common/log.h"
#include "clickhle/video/display.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "video";

class SdlDisplay final : public Display {
public:
    ~SdlDisplay() override { close(); }

    [[nodiscard]] std::string_view backend() const override { return "sdl3"; }
    [[nodiscard]] bool usable() const override { return true; }

    void set_headless(bool headless) override { headless_ = headless; }
    [[nodiscard]] int scale() const override { return scale_; }

    void set_scale(int scale) override {
        if (scale > 0) {
            scale_ = scale;
            scale_explicit_ = true;
        }
    }

    void set_fullscreen(bool fullscreen) override { fullscreen_ = fullscreen; }

    [[nodiscard]] bool open(int width, int height,
                            const std::string& title) override {
        if (window_ != nullptr) {
            return true;
        }
        // SDL installs its own SIGINT/SIGTERM handlers, which only set an
        // internal quit flag. Any loop that does not poll for that flag then
        // ignores Ctrl-C and, worse, survives `timeout` and kill -TERM -- an
        // emulator that wedges becomes an orphan burning a core until someone
        // notices. We do our own event handling, so take the signals back.
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

        if (headless_) {
            // SDL's offscreen driver renders through Mesa's surfaceless EGL, so
            // this works with no display server at all -- not merely a window
            // that happens to be hidden.
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            scale_ = 1;  // no point scaling up something nobody is looking at
            scale_explicit_ = false;  // and an explicit --scale cannot override

        }
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            CLICKHLE_ERROR(kCategory) << "SDL_InitSubSystem: " << SDL_GetError();
            return false;
        }

        // A 2.1 compatibility context, deliberately.
        //
        // OpenGL ES 1.0 is a fixed-function API: matrix stacks, glVertexPointer,
        // alpha test. Desktop GL up to 2.1 has all of it, so the translation is
        // close to one-for-one and the renderer stays small. A core-profile
        // path with an ubershader is the eventual answer (roadmap task #20) and
        // is what a modern macOS build would need, but it buys nothing yet.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);

        // Presented at 2x so a 320x240 panel is legible on a modern monitor;
        // the guest still renders at native resolution and the GLES layer
        // scales its viewport to match.
        SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
        // SDL installs its own SIGINT/SIGTERM handlers, which only set an
        // internal quit flag. Any loop that does not poll for that flag then
        // ignores Ctrl-C and, worse, survives `timeout` and kill -TERM -- an
        // emulator that wedges becomes an orphan burning a core until someone
        // notices. We do our own event handling, so take the signals back.
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

        if (headless_) {
            flags |= SDL_WINDOW_HIDDEN;
        } else if (fullscreen_) {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        window_ = SDL_CreateWindow(title.c_str(), width * scale_,
                                   height * scale_, flags);
        if (window_ == nullptr) {
            CLICKHLE_ERROR(kCategory) << "SDL_CreateWindow: " << SDL_GetError();
            return false;
        }
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            CLICKHLE_ERROR(kCategory) << "SDL_GL_CreateContext: " << SDL_GetError();
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            return false;
        }
        SDL_GL_MakeCurrent(window_, context_);
        SDL_GL_SetSwapInterval(0);  // the guest paces itself

        width_ = width;
        height_ = height;
        CLICKHLE_INFO(kCategory)
            << (headless_ ? "opened headless " : "opened ") << width << "x"
            << height << " surface at " << scale_ << "x, GL "
            << reinterpret_cast<const char*>(glGetString(GL_VERSION));
        // The host's texture-dimension ceiling. musika asks for 23644x1 and
        // 77824x1 storage allocations; whether those are refusable at all
        // depends on this number, so log it rather than assume the usual
        // 16384.
        {
            GLint max_tex = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
            CLICKHLE_INFO(kCategory) << "GL_MAX_TEXTURE_SIZE " << max_tex;
        }
        return true;
    }

    void close() override {
        if (context_ != nullptr) {
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    [[nodiscard]] bool is_open() const override { return window_ != nullptr; }

    void present() override {
        if (window_ != nullptr) {
            SDL_GL_SwapWindow(window_);
        }
    }

    [[nodiscard]] bool pump() override {
        SDL_Event event;
        bool keep_running = true;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    keep_running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    if (event.key.key == SDLK_ESCAPE) {
                        // Host-side quit. The device's own "menu" button is the
                        // right shoulder, so Escape is free to mean this.
                        keep_running = false;
                        break;
                    }
                    if (event.key.repeat) {
                        break;  // the guest expects one message per transition
                    }
                    Button button = Button::Count;
                    if (map_key(event.key.key, button)) {
                        input_.push_back({button,
                                          event.type == SDL_EVENT_KEY_DOWN});
                    }
                    break;
                }
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (gamepad_ == nullptr) {
                        gamepad_ = SDL_OpenGamepad(event.gdevice.which);
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad_ != nullptr) {
                        SDL_CloseGamepad(gamepad_);
                        gamepad_ = nullptr;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                    Button button = Button::Count;
                    if (map_gamepad(event.gbutton.button, button)) {
                        input_.push_back(
                            {button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN});
                    }
                    break;
                }
                default:
                    break;
            }
        }
        if (auto_interval_ != 0) {
            // A press and its release, a few pumps apart, so the guest sees a
            // genuine transition rather than two events in the same frame.
            ++auto_counter_;
            if (auto_counter_ % auto_interval_ == 0) {
                input_.push_back({auto_button_, true});
            } else if (auto_counter_ % auto_interval_ == 3) {
                input_.push_back({auto_button_, false});
            }
        }
        // Everything scheduled for this pump or any pump already missed, so a
        // script whose frames outrun the run's length still ends with every
        // button released rather than stuck down.
        while (script_next_ < script_.size() &&
               script_[script_next_].frame <= script_counter_) {
            const ScriptedInput& scripted = script_[script_next_++];
            input_.push_back({scripted.button, scripted.pressed});
        }
        ++script_counter_;
        return keep_running;
    }

    void inject(Button button, bool pressed) override {
        input_.push_back({button, pressed});
    }

    void set_auto_press(Button button, u32 interval) override {
        auto_button_ = button;
        auto_interval_ = interval;
        auto_counter_ = 0;
    }

    void set_input_script(std::vector<ScriptedInput> script) override {
        // Stable sort by frame: two events on the same frame keep the order
        // they were written in, so "release then press" to flip direction
        // without a gap does what it reads like.
        std::stable_sort(script.begin(), script.end(),
                         [](const ScriptedInput& a, const ScriptedInput& b) {
                             return a.frame < b.frame;
                         });
        script_ = std::move(script);
        script_next_ = 0;
        script_counter_ = 0;
    }

    [[nodiscard]] bool next_input(InputEvent& out) override {
        if (input_.empty()) {
            return false;
        }
        out = input_.front();
        input_.pop_front();
        return true;
    }

    [[nodiscard]] bool screenshot(const std::filesystem::path& path) override {
        if (window_ == nullptr) {
            return false;
        }
        int w = width_;
        int h = height_;
        SDL_GetWindowSizeInPixels(window_, &w, &h);

        const auto row = static_cast<std::size_t>(w) * 3u;
        std::vector<u8> pixels(row * static_cast<std::size_t>(h));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // GL_BGR and bottom-up rows are exactly BMP's layout, so the readback
        // needs no further conversion.
        glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());
        return write_bmp(path, w, h, pixels.data());
    }

    [[nodiscard]] std::vector<u8> read_rgba(int& width, int& height) override {
        if (window_ == nullptr) {
            width = 0;
            height = 0;
            return {};
        }
        width = width_;
        height = height_;
        std::vector<u8> pixels(static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4u);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // The guest frame ends at SDL_GL_SwapWindow. Read the presented front
        // buffer; the back buffer is undefined after a double-buffer swap.
        glReadBuffer(GL_FRONT);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        glReadBuffer(GL_BACK);
        return pixels;
    }

private:
    /// Keyboard to click wheel.
    ///
    /// The device has SIX controls and no more: the ring (which reports an
    /// absolute position, so it is *held* to scroll, not tapped), its four taps
    /// -- Menu, Forward, Play, Back -- and the centre Select button.
    ///
    /// The old table bound Return and Z to `ActionSouth`, and Q/W/1-5 to
    /// shoulders and piano keys. **None of those exist on this hardware.**
    /// `button_event_type()` maps them to event type 0, which posts no record
    /// at all, so the two most instinctive confirm keys on a keyboard did
    /// literally nothing. That is a trap worth removing rather than
    /// documenting: Return and Space now Select, which is what a person
    /// pressing them means.
    ///
    /// The ring is on Up/Down as well as X/S, because "scroll a menu" is what
    /// the ring is for and the arrow keys are where a person reaches for it.
    /// HOLD them -- games read wheel position and difference it into velocity,
    /// so a tap barely moves the selection.
    ///
    /// Left/Right stay on the Back and Forward taps, which is what they are.
    /// The four taps keep their arrow bindings where they do not collide.
    static bool map_key(SDL_Keycode key, Button& out) {
        switch (key) {
            // The ring. Held, not tapped.
            case SDLK_DOWN:
            case SDLK_X:        out = Button::ActionEast;    return true;
            case SDLK_UP:
            case SDLK_S:        out = Button::ActionWest;    return true;

            // Select, the centre button. Every key a person might press to
            // confirm.
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
            case SDLK_Z:
            case SDLK_A:        out = Button::ActionNorth;   return true;

            // The wheel's taps.
            case SDLK_RIGHT:    out = Button::JoypadEast;    return true;
            case SDLK_LEFT:     out = Button::JoypadWest;    return true;
            case SDLK_M:        out = Button::JoypadNorth;   return true;  // Menu
            case SDLK_P:        out = Button::JoypadSouth;   return true;  // Play
            default: return false;
        }
    }

    /// The action buttons are a diamond, so they map onto a modern pad's face
    /// buttons positionally rather than by label.
    static bool map_gamepad(u8 button, Button& out) {
        switch (button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:        out = Button::JoypadNorth;   return true;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     out = Button::JoypadEast;    return true;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      out = Button::JoypadSouth;   return true;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      out = Button::JoypadWest;    return true;
            case SDL_GAMEPAD_BUTTON_NORTH:          out = Button::ActionNorth;   return true;
            case SDL_GAMEPAD_BUTTON_EAST:           out = Button::ActionEast;    return true;
            case SDL_GAMEPAD_BUTTON_SOUTH:          out = Button::ActionSouth;   return true;
            case SDL_GAMEPAD_BUTTON_WEST:           out = Button::ActionWest;    return true;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  out = Button::LeftShoulder;  return true;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: out = Button::RightShoulder; return true;
            case SDL_GAMEPAD_BUTTON_START:          out = Button::Piano1;        return true;
            default: return false;
        }
    }

    std::deque<InputEvent> input_;
    Button auto_button_ = Button::ActionSouth;
    u32 auto_interval_ = 0;
    u32 auto_counter_ = 0;
    std::vector<ScriptedInput> script_;
    std::size_t script_next_ = 0;
    u32 script_counter_ = 0;
    SDL_Gamepad* gamepad_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int scale_ = 2;
    bool scale_explicit_ = false;
    bool fullscreen_ = false;
    bool headless_ = false;
};

}  // namespace

std::unique_ptr<Display> make_sdl_display() {
    return std::make_unique<SdlDisplay>();
}

}  // namespace clickhle
