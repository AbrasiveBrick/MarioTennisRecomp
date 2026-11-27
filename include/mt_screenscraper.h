#ifndef MT_SCREENSCRAPER_H
#define MT_SCREENSCRAPER_H

#include <filesystem>
#include <string>
#include <vector>

// Optional launcher artwork fetched from ScreenScraper at runtime and kept in memory only.
namespace mt_ss {
    struct Artwork {
        std::vector<char> background; // PNG bytes of the game's fanart, empty if none
        std::vector<char> logo;       // PNG bytes of the game's logo ("wheel"), empty if none
    };

    // Starts the lookup in the background (once). Does nothing without credentials or libcurl.
    void start_fetch(const std::filesystem::path& stored_rom_path, const std::string& rom_name);

    // Hands over the artwork once, when the lookup has finished with something to show.
    bool take_artwork(Artwork& out);
}

#endif
