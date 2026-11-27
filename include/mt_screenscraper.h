#ifndef MT_SCREENSCRAPER_H
#define MT_SCREENSCRAPER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Optional launcher artwork fetched from ScreenScraper at runtime and kept in memory only.
namespace mt_ss {
    struct Artwork {
        std::vector<char> background; // PNG bytes of the game's fanart, empty if none
        std::vector<char> logo;       // PNG bytes of the game's logo ("wheel"), empty if none
        std::vector<char> background_rgba; // decoded launcher background from its link, empty if none
        uint32_t background_width = 0;
        uint32_t background_height = 0;
    };

    // Starts loading the launcher background from its link in the background (once). No credentials needed.
    void start_background_fetch();

    // Starts the lookup in the background (once). Does nothing without credentials or libcurl.
    void start_fetch(const std::filesystem::path& stored_rom_path, const std::string& rom_name);

    // Hands over whatever artwork has arrived since the last call.
    bool take_artwork(Artwork& out);
}

#endif
