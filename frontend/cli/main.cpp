// Command-line frontend for loading and running game images.

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "clickhle/common/log.h"
#include "clickhle/core/boot.h"
#include "clickhle/core/cpu.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/modules.h"
#include "clickhle/hle/registry.h"
#if CLICKHLE_HAVE_INSTALLER
#include "clickhle/installer/package.h"
#endif

namespace {

// Set only when --profile is active, and read only by the handler below.
clickhle::ICpuCore* g_cpu_to_stop = nullptr;

extern "C" void stop_on_signal(int) {
    if (g_cpu_to_stop != nullptr) {
        g_cpu_to_stop->request_stop();
    }
}

struct Options;
void report_instruments(const Options& options, clickhle::Emulator& emulator);

struct Options {
    std::filesystem::path executable;
    std::filesystem::path ordinal_names;
    std::filesystem::path screenshot;
    std::string cpu_backend;
    clickhle::u32 platform_id = 0;
    unsigned long clock_hz = 0;
    bool deterministic_system_clock = false;
    unsigned long scale = 0;
    std::vector<unsigned long> watches;
    std::vector<unsigned long> read_watches;
    bool watch_breaks = false;
    std::vector<std::array<unsigned long, 3>> breakpoints;
    std::filesystem::path save_state;
    std::filesystem::path load_state;
    bool fullscreen = false;
    clickhle::log::Level log_level = clickhle::log::Level::Info;
    bool run = false;
    bool plan = false;
    bool headless = false;
    bool capture_videocore = false;
    clickhle::VideoCoreEngine videocore_engine =
        clickhle::videocore_jit_available() ? clickhle::VideoCoreEngine::Jit
                                           : clickhle::VideoCoreEngine::Interpreter;
    bool show_calls = false;
    std::vector<unsigned long> tracepoints;
    unsigned long profile_period = 0;
    bool no_completions = false;
    unsigned long forced_buttons = 0;
    unsigned long forced_wheel = 0;
    std::string auto_press;
    unsigned long auto_press_interval = 60;
    std::string input_script;
    std::filesystem::path input_file;
    std::filesystem::path replay_file;
    std::string replay_text;
    unsigned long frames = 0;
    unsigned long long instructions = 0;
    bool list_missing = true;
    std::vector<std::string> probes;
    unsigned long probe_return = 0;
    bool help = false;
};

/// Prints tracepoint counts and the sampling profile.
///
/// Called from both the clean and the faulting exit, because a run that
/// faulted is precisely the run whose counters you wanted.
void report_instruments(const Options& options, clickhle::Emulator& emulator) {
    if (emulator.cpu() == nullptr) {
        return;
    }
    for (const unsigned long address : options.tracepoints) {
        std::cout << "  pc 0x" << std::hex << address << std::dec
                  << " executed "
                  << emulator.cpu()->tracepoint_hits(
                         static_cast<clickhle::u32>(address))
                  << " time(s)\n";
    }
    if (options.profile_period == 0) {
        return;
    }
    const auto top = emulator.cpu()->profile_top(20);
    clickhle::u64 total = 0;
    for (const auto& [address, hits] : top) {
        (void)address;
        total += hits;
    }
    std::cout << "hottest guest addresses (" << total << " samples, 1 per "
              << options.profile_period << " instructions):\n";
    for (const auto& [address, hits] : top) {
        const double share = total == 0 ? 0.0
                                        : 100.0 * static_cast<double>(hits) /
                                              static_cast<double>(total);
        std::cout << "  0x" << std::hex << address << std::dec << "  " << hits
                  << "  " << std::fixed << std::setprecision(1) << share
                  << "%\n";
    }
}

void print_usage(std::ostream& out) {
    out << R"(ClickHLE -- an iPod click-wheel game emulator

Usage:
  clickhle <game.bin> [options]

Options:
  --run                 Execute the image after loading it
  --plan                Group every import by subsystem and show what is left
  --cpu <backend>       CPU backend to use (default: build default)
  --platform <id>       Select a package platform ID (default: manifest order)
  --vc2-engine <name>   VideoCore engine: interpreter or jit
  --names <file>        Ordinal->name file, used to make reports about
                        unimplemented ordinals readable.
  --clock-hz <n>        Emulated clock rate (default 20000000). Guest time
                        always comes from work the guest has done, never the
                        host clock, so runs are reproducible. The default is
                        deliberately below the device's 400 MHz: HLE does in
                        one guest instruction what hardware spent millions of
                        cycles on, so the true rate runs games in slow motion.
                        Raise it for timing accuracy, lower it to skip intros
  --deterministic-clock Use emulated time for system-clock API calls
  --scale <n>           Integer window scale over the 320x240 panel
                        (default 2). Ignored with --headless, which always
                        renders at 1x since nobody is looking at it
  --fullscreen          Borderless full-screen instead of a window
  --watch <addr>        Report every write covering this guest address, with
                        the pc that did it. Repeatable. A structure that goes
                        null is nearly always an earlier write putting the
                        wrong thing there, and the fault is a long way from
                        whoever did it
  --break <spec>        Stop just before executing a guest address and dump
                        registers and the recent HLE calls. Repeatable. Costs
                        nothing when unused. Either a bare address, or
                        <addr>:r<n>=<value> to stop only when register n holds
                        that value -- which is what you need when a function is
                        called thousands of times and only the last call is
                        wrong
  --save-state <file>   Write the guest state when the run ends. Refuses, with
                        a reason, if a module is holding a host resource that
                        cannot round-trip -- an open file, queued audio
  --load-state <file>   Restore a state after loading, before running
  --watch-read <addr>   Report every read covering this address, with the pc.
                        Repeatable. --watch answers "who put this here?";
                        this answers "what is this code looking at?", which is
                        the question when a guest polls a structure whose
                        layout is unknown
  --watch-break         Make --watch stop the run at the first write instead of
                        logging every one. Turns a watchpoint into a
                        breakpoint on data, which is what you need when the
                        interesting write is one of thousands and cannot be
                        picked out by a register value
  --videocore           Accepted for compatibility; VC2 is always enabled
  --headless            Render without showing a window (works with no display
                        server at all); screenshots still work
  --capture-videocore   Save VC2 packets, raw frames and diagnostics for testing
                        (requires --headless; disabled by default)
  --instructions <n>    Stop after n guest instructions in a single callback.
                        A frame limit cannot bound a title that spins without
                        presenting; this can, and the register dump on the way
                        out says where it was spinning
  --frames <n>          Stop after n presented frames
  --press <button>      Tap a button repeatedly, to get past intros and title
                        screens with nobody at the keyboard. One of:
                        up down left right north east south west
                        lshoulder rshoulder piano1..piano5
  --press-every <n>     Frames between taps (default 60)
  --input <script>      Replay button presses at exact frames, so a button can
                        be HELD -- which is what walking, charging a shot or
                        holding a turn all need, and what --press cannot do.
                        Comma-separated <frame><+|-><button>, press and release
                        given their own frames:
                          --input 2600+right,2900-right,2905+south,2910-south
                        walks right for 300 frames, then jumps. Order does not
                        matter; frames are sorted. Same button names as --press
  --input-file <file>   Read the legacy button-transition script from a text
                        file; lines may contain transitions and # comments
  --replay-file <file>  Headless only. Replace reads of the 0x7000c140 input
                        port. Each line is <poll-count> <32-bit port word>.
                        Button bits are 8..13; wheel raw is in bits 16..22;
                        bit 30 means touched. Example: 4 0x8000011a.
  --screenshot <file>   Write every presented frame to <file> as a BMP, so the
                        file ends up holding the last frame drawn
  --probe <Lib#n>       Log every call to an ordinal, with its arguments, and
                        return 0. Repeatable. An INVESTIGATION TOOL, not an
                        implementation: probes are registered after the
                        coverage report is taken, so they never inflate it, and
                        a probed ordinal is still unimplemented. Use it to find
                        out what a game actually passes before deciding what a
                        function is -- guessing a signature from call sites
                        alone risks a plausible-looking wrong answer
  --boot <osos.bin>     Run firmware instead of a game, and report whether the
                        library descriptor table has been decrypted. Needs an
                        image extracted from device firmware. Not part of the
                        game-emulation path.
  --boot-entry <addr>   Entry point for --boot (default 0x10727800)
  --boot-instructions <n>  Instruction budget for --boot
  --boot-watch <addr>   Report writes covering this address during --boot, with
                        the pc responsible. Repeatable. How the table's
                        decryptor gets located
  --probe-returns <n>   What --probe hands back (default 0). Zero is the right
                        default -- it is what a failed call returns -- but an
                        ordinal that returns a handle will make its caller fail
                        immediately on zero, and a non-zero value tests that
                        without committing to what the function does
  --force-buttons <m>   Drive the raw button word with mask <m>, pulsed every
                        --press-every polls. Which bit is which click-wheel
                        control is not established; this finds out.
  --force-wheel <m>     Drive the wheel word with mask <m>, same pulse as
                        --force-buttons. Which of the two words a title reads
                        is testable rather than assumed.
  --no-completions      Do not deliver async file completions. Bisection only:
                        completing them changed most of the corpus at once, so
                        this answers whether a fault is caused by a completion
                        or merely reached because of one.
  --trace-pc <addr>     Count executions of a guest address WITHOUT stopping.
                        A breakpoint answers "is this reached"; only a counter
                        answers "how often", which is the question a stalled
                        title actually poses. Repeatable.
  --calls               After the run, list every import the game called and
                        how many times, most-called first. A stalled title is
                        stalled inside a loop, and the shape of that loop is
                        the fastest way to see what it is waiting for.
  --list-implemented    Print every ordinal the registry answers, then exit.
                        Source-scraping cannot see ordinals registered in a
                        loop, so anything checking our coverage has to ask the
                        registry itself
  --quiet               Do not list unimplemented imports
  --log <level>         trace | debug | info | warn | error | off
  -h, --help            Show this message

With no options, loads the image and prints an import coverage report.
)";
}

/// Parses "2600+right,2900-right" into button transitions.
///
/// Every failure names the offending term rather than just refusing: a script
/// is usually typed by hand at the end of a long command line, and "unknown
/// button 'rigth'" saves the reread that "bad script" costs.
bool parse_input_script(const std::string& text,
                        const std::map<std::string, clickhle::Button>& names,
                        std::vector<clickhle::ScriptedInput>& out,
                        std::string& error) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string term =
            text.substr(start, comma == std::string::npos ? std::string::npos
                                                          : comma - start);
        start = comma == std::string::npos ? text.size() + 1 : comma + 1;
        if (term.empty()) {
            continue;  // a trailing or doubled comma is not worth refusing over
        }

        const std::size_t sign = term.find_first_of("+-");
        if (sign == std::string::npos || sign == 0) {
            error = "'" + term + "' is not <frame><+|-><button>";
            return false;
        }
        const std::string frame_text = term.substr(0, sign);
        const std::string button_text = term.substr(sign + 1);
        if (frame_text.find_first_not_of("0123456789") != std::string::npos) {
            error = "'" + frame_text + "' is not a frame number";
            return false;
        }
        const auto it = names.find(button_text);
        if (it == names.end()) {
            error = "unknown button '" + button_text + "'";
            return false;
        }
        out.push_back({static_cast<clickhle::u32>(std::stoul(frame_text)),
                       it->second, term[sign] == '+'});
    }
    if (out.empty()) {
        error = "no events";
        return false;
    }
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value_for = [&](const char* name) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << name << " requires an argument\n";
                return {};
            }
            return args[++i];
        };

        if (arg == "-h" || arg == "--help") {
            options.help = true;
        } else if (arg == "--run") {
            options.run = true;
        } else if (arg == "--plan") {
            options.plan = true;
        } else if (arg == "--probe") {
            const std::string value = value_for("--probe");
            if (value.empty()) return false;
            options.probes.push_back(value);
        } else if (arg == "--probe-returns") {
            const std::string value = value_for("--probe-returns");
            if (value.empty()) return false;
            options.probe_return = std::stoul(value, nullptr, 0);
        } else if (arg == "--quiet") {
            options.list_missing = false;
        } else if (arg == "--cpu") {
            options.cpu_backend = value_for("--cpu");
            if (options.cpu_backend.empty()) return false;
        } else if (arg == "--platform") {
            const std::string value = value_for("--platform");
            if (value.empty()) return false;
            try {
                std::size_t used = 0;
                const auto selected = std::stoul(value, &used, 0);
                if (used != value.size() || selected == 0 || selected > 4)
                    throw std::out_of_range("platform ID");
                options.platform_id = static_cast<clickhle::u32>(selected);
            } catch (const std::exception&) {
                std::cerr << "--platform expects an ID from 1 to 4\n";
                return false;
            }
        } else if (arg == "--vc2-engine") {
            const std::string value = value_for("--vc2-engine");
            if (value == "jit") {
                options.videocore_engine = clickhle::VideoCoreEngine::Jit;
            } else if (value == "interpreter") {
                options.videocore_engine = clickhle::VideoCoreEngine::Interpreter;
            } else {
                std::cerr << "--vc2-engine must be jit or interpreter\n";
                return false;
            }
        } else if (arg == "--scale") {
            const std::string value = value_for("--scale");
            if (value.empty()) return false;
            options.scale = std::stoul(value);
        } else if (arg == "--watch") {
            const std::string value = value_for("--watch");
            if (value.empty()) return false;
            options.watches.push_back(std::stoul(value, nullptr, 0));
        } else if (arg == "--break") {
            const std::string value = value_for("--break");
            if (value.empty()) return false;
            // "<addr>" or "<addr>:r<n>=<v>"
            const auto colon = value.find(':');
            unsigned long reg_index = 32;
            unsigned long reg_value = 0;
            std::string address = value;
            if (colon != std::string::npos) {
                address = value.substr(0, colon);
                const std::string cond = value.substr(colon + 1);
                const auto equals = cond.find('=');
                if (equals == std::string::npos || cond.empty() ||
                    cond[0] != 'r') {
                    std::cerr << "--break condition must look like r0=0x8\n";
                    return false;
                }
                reg_index = std::stoul(cond.substr(1, equals - 1));
                reg_value = std::stoul(cond.substr(equals + 1), nullptr, 0);
            }
            options.breakpoints.push_back(
                {std::stoul(address, nullptr, 0), reg_index, reg_value});
        } else if (arg == "--save-state") {
            const std::string v = value_for("--save-state");
            if (v.empty()) return false;
            options.save_state = v;
        } else if (arg == "--load-state") {
            const std::string v = value_for("--load-state");
            if (v.empty()) return false;
            options.load_state = v;
        } else if (arg == "--watch-read") {
            const std::string value = value_for("--watch-read");
            if (value.empty()) return false;
            options.read_watches.push_back(std::stoul(value, nullptr, 0));
        } else if (arg == "--watch-break") {
            options.watch_breaks = true;
        } else if (arg == "--fullscreen") {
            options.fullscreen = true;
        } else if (arg == "--videocore") {
            // Accepted for compatibility; VC2 is always enabled.
        } else if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--capture-videocore") {
            options.capture_videocore = true;
        } else if (arg == "--calls") {
            options.show_calls = true;
        } else if (arg == "--trace-pc") {
            const std::string value = value_for("--trace-pc");
            if (value.empty()) return false;
            options.tracepoints.push_back(std::stoul(value, nullptr, 0));
        } else if (arg == "--profile") {
            const std::string value = value_for("--profile");
            if (value.empty()) return false;
            options.profile_period = std::stoul(value, nullptr, 0);
        } else if (arg == "--no-completions") {
            options.no_completions = true;
        } else if (arg == "--force-wheel") {
            const std::string value = value_for("--force-wheel");
            if (value.empty()) return false;
            options.forced_wheel = std::stoul(value, nullptr, 0);
        } else if (arg == "--force-buttons") {
            const std::string value = value_for("--force-buttons");
            if (value.empty()) return false;
            options.forced_buttons = std::stoul(value, nullptr, 0);
        } else if (arg == "--instructions") {
            const std::string value = value_for("--instructions");
            if (value.empty()) return false;
            options.instructions = std::stoull(value, nullptr, 0);
        } else if (arg == "--frames") {
            const std::string value = value_for("--frames");
            if (value.empty()) return false;
            options.frames = std::stoul(value);
        } else if (arg == "--clock-hz") {
            const std::string value = value_for("--clock-hz");
            if (value.empty()) return false;
            options.clock_hz = std::stoul(value);
        } else if (arg == "--deterministic-clock") {
            options.deterministic_system_clock = true;
        } else if (arg == "--press") {
            options.auto_press = value_for("--press");
            if (options.auto_press.empty()) return false;
        } else if (arg == "--input") {
            options.input_script = value_for("--input");
            if (options.input_script.empty()) return false;
        } else if (arg == "--input-file") {
            const std::string value = value_for("--input-file");
            if (value.empty()) return false;
            options.input_file = value;
        } else if (arg == "--replay-file") {
            const std::string value = value_for("--replay-file");
            if (value.empty()) return false;
            options.replay_file = value;
        } else if (arg == "--press-every") {
            const std::string value = value_for("--press-every");
            if (value.empty()) return false;
            options.auto_press_interval = std::stoul(value);
        } else if (arg == "--screenshot") {
            const std::string path = value_for("--screenshot");
            if (path.empty()) return false;
            options.screenshot = path;
        } else if (arg == "--names") {
            const std::string path = value_for("--names");
            if (path.empty()) return false;
            options.ordinal_names = path;
        } else if (arg == "--log") {
            const std::string level = value_for("--log");
            if (level.empty()) return false;
            if (!clickhle::log::parse_level(level, options.log_level)) {
                std::cerr << "unknown log level '" << level << "'\n";
                return false;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option '" << arg << "'\n";
            return false;
        } else if (options.executable.empty()) {
            options.executable = arg;
        } else {
            std::cerr << "unexpected argument '" << arg << "'\n";
            return false;
        }
    }
    if (!options.input_file.empty()) {
        if (!options.input_script.empty()) {
            std::cerr << "use either --input or --input-file, not both\n";
            return false;
        }
        std::ifstream replay(options.input_file);
        if (!replay) {
            std::cerr << "cannot open replay file '" << options.input_file.string()
                      << "'\n";
            return false;
        }
        std::string line;
        while (std::getline(replay, line)) {
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos) line.resize(comment);
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos) continue;
            const auto last = line.find_last_not_of(" \t\r");
            if (!options.input_script.empty()) options.input_script.push_back(',');
            options.input_script.append(line, first, last - first + 1);
        }
        // An empty replay is useful for a deterministic idle smoke run.
    }
    if (!options.replay_file.empty()) {
        if (!options.input_script.empty() || !options.input_file.empty()) {
            std::cerr << "use one replay format at a time\n";
            return false;
        }
        std::ifstream replay(options.replay_file);
        if (!replay) {
            std::cerr << "cannot open replay file '" << options.replay_file.string()
                      << "'\n";
            return false;
        }
        std::ostringstream contents;
        contents << replay.rdbuf();
        options.replay_text = contents.str();
    }
    return true;
}

/// Loads "<ordinal>\t<name>" lines. Blank lines and anything not starting with
/// a digit are ignored, so the tool's header line passes through harmlessly.
std::map<clickhle::u32, std::string> load_ordinal_names(
    const std::filesystem::path& path) {
    std::map<clickhle::u32, std::string> names;
    std::ifstream file(path);
    if (!file) {
        std::cerr << "warning: cannot read ordinal names from '"
                  << path.string() << "'\n";
        return names;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || std::isdigit(static_cast<unsigned char>(line[0])) == 0) {
            continue;
        }
        std::istringstream stream(line);
        unsigned long ordinal = 0;
        std::string name;
        if (stream >> ordinal >> name) {
            names.emplace(static_cast<clickhle::u32>(ordinal), std::move(name));
        }
    }
    return names;
}

/// The library a --names file describes. One library, one file: applying one
/// library's ordinal names to another's imports is silently wrong rather than
/// loudly wrong, which is the worse kind.
constexpr const char* kNamedModule = "OpenGLES";

/// Turns "OpenGLES#42" into "OpenGLES#42 (glClear)" when a name file is
/// available.
///
/// The name file is per-library: applying one library's ordinal names to
/// another's imports is silently wrong rather than loudly wrong, which is the
/// worse kind. Only the module the file was generated for is annotated.
std::string annotate(const std::string& key,
                     const std::map<clickhle::u32, std::string>& names) {
    const auto hash = key.find('#');
    if (hash == std::string::npos || names.empty()) {
        return key;
    }
    if (key.compare(0, hash, kNamedModule) != 0) {
        return key;
    }
    try {
        const auto ordinal =
            static_cast<clickhle::u32>(std::stoul(key.substr(hash + 1)));
        const auto it = names.find(ordinal);
        if (it != names.end()) {
            return key + " (" + it->second + ")";
        }
    } catch (const std::exception&) {
        // Malformed key; fall through and print it unannotated.
    }
    return key;
}

/// Groups an import into the subsystem that will implement it.
///
/// Classification is by resolved name, so it needs --names. Until the firmware
/// exports have been identified there are no names at all and everything lands
/// in one bucket -- which is an accurate picture of where the project is, not a
/// failure of the report. The point of this function is to turn a flat list of
/// 179 ordinals into "you need the texture path and six matrix calls", which is
/// a schedulable piece of work, and it can only do that once names exist.
std::string classify(const std::string& module, const std::string& name) {
    const auto has = [&name](std::string_view prefix) {
        return name.compare(0, prefix.size(), prefix) == 0;
    };
    const auto contains = [&name](std::string_view needle) {
        return name.find(needle) != std::string::npos;
    };

    if (name.empty()) {
        return "Unidentified (pass --names)";
    }
    if (has("egl")) {
        return "EGL / presentation";
    }
    if (contains("Tex") || contains("Image")) {
        return "Textures";
    }
    if (contains("Matrix") || has("glLoad") || has("glMult") ||
        has("glRotate") || has("glTranslate") || has("glScale") ||
        has("glFrustum") || has("glOrtho") || has("glPush") ||
        has("glPop")) {
        return "Matrix stack";
    }
    if (contains("Draw") || contains("Array") || contains("Pointer") ||
        contains("Buffer")) {
        return "Geometry submission";
    }
    if (contains("Blend") || contains("Depth") || contains("Stencil") ||
        contains("Alpha") || contains("Cull") || contains("Fog") ||
        contains("Light") || contains("Material")) {
        return "Raster & fragment state";
    }
    if (has("gl")) {
        return "OpenGL ES (other)";
    }
    (void)module;
    return "Uncategorised";
}

void print_plan(clickhle::Emulator& emulator, const clickhle::EappImage& image,
                const std::map<clickhle::u32, std::string>& names) {
    struct Group {
        std::size_t done = 0;
        std::vector<std::string> remaining;
    };
    std::map<std::string, Group> groups;
    std::set<std::string> seen;

    for (const auto& import : image.imports()) {
        const std::string key = import.key();
        if (!seen.insert(key).second) {
            continue;
        }
        std::string name = import.name;
        if (name.empty() && import.by_ordinal &&
            import.module == kNamedModule) {
            const auto it = names.find(import.ordinal);
            if (it != names.end()) {
                name = it->second;
            }
        }
        Group& group = groups[classify(import.module, name)];
        if (emulator.registry().resolve(import) != nullptr) {
            ++group.done;
        } else {
            group.remaining.push_back(name.empty() ? key : name);
        }
    }

    std::cout << "\n  implementation plan\n\n";
    // Most-complete first: it shows what is nearly finished and therefore
    // cheapest to close out.
    std::vector<std::pair<std::string, const Group*>> ordered;
    ordered.reserve(groups.size());
    for (const auto& [label, group] : groups) {
        ordered.emplace_back(label, &group);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        const auto total_a = a.second->done + a.second->remaining.size();
        const auto total_b = b.second->done + b.second->remaining.size();
        const double ratio_a =
            total_a == 0 ? 0.0 : static_cast<double>(a.second->done) /
                                     static_cast<double>(total_a);
        const double ratio_b =
            total_b == 0 ? 0.0 : static_cast<double>(b.second->done) /
                                     static_cast<double>(total_b);
        if (ratio_a != ratio_b) {
            return ratio_a > ratio_b;
        }
        return a.first < b.first;
    });

    for (const auto& [label, group] : ordered) {
        const std::size_t total = group->done + group->remaining.size();
        const int filled = total == 0 ? 0
                                      : static_cast<int>(10 * group->done / total);
        std::cout << "  " << std::left << std::setw(26) << label << std::right
                  << std::setw(4) << group->done << " / " << std::setw(3) << total
                  << "  ";
        for (int i = 0; i < 10; ++i) {
            std::cout << (i < filled ? "#" : ".");
        }
        std::cout << (group->remaining.empty() ? "  done" : "") << "\n";
    }

    for (const auto& [label, group] : ordered) {
        if (group->remaining.empty()) {
            continue;
        }
        std::cout << "\n  " << label << " (" << group->remaining.size()
                  << " remaining)\n";
        auto sorted = group->remaining;
        std::sort(sorted.begin(), sorted.end());
        for (const auto& entry : sorted) {
            std::cout << "      " << entry << "\n";
        }
    }
    std::cout << "\n";
}

void print_image_report(const clickhle::EappImage& image) {
    std::cout << "  format        : eapp, " << image.vector_count()
              << " entry vectors\n";
    std::cout << "  platform      : " << image.platform_id();
    if (image.platform_id() == 0) std::cout << " (unknown)";
    std::cout << "\n";
    std::cout << "  image base    : 0x" << std::hex << image.image_base()
              << "\n";
    std::cout << "  size          : " << std::dec << image.size_of_image()
              << " bytes\n";
    std::cout << "  code starts   : 0x" << std::hex << image.code_start()
              << "\n";
    std::cout << "  entry point   : 0x" << image.entry_point() << "\n";
    std::cout << "  vectors       :";
    for (const auto address : image.vectors()) {
        std::cout << " 0x" << address;
    }
    std::cout << std::dec << "\n";
    std::cout << "  import blocks : " << image.blocks().size() << "\n";
    for (const auto& block : image.blocks()) {
        std::cout << "      " << std::left << std::setw(32) << block.name
                  << std::right << std::setw(4) << block.count
                  << "  table 0x" << std::hex << block.import_table << std::dec
                  << (block.count && !block.compatible_abi ? "  (ABI unbound)" : "")
                  << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--install") {
#if CLICKHLE_HAVE_INSTALLER
        if (argc < 3 || argc > 5) {
            std::cerr << "Usage: clickhle --install <game.ipg|game.zip> [hdd] [keys]\n";
            return 2;
        }
        const auto beside_exe = std::filesystem::weakly_canonical(argv[0]).parent_path();
        const auto hdd = argc >= 4 ? std::filesystem::path(argv[3]) : beside_exe / "hdd";
        const auto keys = argc >= 5 ? std::filesystem::path(argv[4]) : beside_exe / "keys";
        try {
            clickhle::install_game_package(argv[2], hdd, keys);
            std::cout << "Game installed in " << hdd << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Installation failed: " << error.what() << '\n';
            return 1;
        }
#else
        std::cerr << "Package installation requires libzip and OpenSSL at build time\n";
        return 1;
#endif
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--list-implemented") {
            continue;
        }
        clickhle::log::set_level(clickhle::log::Level::Off);
        clickhle::HleRegistry registry;
        clickhle::register_all_modules(registry);
        for (const clickhle::HleFunction* function : registry.all()) {
            if (function->has_ordinal) {
                std::cout << function->module << "#" << function->ordinal
                          << "\t" << function->name << "\t"
                          << static_cast<unsigned>(function->arity) << "\n";
            }
        }
        return 0;
    }

    // Firmware mode is a different program that happens to share a binary, so
    // it is handled before the game options are parsed rather than folded in.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--boot") {
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "--boot wants a path to an extracted osos image\n";
            return 1;
        }
        clickhle::BootOptions boot;
        boot.image = argv[i + 1];
        for (int j = 1; j + 1 < argc; ++j) {
            const std::string flag = argv[j];
            if (flag == "--boot-entry") {
                boot.entry = static_cast<clickhle::u32>(
                    std::stoul(argv[j + 1], nullptr, 0));
            } else if (flag == "--boot-instructions") {
                boot.max_instructions = std::stoull(argv[j + 1], nullptr, 0);
            } else if (flag == "--cpu") {
                boot.cpu_backend = argv[j + 1];
            } else if (flag == "--boot-watch") {
                boot.watches.push_back(static_cast<clickhle::u32>(
                    std::stoul(argv[j + 1], nullptr, 0)));
            }
        }
        return clickhle::run_firmware(boot);
    }

    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    if (options.help || options.executable.empty()) {
        print_usage(options.help ? std::cout : std::cerr);
        return options.help ? 0 : 2;
    }

    clickhle::log::set_level(options.log_level);

    std::vector<clickhle::ReplayEntry> replay_entries;
    if (!options.replay_file.empty()) {
        std::string why;
        if (!clickhle::parse_replay(options.replay_text, replay_entries, why)) {
            std::cerr << "replay: " << why << "\n";
            return 2;
        }
    }

    clickhle::Emulator::Config config;
    config.executable = options.executable;
    config.platform_id = options.platform_id;
    if (options.capture_videocore && !options.headless) {
        std::cerr << "--capture-videocore requires --headless\n";
        return 2;
    }
    if (!options.replay_file.empty() && !options.headless) {
        std::cerr << "--replay-file requires --headless\n";
        return 2;
    }
    config.capture_videocore = options.capture_videocore;
    if (options.videocore_engine == clickhle::VideoCoreEngine::Jit &&
        !clickhle::videocore_jit_available()) {
        std::cerr << "VC2 JIT is unavailable in this build\n";
        return 2;
    }
    config.videocore_engine = options.videocore_engine;
    config.cpu_backend = options.cpu_backend;
    if (options.clock_hz != 0) {
        config.emulated_hz = options.clock_hz;
    }
    config.deterministic_system_clock = options.deterministic_system_clock ||
                                        !options.replay_file.empty();

    clickhle::Emulator emulator;
    emulator.set_screenshot_path(options.screenshot);
    emulator.set_frame_limit(options.frames);
    for (const unsigned long address : options.watches) {
        emulator.memory().watch(static_cast<clickhle::GuestAddr>(address));
    }
    for (const unsigned long address : options.read_watches) {
        emulator.memory().watch_read(static_cast<clickhle::GuestAddr>(address));
    }
    if (!options.read_watches.empty()) {
        clickhle::Emulator* target = &emulator;
        emulator.memory().set_read_watch_handler(
            [target](clickhle::GuestAddr address, clickhle::u32 size,
                     clickhle::u64 value) {
                // Unicorn exposes the executing address directly. ARM's
                // architectural PC lookahead applies to guest instructions,
                // not to ICpuCore::pc().
                const clickhle::u32 raw =
                    target->cpu() != nullptr ? target->cpu()->pc() : 0;
                std::cout << "  [read] " << size << "B at 0x" << std::hex
                          << address << " = 0x" << value << " from pc 0x"
                          << raw << std::dec << "\n";
            });
    }
    if (!options.watches.empty()) {
        clickhle::Emulator* target = &emulator;
        const bool stop_on_write = options.watch_breaks;
        emulator.memory().set_watch_handler(
            [target, stop_on_write](clickhle::GuestAddr addr, clickhle::u32 size,
                                    clickhle::u64 value) {
                const clickhle::u32 raw =
                    target->cpu() == nullptr ? 0 : target->cpu()->reg(15);
                std::cerr << "[watch] write " << size << " bytes at 0x"
                          << std::hex << addr << " = 0x" << value
                          << " from pc 0x" << raw
                          << std::dec << "\n";
                if (stop_on_write && target->cpu() != nullptr) {
                    std::ostringstream regs;
                    for (unsigned i = 0; i < clickhle::reg::kCount; ++i) {
                        regs << " r" << i << "=0x" << std::hex
                             << target->cpu()->reg(i);
                    }
                    std::cerr << "[watch] regs:" << regs.str() << "\n";
                    target->cpu()->request_stop();
                }
            });
    }
    emulator.display().set_headless(options.headless);
    emulator.display().set_scale(static_cast<int>(options.scale));
    emulator.display().set_fullscreen(options.fullscreen);
    const std::map<std::string, clickhle::Button> button_names = {
        {"up", clickhle::Button::JoypadNorth},
        {"down", clickhle::Button::JoypadSouth},
        {"left", clickhle::Button::JoypadWest},
        {"right", clickhle::Button::JoypadEast},
        {"north", clickhle::Button::ActionNorth},
        {"east", clickhle::Button::ActionEast},
        {"south", clickhle::Button::ActionSouth},
        {"west", clickhle::Button::ActionWest},
        {"lshoulder", clickhle::Button::LeftShoulder},
        {"rshoulder", clickhle::Button::RightShoulder},
        {"piano1", clickhle::Button::Piano1},
        {"piano2", clickhle::Button::Piano2},
        {"piano3", clickhle::Button::Piano3},
        {"piano4", clickhle::Button::Piano4},
        {"piano5", clickhle::Button::Piano5},
    };
    if (!options.auto_press.empty()) {
        const auto it = button_names.find(options.auto_press);
        if (it == button_names.end()) {
            std::cerr << "unknown button '" << options.auto_press << "'\n";
            return 2;
        }
        emulator.display().set_auto_press(
            it->second, static_cast<clickhle::u32>(options.auto_press_interval));
    }
    if (!options.input_script.empty()) {
        std::vector<clickhle::ScriptedInput> script;
        std::string error;
        if (!parse_input_script(options.input_script, button_names, script,
                                error)) {
            std::cerr << "--input: " << error << "\n";
            return 2;
        }
        emulator.display().set_input_script(std::move(script));
    }
    std::string error;
    if (!emulator.load(std::move(config), &error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    if (!options.replay_file.empty())
        emulator.set_replay(std::move(replay_entries));

    // After load(), because the CPU is created there -- registering earlier
    for (const unsigned long address : options.tracepoints) {
        if (emulator.cpu() != nullptr) {
            emulator.cpu()->add_tracepoint(
                static_cast<clickhle::u32>(address));
        }
    }
    if (options.profile_period != 0 && emulator.cpu() != nullptr) {
        emulator.cpu()->set_profile_period(options.profile_period);
        // A spin has to be interruptible to be measurable: the histogram is
        // useless if the only way to end the run is a kill that takes the
        // process down before it prints. SIGINT/SIGTERM ask the core to stop,
        // the run unwinds normally, and the summary below still runs.
        g_cpu_to_stop = emulator.cpu();
        std::signal(SIGINT, stop_on_signal);
        std::signal(SIGTERM, stop_on_signal);
    }
    // silently dropped every breakpoint against a null core.
    for (const auto& bp : options.breakpoints) {
        if (emulator.cpu() != nullptr) {
            emulator.cpu()->add_breakpoint(
                static_cast<clickhle::u32>(bp[0]),
                static_cast<unsigned>(bp[1]),
                static_cast<clickhle::u32>(bp[2]));
        }
    }

    const clickhle::EappImage* image = emulator.image();
    std::cout << "\n" << options.executable.filename().string() << "\n";
    print_image_report(*image);

    std::map<std::string, std::size_t> per_module;
    for (const auto& import : image->imports()) {
        ++per_module[import.module];
    }
    std::cout << "  imports       : " << image->imports().size() << "\n";
    for (const auto& [module, count] : per_module) {
        std::cout << "      " << module << "  " << count << "\n";
    }

    const auto coverage = emulator.coverage();
    const double percent =
        coverage.total == 0
            ? 100.0
            : 100.0 * static_cast<double>(coverage.implemented) /
                  static_cast<double>(coverage.total);
    // Labelled "of the linked interface" deliberately. The denominator is the
    // sum of the *full* interface sizes this title binds -- what RetailOS
    // publishes -- not what the title needs. Tetris links 429 slots and calls
    // 69 of them, so reading this as "22% done" was wrong in the pessimistic
    // direction for a long time and pointed work at ordinals nothing calls.
    //
    // For the number that matters -- implemented against *required* -- use
    // tools/used_slots.py, which measures against the slots titles actually
    // reference. On that basis the corpus stands at 100 of 160.
    std::cout << "\n  coverage      : " << coverage.implemented << " / "
              << coverage.total << " of the linked interface ("
              << static_cast<int>(percent) << "%)\n"
              << "                  the linked size is not the requirement; "
                 "see tools/used_slots.py\n";

    const auto names = options.ordinal_names.empty()
                           ? std::map<clickhle::u32, std::string>{}
                           : load_ordinal_names(options.ordinal_names);

    if (options.plan) {
        print_plan(emulator, *image, names);
    } else if (options.list_missing && !coverage.missing.empty()) {
        std::cout << "\n  unimplemented (" << coverage.missing.size() << "):\n";
        for (const auto& key : coverage.missing) {
            std::cout << "      " << annotate(key, names) << "\n";
        }
        if (names.empty()) {
            std::cout
                << "\n  (pass --names <file> to see the "
                   "function\n   names behind these ordinals)\n";
        }
    }

    if (!options.run) {
        return 0;
    }

    std::cout << "\n";
    if (!options.load_state.empty()) {
        const std::string why = emulator.load_state(options.load_state);
        if (!why.empty()) {
            std::cerr << "could not load state: " << why << "\n";
            return 1;
        }
    }

    // Probes are installed here, after the coverage report above has already
    emulator.set_completions_enabled(!options.no_completions);
    if (const char* base = std::getenv("CLICKHLE_EVENT_TYPE_BASE")) {
        clickhle::set_event_type_base(
            static_cast<clickhle::u32>(std::strtoul(base, nullptr, 0)));
    }
    clickhle::set_forced_wheel(
        static_cast<clickhle::u32>(options.forced_wheel));
    clickhle::set_forced_buttons(
        static_cast<clickhle::u32>(options.forced_buttons),
        static_cast<clickhle::u32>(options.auto_press_interval));

    // been printed, so a probed ordinal never counts as implemented. It is not
    // implemented -- it logs what it was handed and returns zero.
    for (const std::string& spec : options.probes) {
        const std::size_t hash = spec.find('#');
        if (hash == std::string::npos) {
            std::cerr << "--probe wants Library#ordinal, not '" << spec << "'\n";
            return 1;
        }
        const std::string module = spec.substr(0, hash);
        clickhle::u32 ordinal = 0;
        try {
            ordinal = static_cast<clickhle::u32>(std::stoul(spec.substr(hash + 1)));
        } catch (const std::exception&) {
            std::cerr << "--probe: '" << spec << "' has no ordinal\n";
            return 1;
        }
        // Six words: four in registers and two off the stack, which covers
        // every call shape seen so far. Reading more than a function really
        // takes is harmless -- ArgReader reports when it runs off the mapped
        // stack, and those words print as unavailable rather than as data.
        const bool installed = emulator.install_probe(
            module, ordinal, 8,
            [spec, ret = options.probe_return](clickhle::HleCallFrame& f) {
                std::ostringstream line;
                line << spec << "(";
                for (int i = 0; i < 8; ++i) {
                    const clickhle::u32 word = f.args.next_u32();
                    if (i) line << ", ";
                    if (!f.args.ok()) { line << "?"; break; }
                    line << "0x" << std::hex << word << std::dec;
                    // Show the text when an argument points at a printable,
                    // NUL-terminated string. Filenames are the whole reason
                    // the file API is hard to read: the games build their
                    // paths at run time, so the string is never in the image
                    // and static analysis cannot see it.
                    std::string text;
                    for (clickhle::u32 k = 0; k < 96; ++k) {
                        const auto c = f.memory.read<clickhle::u8>(word + k);
                        if (!c.has_value() || *c == 0) break;
                        if (*c < 0x20 || *c > 0x7e) { text.clear(); break; }
                        text.push_back(static_cast<char>(*c));
                    }
                    if (text.size() >= 3) {
                        line << " \"" << text << "\"";
                    }
                }
                line << ")";
                std::cout << "  probe " << line.str() << "\n";
                f.ret(static_cast<clickhle::u32>(ret));
            });
        if (!installed) {
            std::cerr << spec << " is not imported by this title\n";
            continue;
        }
        std::cout << "probing " << spec << " (still counted unimplemented)\n";
    }

    const clickhle::RunResult result =
        emulator.run_game(options.instructions);

    // Before the outcome is dispatched on, so a title that faults still reports
    // what it called. It used to sit in the success branch, which meant a
    // faulting title showed zero calls -- and that is the case where knowing
    // what it was doing matters most. It also silently corrupted a corpus-wide
    // measurement: Texas Hold'em reported calling nothing at all.
    if (options.show_calls) {
        const auto names_for_calls =
            options.ordinal_names.empty()
                ? std::map<clickhle::u32, std::string>{}
                : load_ordinal_names(options.ordinal_names);
        std::vector<std::pair<std::string, clickhle::u64>> ranked(
            emulator.call_counts().begin(), emulator.call_counts().end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second;
                  });
        const auto& audit = emulator.return_audit();
        std::cout << "\nreturn values: " << audit.calls << " calls, "
                  << audit.zero << " zero, " << audit.small
                  << " handle-shaped, " << audit.mapped
                  << " a mapped address\n";
        if (audit.mapped != 0) {
            std::cout << "  a mapped return is one a title could dereference "
                         "without faulting:\n";
            for (const auto& [key, count] : audit.mapped_by_import) {
                std::cout << "      " << std::setw(9) << count << "  "
                          << key << "\n";
            }
        }

        std::cout << "\ndraws by texture:\n";
        for (const auto& [texture, count] : clickhle::gles_draws_by_texture(emulator)) {
            std::cout << "  " << std::setw(9) << count << "  texture "
                      << texture << "\n";
        }
        std::cout << "\ncalls by import:\n";
        for (const auto& [key, count] : ranked) {
            std::cout << "  " << std::setw(9) << count << "  "
                      << annotate(key, names_for_calls) << "\n";
        }
    }

    if (!options.save_state.empty()) {
        const std::string why = emulator.save_state(options.save_state);
        if (!why.empty()) {
            std::cerr << "could not save state: " << why << "\n";
        }
    }
    switch (result.status) {
        case clickhle::RunStatus::Unsupported:
            std::cerr << "cannot run: " << result.fault << "\n";
            return 1;
        case clickhle::RunStatus::Fault:
            std::cerr << "faulted after " << result.instructions_executed
                      << " instructions: " << result.fault << "\n";
            // Report the instruments on the way out. They used to print only on
            // the clean path, so a faulting run -- exactly the run you attached
            // a tracepoint to -- discarded every count silently and looked as
            // though the addresses had never been reached. Same failure mode as
            // the screenshot that held the last pre-fault frame.
            report_instruments(options, emulator);
            return 1;
        default:
            std::cout << "stopped after " << result.instructions_executed
                      << " instructions; " << emulator.hle_call_count()
                      << " HLE calls (" << emulator.hle_missing_call_count()
                      << " unimplemented)\n";
            clickhle::u64 frames = 0;
            clickhle::u64 draws = 0;
            clickhle::gles_statistics(emulator, frames, draws);
            std::cout << "rendered " << frames << " frames, " << draws
                      << " draw calls\n";
            report_instruments(options, emulator);

            if (!emulator.last_missing_import().empty()) {
                std::cout << "next to implement: "
                          << annotate(emulator.last_missing_import(), names)
                          << "\n";
            }
            return 0;
    }
}
