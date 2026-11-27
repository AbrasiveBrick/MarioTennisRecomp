#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#include "nfd.h"
#include "mt_screenscraper.h"
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

static void start_artwork_fetch() {
    std::u8string stored = supported_games[0].game_id + u8".z64";
    mt_ss::start_fetch(zelda64::get_app_folder_path() / stored, "Mario Tennis (USA).z64");
}

// Called by the UI every frame before rendering, on the UI thread.
void recompui::launcher_frame_update() {
    mt_ss::Artwork art;
    if (!mt_ss::take_artwork(art)) {
        return;
    }
    if (!art.background.empty()) {
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

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
