// Bonus characters: Shy Guy and Donkey Kong Jr. are normally unlocked by winning Star Cup tournaments
// (singles and doubles). The game keeps their unlock bits in one byte of its save data in RAM,
// 0x800653C8 (bit 0 = DK Jr., bit 1 = Shy Guy; the GameShark code "Have D.K. Jr. & Shy Guy" is
// 800653C8 0003). Setting both bits every frame makes the two characters always selectable; the game
// then also writes them to the save like any other unlock.
#include <cstdint>

extern "C" void mt_bonus_characters(uint8_t* rdram) {
    // RDRAM is stored as native-endian 32-bit words, so byte addresses are XORed with 3.
    uint8_t& unlocks = rdram[0x653C8 ^ 3];
    unlocks |= 0x03;
}
