// Mario Tennis recomp: optional launcher artwork from ScreenScraper (https://www.screenscraper.fr).
//
// At launch, if ScreenScraper developer credentials are available, a background thread looks the
// stored ROM up with jeuInfos.php (CRC32 + size) and downloads the game's fanart and logo ("wheel")
// as PNG. The bytes only ever live in memory: they are handed to the UI's image queue and never
// written to disk. If anything is missing (no credentials, no network, no ROM yet, no media, an API
// error), nothing changes and the launcher keeps its built-in court and title.
//
// Credentials, in order of precedence:
//   - environment variables MT_SS_DEVID, MT_SS_DEVPASSWORD (and optionally MT_SS_USER, MT_SS_PASSWORD
//     for a ScreenScraper member account, which raises the request quota);
//   - values baked in at build time with -DMT_SS_DEVID=... -DMT_SS_DEVPASSWORD=... (never committed).
// Set MT_SS_DISABLE=1 to turn the lookup off. MT_SS_API replaces the API base URL (for testing).
//
// Launcher background: the official Mario Tennis artwork is loaded from Nintendo's site by link
// (launcher_background_url below) on every launch, decoded in memory and never saved, so no copyrighted
// image is in the repository. It needs no credentials and wins over ScreenScraper's fanart.
// MT_LAUNCHER_BG_URL replaces the link (for testing); MT_LAUNCHER_BG_URL=none turns it off.

#include "mt_screenscraper.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef MT_HAVE_CURL
#include <curl/curl.h>
#include "json/json.hpp"
#include "../../lib/rt64/src/contrib/stb/stb_image.h"
#endif

#ifndef MT_SS_DEVID_DEFAULT
#define MT_SS_DEVID_DEFAULT ""
#endif
#ifndef MT_SS_DEVPASSWORD_DEFAULT
#define MT_SS_DEVPASSWORD_DEFAULT ""
#endif

namespace {

std::mutex result_mutex;
mt_ss::Artwork result;
std::atomic<bool> result_ready{ false };
std::atomic<bool> started{ false };
std::atomic<bool> background_started{ false };

constexpr const char* launcher_background_url =
    "https://www.nintendo.com/eu/media/images/10_share_images/games_15/nintendo_7/SI_N64_MarioTennis_image1600w.jpg";

// Merges a finished job's images into the pending result.
void publish(mt_ss::Artwork&& art) {
    std::lock_guard lock{ result_mutex };
    if (!art.background.empty()) {
        result.background = std::move(art.background);
    }
    if (!art.background_rgba.empty()) {
        result.background_rgba = std::move(art.background_rgba);
        result.background_width = art.background_width;
        result.background_height = art.background_height;
    }
    if (!art.logo.empty()) {
        result.logo = std::move(art.logo);
    }
    result_ready.store(true);
}

#ifdef MT_HAVE_CURL

constexpr int nintendo_64_system_id = 14;
constexpr const char* soft_name = "MarioTennisRecompiled";

std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string{ v } : std::string{ fallback };
}

uint32_t crc32(const std::vector<char>& data) {
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (char ch : data) {
        crc = table[(crc ^ static_cast<uint8_t>(ch)) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

size_t write_to_vector(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<char>*>(userdata);
    out->insert(out->end(), ptr, ptr + size * nmemb);
    return size * nmemb;
}

// GET a URL into memory. Returns false on transport errors or a non-200 status.
bool http_get(CURL* curl, const std::string& url, std::vector<char>& out, long* status_out = nullptr) {
    out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vector);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, soft_name);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status_out) {
        *status_out = status;
    }
    return rc == CURLE_OK && status == 200;
}

std::string escape(CURL* curl, const std::string& s) {
    char* e = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string r = e ? e : "";
    curl_free(e);
    return r;
}

// Picks the best media URL of a type, preferring the US/world regions.
std::string pick_media(const nlohmann::json& medias, const char* type) {
    static const char* regions[] = { "us", "wor", "ss", "eu", "jp", "" };
    for (const char* region : regions) {
        for (const auto& m : medias) {
            if (!m.is_object() || m.value("type", "") != type) {
                continue;
            }
            std::string r = m.value("region", "");
            if (region[0] == '\0' || r == region) {
                std::string url = m.value("url", "");
                if (!url.empty()) {
                    return url;
                }
            }
        }
    }
    return {};
}

bool download_png(CURL* curl, std::string url, int max_width, std::vector<char>& out) {
    if (url.empty()) {
        return false;
    }
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += "outputformat=png&maxwidth=" + std::to_string(max_width);
    if (!http_get(curl, url, out)) {
        return false;
    }
    // Anything that isn't a PNG (e.g. "NOMEDIA") is ignored.
    static const unsigned char png_sig[] = { 0x89, 'P', 'N', 'G' };
    return out.size() > 8 && std::equal(png_sig, png_sig + 4, reinterpret_cast<const unsigned char*>(out.data()));
}

void fetch(std::filesystem::path rom_path, std::string rom_name) {
    std::string devid = env_or("MT_SS_DEVID", MT_SS_DEVID_DEFAULT);
    std::string devpassword = env_or("MT_SS_DEVPASSWORD", MT_SS_DEVPASSWORD_DEFAULT);
    std::string ssid = env_or("MT_SS_USER", "");
    std::string sspassword = env_or("MT_SS_PASSWORD", "");

    std::vector<char> rom;
    {
        std::ifstream f{ rom_path, std::ios::binary };
        if (!f) {
            return;
        }
        rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (rom.empty()) {
        return;
    }
    char crc_hex[9];
    std::snprintf(crc_hex, sizeof(crc_hex), "%08X", crc32(rom));
    size_t rom_size = rom.size();
    rom.clear();
    rom.shrink_to_fit();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return;
    }

    // MT_SS_API points the lookup at another server, for testing the launcher without ScreenScraper.
    std::string url = env_or("MT_SS_API", "https://api.screenscraper.fr/api2") + "/jeuInfos.php?output=json";
    url += "&devid=" + escape(curl, devid);
    url += "&devpassword=" + escape(curl, devpassword);
    url += "&softname=" + escape(curl, soft_name);
    if (!ssid.empty()) {
        url += "&ssid=" + escape(curl, ssid) + "&sspassword=" + escape(curl, sspassword);
    }
    url += "&systemeid=" + std::to_string(nintendo_64_system_id);
    url += "&romtype=rom&crc=" + std::string{ crc_hex };
    url += "&romtaille=" + std::to_string(rom_size);
    url += "&romnom=" + escape(curl, rom_name);

    std::vector<char> body;
    long status = 0;
    if (!http_get(curl, url, body, &status)) {
        std::fprintf(stderr, "[mt] ScreenScraper lookup failed (HTTP %ld); using the built-in launcher art.\n", status);
        curl_easy_cleanup(curl);
        return;
    }

    mt_ss::Artwork art;
    try {
        nlohmann::json j = nlohmann::json::parse(body.begin(), body.end());
        const nlohmann::json& medias = j.at("response").at("jeu").at("medias");
        if (!download_png(curl, pick_media(medias, "fanart"), 1920, art.background)) {
            art.background.clear();
        }
        std::string logo_url = pick_media(medias, "wheel-hd");
        if (logo_url.empty()) {
            logo_url = pick_media(medias, "wheel");
        }
        if (!download_png(curl, logo_url, 960, art.logo)) {
            art.logo.clear();
        }
    }
    catch (const std::exception&) {
        std::fprintf(stderr, "[mt] ScreenScraper answer not understood; using the built-in launcher art.\n");
    }
    curl_easy_cleanup(curl);

    if (art.background.empty() && art.logo.empty()) {
        return;
    }
    std::fprintf(stderr, "[mt] ScreenScraper artwork loaded (background: %s, logo: %s).\n",
        art.background.empty() ? "no" : "yes", art.logo.empty() ? "no" : "yes");
    publish(std::move(art));
}

// Downloads the launcher background by link and decodes it (JPEG or PNG) to RGBA in memory.
void fetch_background(std::string url) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return;
    }
    std::vector<char> bytes;
    long status = 0;
    bool ok = http_get(curl, url, bytes, &status);
    curl_easy_cleanup(curl);
    if (!ok) {
        std::fprintf(stderr, "[mt] Launcher background download failed (HTTP %ld); using the built-in court.\n", status);
        return;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()),
        &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        std::fprintf(stderr, "[mt] Launcher background could not be decoded; using the built-in court.\n");
        if (pixels) {
            stbi_image_free(pixels);
        }
        return;
    }
    mt_ss::Artwork art;
    art.background_rgba.assign(reinterpret_cast<char*>(pixels), reinterpret_cast<char*>(pixels) + size_t(width) * height * 4);
    art.background_width = static_cast<uint32_t>(width);
    art.background_height = static_cast<uint32_t>(height);
    stbi_image_free(pixels);
    std::fprintf(stderr, "[mt] Launcher background loaded (%dx%d).\n", width, height);
    publish(std::move(art));
}

#endif // MT_HAVE_CURL

} // namespace

void mt_ss::start_fetch(const std::filesystem::path& stored_rom_path, const std::string& rom_name) {
#ifdef MT_HAVE_CURL
    if (started.exchange(true)) {
        return;
    }
    if (env_or("MT_SS_DISABLE", "") == "1") {
        return;
    }
    if (env_or("MT_SS_DEVID", MT_SS_DEVID_DEFAULT).empty() || env_or("MT_SS_DEVPASSWORD", MT_SS_DEVPASSWORD_DEFAULT).empty()) {
        return;
    }
    static bool curl_ready = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    if (!curl_ready) {
        return;
    }
    std::thread{ fetch, stored_rom_path, rom_name }.detach();
#else
    (void)stored_rom_path;
    (void)rom_name;
#endif
}

void mt_ss::start_background_fetch() {
#ifdef MT_HAVE_CURL
    if (background_started.exchange(true)) {
        return;
    }
    std::string url = env_or("MT_LAUNCHER_BG_URL", launcher_background_url);
    if (url == "none") {
        return;
    }
    static bool curl_ready = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    if (!curl_ready) {
        return;
    }
    std::thread{ fetch_background, url }.detach();
#endif
}

bool mt_ss::take_artwork(Artwork& out) {
    if (!result_ready.exchange(false)) {
        return false;
    }
    std::lock_guard lock{ result_mutex };
    out = std::move(result);
    result = {};
    return true;
}
