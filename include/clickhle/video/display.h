#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

/// Inherited from the Gizmondo's control set and still to be replaced by the
/// click wheel: spin direction and velocity, taps at the cardinal points, and
/// the centre button. See milestone M6.
/// a four-way joypad left of the screen, four action buttons right of it, two
/// shoulder buttons, and five "piano" keys across the top.
///
/// Deliberately physical rather than virtual-key: the display backend knows
/// about host keyboards and gamepads, and the HLE layer knows about VK codes.
/// Neither should have to know about the other.
/// The click wheel's controls.
///
/// Inherited names on the left of each line, kept because the display backend
/// and the --input script parser already speak them. What matters is the right:
/// a click wheel is a ring you rotate, four taps at the cardinal points, and a
/// centre button.
enum class Button {
    JoypadNorth,    ///< Menu    -- the wheel's top tap
    JoypadEast,     ///< Forward -- right tap
    JoypadSouth,    ///< Play    -- bottom tap
    JoypadWest,     ///< Back    -- left tap
    ActionNorth,    ///< Select  -- the centre button
    /// Rotate the wheel. Not buttons on the device, but a keyboard has no ring:
    /// holding one advances the wheel position, which is what the games read.
    ActionEast,     ///< spin clockwise
    ActionWest,     ///< spin anticlockwise
    ActionSouth,
    LeftShoulder, RightShoulder,
    Piano1, Piano2, Piano3, Piano4, Piano5,
    Count,
};

struct InputEvent {
    Button button = Button::Count;
    bool pressed = false;
};

/// One button transition at a known point in a run.
///
/// Frames rather than seconds because guest time is derived from guest work,
/// so a frame number is the one clock both sides agree on and a scripted run
/// replays identically every time.
struct ScriptedInput {
    u32 frame = 0;
    Button button = Button::Count;
    bool pressed = false;
};

/// The host window and GL context.
///
/// An interface rather than a direct SDL dependency for two reasons: the tree
/// must still build and test with no SDL present, and nothing in src/ or
/// include/ is allowed to branch on the host platform -- so the windowing
/// backend is chosen at configure time, not by #ifdef.
class Display {
public:
    virtual ~Display() = default;

    /// Creates the backend compiled into this build. Never returns null: with
    /// no SDL, a headless implementation is returned whose open() fails
    /// cleanly, so a game that wants graphics gets a diagnostic rather than a
    /// crash.
    [[nodiscard]] static std::unique_ptr<Display> create();

    [[nodiscard]] virtual std::string_view backend() const = 0;
    [[nodiscard]] virtual bool usable() const = 0;

    /// Renders and presents without showing a window.
    ///
    /// Must be set before open(). Needed for tests and CI: a suite that pops up
    /// windows cannot run in the background, and a machine with no display
    /// server should still be able to check that the right pixels come out.
    /// Screenshots keep working -- that is the whole point.
    virtual void set_headless(bool headless) = 0;

    /// Integer factor between the host surface and the guest's 320x240 panel.
    [[nodiscard]] virtual int scale() const = 0;

    /// Sets that factor. Must be called before open(); zero or negative is
    /// ignored. Headless overrides it to 1 -- there is no point scaling up
    /// something nobody is looking at, and it only costs fill rate.
    virtual void set_scale(int scale) = 0;

    /// Opens borderless full-screen at the desktop resolution instead of a
    /// window. Must be set before open().
    virtual void set_fullscreen(bool fullscreen) = 0;

    /// Opens a window with a GL context current on the calling thread.
    [[nodiscard]] virtual bool open(int width, int height,
                                    const std::string& title) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    /// Presents the back buffer.
    virtual void present() = 0;

    /// Drains host events. Returns false once the user has asked to quit,
    /// which the guest sees as a WM_CLOSE. Button presses seen while pumping
    /// are queued for next_input().
    [[nodiscard]] virtual bool pump() = 0;

    /// Pops one queued button event. Returns false when the queue is empty.
    [[nodiscard]] virtual bool next_input(InputEvent& out) = 0;

    /// Queues a synthetic button press as though the user had made it.
    virtual void inject(Button button, bool pressed) = 0;

    /// Taps `button` every `interval` pumps, forever. Zero disables.
    ///
    /// For headless testing: intros and title screens wait for a button, and
    /// without this there is no way to get past them with no user present.
    virtual void set_auto_press(Button button, u32 interval) = 0;

    /// Replays a list of button transitions at given pump counts.
    ///
    /// set_auto_press only taps, which is enough to dismiss a title screen and
    /// no use at all for playing: walking, charging a shot and holding a turn
    /// all need a button down across many frames. A script gives press and
    /// release their own frame numbers, so a hold is just the gap between them.
    /// Events may be listed in any order; they are sorted on the way in.
    virtual void set_input_script(std::vector<ScriptedInput> script) = 0;

    /// Reads back the framebuffer and writes it as a 24-bit BMP.
    ///
    /// This is the project's ground truth for "does it draw". It also makes the
    /// renderer testable without a human looking at a screen, and gives the
    /// framebuffer diff harness (roadmap task #31) something to compare.
    [[nodiscard]] virtual bool screenshot(const std::filesystem::path& path) = 0;

    /// Reads the current framebuffer as bottom-up RGBA8 pixels. Returns an
    /// empty buffer when the backend has no live GL surface.
    [[nodiscard]] virtual std::vector<u8> read_rgba(int& width, int& height) = 0;
};

/// Writes 24-bit BGR rows, bottom-up, as an uncompressed BMP. Separate from the
/// display so tests can use it without a window.
[[nodiscard]] bool write_bmp(const std::filesystem::path& path, int width,
                             int height, const u8* bgr_bottom_up);

}  // namespace clickhle
