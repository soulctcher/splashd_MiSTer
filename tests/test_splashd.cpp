#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "src/splashd.cpp"

namespace {

std::string make_temp_dir() {
    char tmpl[] = "/tmp/splashd-test-XXXXXX";
    char* path = mkdtemp(tmpl);
    assert(path);
    return path;
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    assert(f);
    assert(std::fwrite(data.data(), 1, data.size(), f) == data.size());
    std::fclose(f);
}

void write_text(const std::string& path, const std::string& text) {
    std::vector<uint8_t> data(text.begin(), text.end());
    write_file(path, data);
}

std::vector<uint8_t> png_2x2_rgba() {
    return {137, 80, 78,  71,  13,  10,  26,  10,  0,   0,   0,   13,  73,  72, 68, 82,  0,  0,  0,
            2,   0,  0,   0,   2,   8,   6,   0,   0,   0,   114, 182, 13,  36, 0,  0,   0,  19, 73,
            68,  65, 84,  120, 156, 99,  248, 207, 192, 240, 31,  12,  129, 52, 8,  52,  0,  0,  73,
            73,  9,  120, 40,  160, 219, 119, 0,   0,   0,   0,   73,  69,  78, 68, 174, 66, 96, 130};
}

void test_parse_mode() {
    Mode mode;
    assert(parse_mode_text("8888 1 960 540 3840\n", &mode));
    assert(mode.width == 960);
    assert(mode.height == 540);
    assert(mode.stride == 3840);
    assert(!parse_mode_text("565 1 960 540 1920\n", &mode));
    assert(!parse_mode_text("8888 1 0 540 3840\n", &mode));
}

void test_matching() {
    std::string dir = make_temp_dir();
    write_text(join_path(dir, "chrono trigger.PNG"), "not a real png");
    write_text(join_path(dir, "Displayed Name.JPG"), "not a real jpg");
    write_text(join_path(dir, "Alien vs. Predator.png"), "not a real png");
    write_text(join_path(dir, "Arkanoid.JPEG"), "not a real jpeg");
    write_text(join_path(dir, "Exact (USA).png"), "not a real png");
    write_text(join_path(dir, "Exact (USA).jpg"), "not a real jpg");
    write_text(join_path(dir, "_Arcade.jpg"), "not a real jpg");
    write_text(join_path(dir, "alternatives.jpeg"), "not a real jpeg");
    write_text(join_path(dir, "D. D. Crew.png"), "not a real png");
    write_text(join_path(dir, "Gun.Smoke.png"), "not a real png");
    write_text(join_path(dir, "avsp.png"), "not a real png");
    write_text(join_path(dir, "mslug.png"), "not a real png");
    write_text(join_path(dir, "1on1gov.png"), "not a real png");
    const std::string mra_path = join_path(dir, "Alien vs. Predator (Euro 940520).mra");
    const std::string mgl_path = join_path(dir, "Metal Slug.mgl");
    const std::string root_mra_path = join_path(dir, "1 on 1 Government (Japan).mra");
    write_text(mra_path, "<misterromdescription><setname>avsp</setname></misterromdescription>");
    write_text(mgl_path,
               "<mistergamedescription><file path=\"/media/fat/games/NeoGeo/mslug.neo\"/></mistergamedescription>");
    write_text(root_mra_path, "<misterromdescription><setname>1on1gov</setname></misterromdescription>");

    std::string p = resolve_wallpaper(dir, "/media/fat/games/SNES/Chrono Trigger.sfc", "Displayed Name");
    assert(basename_of(p) == "chrono trigger.PNG");

    p = resolve_wallpaper(dir, "/media/fat/games/SNES/Missing.sfc", "Displayed Name");
    assert(basename_of(p) == "Displayed Name.JPG");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade/Alien vs. Predator (Euro 940520).mra", "");
    assert(basename_of(p) == "Alien vs. Predator.png");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade/Arkanoid (Unl. lives) [hb].mra", "");
    assert(basename_of(p) == "Arkanoid.JPEG");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade/Exact (USA).mra", "");
    assert(basename_of(p) == "Exact (USA).png");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade", "");
    assert(basename_of(p) == "_Arcade.jpg");

    p = resolve_wallpaper(dir, "_Arcade/_alternatives", "");
    assert(basename_of(p) == "alternatives.jpeg");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade/_D.D. Crew", "");
    assert(basename_of(p) == "D. D. Crew.png");

    p = resolve_wallpaper(dir, "", "D.D. Crew (Japan, 2 Players) (FD1094 317-0182)");
    assert(basename_of(p) == "D. D. Crew.png");

    p = resolve_wallpaper(dir, "/media/fat/_Arcade/_alternatives", "_Gun.Smoke");
    assert(basename_of(p) == "Gun.Smoke.png");

    p = resolve_wallpaper(dir, mra_path, "");
    assert(basename_of(p) == "avsp.png");

    p = resolve_wallpaper(dir, mgl_path, "");
    assert(basename_of(p) == "mslug.png");

    p = resolve_wallpaper(dir, "", "1 on 1 Government (Japan)", dir);
    assert(basename_of(p) == "1on1gov.png");

    assert(strip_extension("_D.D. Crew") == "_D.D. Crew");
    assert(strip_extension("D.D. Crew (Japan, 2 Players) (FD1094 317-0182)") ==
           "D.D. Crew (Japan, 2 Players) (FD1094 317-0182)");
    assert(strip_extension("_Gun.Smoke") == "_Gun.Smoke");
    assert(strip_extension("Chrono Trigger.sfc") == "Chrono Trigger");
    assert(normalize_arcade_title("Action Fighter (World, S16A) [FD1089A 317-0018]") == "Action Fighter");
    assert(normalize_arcade_title("1941  Counter Attack -World 900227-") == "1941 Counter Attack -World 900227-");
    assert(strip_menu_directory_prefix("_Arcade") == "Arcade");
    assert(strip_menu_directory_prefix("Arcade") == "Arcade");
    assert(artwork_match_key("D.D. Crew") == artwork_match_key("D. D. Crew.png"));
    assert(rom_stem_from_menu_file(mra_path) == "avsp");
    assert(rom_stem_from_menu_file(mgl_path) == "mslug");
    assert(resolve_menu_file_path(dir, "", "1 on 1 Government (Japan)") == root_mra_path);
}

void test_default_image_resolution() {
    std::string dir = make_temp_dir();
    std::string path = join_path(dir, "menu.png");
    write_file(path, png_2x2_rgba());

    Options opts;
    opts.default_image = path;
    assert(resolve_default_image(opts) == path);

    opts.default_image = join_path(dir, "missing.jpg");
    assert(resolve_default_image(opts).empty());
}

void test_png_decode_and_render() {
    std::string dir = make_temp_dir();
    std::string path = join_path(dir, "sample.png");
    write_file(path, png_2x2_rgba());

    Image image;
    std::string error;
    assert(decode_png_file(path, &image, &error));
    assert(image.width == 2);
    assert(image.height == 2);

    Mode mode{8888, 1, 4, 4, 16};
    std::vector<uint32_t> frame = render_to_frame(image, mode);
    assert(frame.size() == 16);
    assert(frame[0] == 0x00ff0000);
    assert(frame[1] == 0x00ff0000);
    assert(frame[2] == 0x0000ff00);
    assert(frame[8] == 0x000000ff);
    assert(frame[10] == 0x00808080);
}

void test_black_clear_to_slots() {
    Mode mode{8888, 1, 2, 2, 8};
    std::vector<uint32_t> black(4, 0);
    std::vector<uint8_t> memory(kFramebufferMapBytes, 0xff);

    write_frame_to_slot(memory.data(), kFramebufferSlot1, mode, black);
    write_frame_to_slot(memory.data(), kFramebufferSlot2, mode, black);
    write_frame_to_slot(memory.data(), kFramebufferSlot0Offset, mode, black);

    for (size_t i = 0; i < 16; ++i) {
        assert(memory[kFramebufferSlot1 + i] == 0);
        assert(memory[kFramebufferSlot2 + i] == 0);
        assert(memory[kFramebufferSlot0Offset + i] == 0);
    }
}

} // namespace

int main() {
    test_parse_mode();
    test_matching();
    test_default_image_resolution();
    test_png_decode_and_render();
    test_black_clear_to_slots();
    std::puts("all tests passed");
    return 0;
}
