#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#include "nfd.h"
#include "mt_screenscraper.h"
#include "mt_netplay.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

static std::string version_string;

Rml::DataModelHandle model_handle;
bool mm_rom_valid = false;

// Mario Tennis recomp: ScreenScraper artwork, shown over the built-in court when it arrives (see ui_screenscraper.cpp).
static std::string ss_background_decorator = "none";
static std::string ss_logo_src;
static bool ss_has_background = false;
static bool ss_has_logo = false;

extern std::vector<recomp::GameEntry> supported_games;

// Online Play panel (src/game/mt_netplay.cpp does the networking). The choices are kept in
// netplay.txt in the app folder between runs.
namespace {
    bool net_open = false;
    bool net_host = true;
    std::string net_address;
    std::string net_port = "7777";
    int net_delay = 2;
    int net_pad = 0;            // 0 = all, 1 = keyboard, 2.. = controller 1..
    std::string net_pad_label;
    std::string net_status;
    bool net_busy = false;
    bool net_failed = false;

    std::filesystem::path net_settings_path() {
        return zelda64::get_app_folder_path() / "netplay.txt";
    }

    void net_update_pad_label() {
        if (net_pad == 0) net_pad_label = "Keyboard and all controllers";
        else if (net_pad == 1) net_pad_label = "Keyboard only";
        else net_pad_label = "Controller " + std::to_string(net_pad - 1) + " only";
    }

    void net_load_settings() {
        std::ifstream f(net_settings_path());
        std::string key, value;
        while (f >> key && std::getline(f >> std::ws, value)) {
            if (key == "host") net_host = value == "1";
            else if (key == "address") net_address = value;
            else if (key == "port") net_port = value;
            else if (key == "delay") net_delay = std::clamp(std::atoi(value.c_str()), 0, 10);
            else if (key == "pad") net_pad = std::clamp(std::atoi(value.c_str()), 0, 5);
        }
        net_update_pad_label();
    }

    void net_save_settings() {
        std::ofstream f(net_settings_path());
        f << "host " << (net_host ? 1 : 0) << "\naddress " << net_address << "\nport " << net_port
          << "\ndelay " << net_delay << "\npad " << net_pad << "\n";
    }

    void net_dirty() {
        for (const char* v : { "net_open", "net_host", "net_address", "net_port", "net_delay", "net_pad_label",
                               "net_status", "net_busy", "net_failed" }) {
            model_handle.DirtyVariable(v);
        }
    }

    std::string sentence(std::string s) {
        if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
        if (!s.empty() && s.back() != '.') s += '.';
        return s;
    }

    void net_start() {
        // Trim spaces typed around the address.
        while (!net_address.empty() && std::isspace((unsigned char)net_address.back())) net_address.pop_back();
        while (!net_address.empty() && std::isspace((unsigned char)net_address.front())) net_address.erase(0, 1);
        int port = std::atoi(net_port.c_str());
        if (port <= 0 || port > 65535) {
            net_status = "Enter a port number between 1 and 65535.";
            net_failed = true;
            net_dirty();
            return;
        }
        if (!net_host && net_address.empty()) {
            net_status = "Enter the host's address.";
            net_failed = true;
            net_dirty();
            return;
        }
        net_save_settings();
        mt_net::SessionOptions options;
        options.host = net_host;
        options.address = net_address;
        options.port = port;
        options.delay = net_delay;
        options.pad = net_pad == 0 ? "" : net_pad == 1 ? "keyboard" : std::to_string(net_pad - 1);
        mt_net::begin_session(options);
        net_busy = true;
        net_failed = false;
        net_status = sentence(mt_net::session_status());
        net_dirty();
    }
}

static void start_artwork_fetch() {
    std::u8string stored = supported_games[0].game_id + u8".z64";
    mt_ss::start_fetch(zelda64::get_app_folder_path() / stored, "Mario Tennis (USA).z64");
}

// Called by the UI every frame before rendering, on the UI thread.
void recompui::launcher_frame_update() {
    if (net_busy) {
        std::string status = sentence(mt_net::session_status());
        if (status != net_status) {
            net_status = status;
            model_handle.DirtyVariable("net_status");
        }
        switch (mt_net::session_state()) {
            case mt_net::SessionState::Connected:
                net_busy = false;
                net_open = false;
                net_dirty();
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
                break;
            case mt_net::SessionState::Failed:
                net_busy = false;
                net_failed = true;
                net_dirty();
                break;
            case mt_net::SessionState::Idle:
                net_busy = false;
                net_dirty();
                break;
            case mt_net::SessionState::Connecting:
                break;
        }
    }

    mt_ss::Artwork art;
    if (!mt_ss::take_artwork(art)) {
        return;
    }
    // The linked launcher background wins over ScreenScraper's fanart; whichever arrives first shows until then.
    static bool linked_background_shown = false;
    if (!art.background_rgba.empty()) {
        static int generation = 0;
        std::string name = "?/launcher-background-" + std::to_string(++generation);
        recompui::queue_image_from_bytes_rgba32(name, art.background_rgba, art.background_width, art.background_height);
        ss_background_decorator = "image(/" + name + " cover center center)";
        ss_has_background = true;
        linked_background_shown = true;
        model_handle.DirtyVariable("ss_background_decorator");
        model_handle.DirtyVariable("ss_has_background");
    }
    else if (!art.background.empty() && !linked_background_shown) {
        recompui::queue_image_from_bytes_file("?/ss-background", art.background);
        // Set only now, so the texture is first looked up after its bytes are queued. "cover" fills the window.
        ss_background_decorator = "image(/?/ss-background cover center center)";
        ss_has_background = true;
        model_handle.DirtyVariable("ss_background_decorator");
        model_handle.DirtyVariable("ss_has_background");
    }
    if (!art.logo.empty()) {
        recompui::queue_image_from_bytes_file("?/ss-logo", art.logo);
        ss_logo_src = "?/ss-logo";
        ss_has_logo = true;
        model_handle.DirtyVariable("ss_logo_src");
        model_handle.DirtyVariable("ss_has_logo");
    }
}

void select_rom() {
    nfdnchar_t* native_path = nullptr;
    zelda64::open_file_dialog([](bool success, const std::filesystem::path& path) {
        if (success) {
            recomp::RomValidationError rom_error = recomp::select_rom(path, supported_games[0].game_id);
            switch (rom_error) {
                case recomp::RomValidationError::Good:
                    mm_rom_valid = true;
                    model_handle.DirtyVariable("mm_rom_valid");
                    start_artwork_fetch();
                    break;
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    break;
                case recomp::RomValidationError::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    break;
                case recomp::RomValidationError::IncorrectRom:
                    recompui::message_box("This ROM is not the correct game.");
                    break;
                case recomp::RomValidationError::NotYet:
                    recompui::message_box("This game isn't supported yet.");
                    break;
                case recomp::RomValidationError::IncorrectVersion:
                    recompui::message_box(
                            "This ROM is the correct game, but the wrong version.\nThis project requires the NTSC-U N64 version of the game.");
                    break;
                case recomp::RomValidationError::OtherError:
                    recompui::message_box("An unknown error has occurred.");
                    break;
            }
        }
    });
}

recompui::ContextId launcher_context;

recompui::ContextId recompui::get_launcher_context_id() {
	return launcher_context;
}

class LauncherMenu : public recompui::MenuController {
public:
    LauncherMenu() {
        mm_rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
        net_load_settings();
        mt_ss::start_background_fetch();
        if (mm_rom_valid) {
            start_artwork_fetch();
        }
    }
    ~LauncherMenu() override {

    }
    void load_document() override {
		launcher_context = recompui::create_context(zelda64::get_asset_path("launcher.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "select_rom",
            [](const std::string& param, Rml::Event& event) {
                select_rom();
            }
        );
        recompui::register_event(listener, "rom_selected",
            [](const std::string& param, Rml::Event& event) {
                mm_rom_valid = true;
                model_handle.DirtyVariable("mm_rom_valid");
                start_artwork_fetch();
            }
        );
        recompui::register_event(listener, "start_game",
            [](const std::string& param, Rml::Event& event) {
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
            }
        );
        recompui::register_event(listener, "open_controls",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::Controls);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_settings",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::General);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_mods",
            [](const std::string &param, Rml::Event &event) {
                recompui::set_config_tab(recompui::ConfigTab::Mods);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_online",
            [](const std::string& param, Rml::Event& event) {
                net_open = true;
                net_status.clear();
                net_failed = false;
                net_dirty();
            }
        );
        recompui::register_event(listener, "close_online",
            [](const std::string& param, Rml::Event& event) {
                if (net_busy) mt_net::cancel_session();
                net_busy = false;
                net_open = false;
                net_dirty();
            }
        );
        recompui::register_event(listener, "net_set_host",
            [](const std::string& param, Rml::Event& event) {
                if (net_busy) return;
                net_host = true;
                net_status.clear();
                net_failed = false;
                net_dirty();
            }
        );
        recompui::register_event(listener, "net_set_join",
            [](const std::string& param, Rml::Event& event) {
                if (net_busy) return;
                net_host = false;
                net_status.clear();
                net_failed = false;
                net_dirty();
            }
        );
        recompui::register_event(listener, "net_delay_down",
            [](const std::string& param, Rml::Event& event) {
                net_delay = std::max(0, net_delay - 1);
                model_handle.DirtyVariable("net_delay");
            }
        );
        recompui::register_event(listener, "net_delay_up",
            [](const std::string& param, Rml::Event& event) {
                net_delay = std::min(10, net_delay + 1);
                model_handle.DirtyVariable("net_delay");
            }
        );
        recompui::register_event(listener, "net_pad_next",
            [](const std::string& param, Rml::Event& event) {
                net_pad = (net_pad + 1) % 6;
                net_update_pad_label();
                model_handle.DirtyVariable("net_pad_label");
            }
        );
        recompui::register_event(listener, "net_start",
            [](const std::string& param, Rml::Event& event) {
                if (!net_busy) net_start();
            }
        );
        recompui::register_event(listener, "net_cancel",
            [](const std::string& param, Rml::Event& event) {
                mt_net::cancel_session();
                net_busy = false;
                net_status = "Cancelled.";
                net_dirty();
            }
        );
        recompui::register_event(listener, "exit_game",
            [](const std::string& param, Rml::Event& event) {
                ultramodern::quit();
            }
        );
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");

        constructor.Bind("mm_rom_valid", &mm_rom_valid);
        constructor.Bind("ss_background_decorator", &ss_background_decorator);
        constructor.Bind("ss_logo_src", &ss_logo_src);
        constructor.Bind("ss_has_background", &ss_has_background);
        constructor.Bind("ss_has_logo", &ss_has_logo);

        constructor.Bind("net_open", &net_open);
        constructor.Bind("net_host", &net_host);
        constructor.Bind("net_address", &net_address);
        constructor.Bind("net_port", &net_port);
        constructor.Bind("net_delay", &net_delay);
        constructor.Bind("net_pad_label", &net_pad_label);
        constructor.Bind("net_status", &net_status);
        constructor.Bind("net_busy", &net_busy);
        constructor.Bind("net_failed", &net_failed);

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
