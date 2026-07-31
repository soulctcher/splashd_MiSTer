#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <turbojpeg.h>
#include <zlib.h>

namespace {

constexpr uint32_t kFramebufferAddr = 0x22000000;
constexpr size_t kFramebufferPixels = 1920u * 1080u;
constexpr size_t kFramebufferBytes = kFramebufferPixels * sizeof(uint32_t);
constexpr size_t kFramebufferSlot0Offset = 4096;
constexpr size_t kFramebufferSlot1 = kFramebufferBytes;
constexpr size_t kFramebufferSlot2 = kFramebufferBytes * 2;
constexpr size_t kFramebufferMapBytes = kFramebufferBytes * 3;
constexpr const char* kMisterCmdPath = "/dev/MiSTer_cmd";

volatile sig_atomic_t g_stop = 0;

[[maybe_unused]] void on_signal(int) {
    g_stop = 1;
}

struct Options {
    std::string wallpaper_dir = "/media/fat/splashd/wallpapers";
    std::string default_image;
    std::string menu_root = "/media/fat";
    std::string mode_file = "/sys/module/MiSTer_fb/parameters/mode";
    std::string watch_dir = "/tmp";
    bool foreground = false;
    bool once = false;
};

struct Mode {
    int fmt = 0;
    int rb = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

struct LastFrame {
    Mode mode;
    std::vector<uint32_t> frame;
    bool valid = false;
};

std::string join_path(const std::string& dir, const std::string& name);
std::string read_text_file(const std::string& path);

struct TmpState {
    std::string file_select;
    std::string fullpath;
    std::string currentpath;
    std::string osd_visible;
};

TmpState read_tmp_state(const Options& opts) {
    TmpState state;
    state.file_select = read_text_file(join_path(opts.watch_dir, "FILESELECT"));
    state.fullpath = read_text_file(join_path(opts.watch_dir, "FULLPATH"));
    state.currentpath = read_text_file(join_path(opts.watch_dir, "CURRENTPATH"));
    state.osd_visible = read_text_file(join_path(opts.watch_dir, "OSD_VISIBLE"));
    return state;
}

bool state_changed(const TmpState& a, const TmpState& b) {
    return a.file_select != b.file_select || a.fullpath != b.fullpath || a.currentpath != b.currentpath ||
           a.osd_visible != b.osd_visible;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "/") return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string read_text_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[512];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n) out.append(buf, n);
        if (n < sizeof(buf)) break;
    }
    std::fclose(f);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

std::string lowercase_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ends_with_case_insensitive(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return lowercase_ascii(s.substr(s.size() - suffix.size())) == lowercase_ascii(suffix);
}

bool has_supported_image_extension(const std::string& name) {
    return ends_with_case_insensitive(name, ".png") || ends_with_case_insensitive(name, ".jpg") ||
           ends_with_case_insensitive(name, ".jpeg");
}

bool has_known_content_extension(const std::string& name) {
    static const char* exts[] = {
        ".mra", ".mgl", ".neo", ".rom", ".zip", ".7z",  ".nes", ".fds", ".sfc", ".smc", ".gb",  ".gbc", ".gba", ".gen",
        ".md",  ".smd", ".sms", ".gg",  ".sg",  ".pce", ".sgx", ".a26", ".a52", ".a78", ".col", ".int", ".vec", ".dsk",
        ".adf", ".hdf", ".st",  ".msa", ".ipf", ".bin", ".cue", ".chd", ".iso", ".img", ".ccd", ".sub", ".vhd",
    };
    for (const char* ext : exts) {
        if (ends_with_case_insensitive(name, ext)) return true;
    }
    return has_supported_image_extension(name);
}

std::string basename_of(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string strip_extension(const std::string& name) {
    std::string base = basename_of(name);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return base;
    if (!has_known_content_extension(base)) return base;
    return base.substr(0, dot);
}

std::string trim_ascii_spaces(std::string s) {
    size_t first = 0;
    while (first < s.size() && (s[first] == ' ' || s[first] == '\t'))
        ++first;
    size_t last = s.size();
    while (last > first && (s[last - 1] == ' ' || s[last - 1] == '\t'))
        --last;
    return s.substr(first, last - first);
}

std::string normalize_arcade_title(std::string stem) {
    stem = trim_ascii_spaces(stem);

    size_t cut = std::string::npos;
    for (size_t i = 0; i < stem.size(); ++i) {
        if (stem[i] == '(' || stem[i] == '[') {
            cut = i;
            break;
        }
    }

    if (cut != std::string::npos) stem = trim_ascii_spaces(stem.substr(0, cut));

    while (stem.find("  ") != std::string::npos) {
        size_t pos = stem.find("  ");
        stem.replace(pos, 2, " ");
    }

    return stem;
}

std::string strip_menu_directory_prefix(const std::string& stem) {
    if (stem.size() > 1 && stem[0] == '_') return stem.substr(1);
    return stem;
}

std::string artwork_match_key(const std::string& name) {
    std::string stem = strip_extension(name);
    stem = strip_menu_directory_prefix(stem);

    std::string key;
    key.reserve(stem.size());
    for (char c : stem) {
        if (c >= 'A' && c <= 'Z') {
            key.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            key.push_back(c);
        }
    }
    return key;
}

void add_wallpaper_candidate(std::vector<std::string>* candidates, const std::string& stem) {
    if (!candidates || stem.empty()) return;
    for (const std::string& existing : *candidates) {
        if (existing == stem) return;
    }
    candidates->push_back(stem);
}

std::vector<std::string> wallpaper_candidates(const std::string& stem) {
    std::vector<std::string> candidates;
    add_wallpaper_candidate(&candidates, stem);
    add_wallpaper_candidate(&candidates, strip_menu_directory_prefix(stem));
    add_wallpaper_candidate(&candidates, normalize_arcade_title(stem));
    add_wallpaper_candidate(&candidates, normalize_arcade_title(strip_menu_directory_prefix(stem)));
    return candidates;
}

std::string xml_tag_value(const std::string& text, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t start = text.find(open);
    if (start == std::string::npos) return {};
    start += open.size();
    size_t end = text.find(close, start);
    if (end == std::string::npos) return {};
    return trim_ascii_spaces(text.substr(start, end - start));
}

std::string xml_attribute_value(const std::string& element, const std::string& attribute) {
    size_t pos = 0;
    while ((pos = element.find(attribute, pos)) != std::string::npos) {
        const bool left_boundary =
            pos == 0 || element[pos - 1] == ' ' || element[pos - 1] == '\t' || element[pos - 1] == '\n';
        size_t equals = pos + attribute.size();
        while (equals < element.size() && (element[equals] == ' ' || element[equals] == '\t'))
            ++equals;
        if (!left_boundary || equals >= element.size() || element[equals] != '=') {
            pos += attribute.size();
            continue;
        }
        ++equals;
        while (equals < element.size() && (element[equals] == ' ' || element[equals] == '\t'))
            ++equals;
        if (equals >= element.size() || (element[equals] != '"' && element[equals] != '\'')) return {};
        const char quote = element[equals++];
        size_t end = element.find(quote, equals);
        if (end == std::string::npos) return {};
        return element.substr(equals, end - equals);
    }
    return {};
}

std::string rom_stem_from_menu_file(const std::string& path) {
    if (ends_with_case_insensitive(path, ".mra")) {
        return xml_tag_value(read_text_file(path), "setname");
    }
    if (!ends_with_case_insensitive(path, ".mgl")) return {};

    const std::string text = read_text_file(path);
    std::string fallback;
    size_t pos = 0;
    while ((pos = text.find("<file", pos)) != std::string::npos) {
        size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        const std::string file_path = xml_attribute_value(text.substr(pos, end - pos + 1), "path");
        if (!file_path.empty()) {
            const std::string stem = strip_extension(file_path);
            if (ends_with_case_insensitive(file_path, ".neo")) return stem;
            if (fallback.empty() && has_known_content_extension(file_path)) fallback = stem;
        }
        pos = end + 1;
    }
    return fallback;
}

bool is_menu_directory_stem(const std::string& stem) {
    return stem.size() > 1 && stem[0] == '_' && !has_known_content_extension(stem);
}

bool parse_mode_text(const std::string& text, Mode* mode) {
    if (!mode) return false;
    Mode parsed;
    char extra = 0;
    if (std::sscanf(text.c_str(), "%d %d %d %d %d %c", &parsed.fmt, &parsed.rb, &parsed.width, &parsed.height,
                    &parsed.stride, &extra) != 5) {
        return false;
    }
    if (parsed.fmt != 8888) return false;
    if (parsed.width <= 0 || parsed.height <= 0) return false;
    if (parsed.width > 1920 || parsed.height > 1080) return false;
    if (parsed.stride < parsed.width * 4) return false;
    if (parsed.stride > 1920 * 4) return false;
    if (parsed.stride % 4 != 0) return false;
    if (static_cast<size_t>(parsed.stride) * parsed.height > kFramebufferBytes) return false;
    *mode = parsed;
    return true;
}

bool read_mode_file(const std::string& path, Mode* mode) {
    return parse_mode_text(read_text_file(path), mode);
}

std::string find_case_insensitive_image(const std::string& dir, const std::string& stem) {
    if (stem.empty()) return {};
    const std::vector<std::string> targets = {
        lowercase_ascii(stem + ".png"),
        lowercase_ascii(stem + ".jpg"),
        lowercase_ascii(stem + ".jpeg"),
    };
    DIR* d = opendir(dir.c_str());
    if (!d) return {};

    std::string result;
    for (const std::string& target : targets) {
        rewinddir(d);
        while (dirent* de = readdir(d)) {
            if (de->d_name[0] == '.') continue;
            if (lowercase_ascii(de->d_name) == target) {
                result = join_path(dir, de->d_name);
                break;
            }
        }
        if (!result.empty()) break;
    }
    closedir(d);
    return result;
}

std::string find_fuzzy_image(const std::string& dir, const std::string& stem) {
    const std::string target = artwork_match_key(stem);
    if (target.empty()) return {};

    DIR* d = opendir(dir.c_str());
    if (!d) return {};

    std::string result;
    const std::vector<std::string> extensions = {".png", ".jpg", ".jpeg"};
    for (const std::string& extension : extensions) {
        rewinddir(d);
        while (dirent* de = readdir(d)) {
            if (de->d_name[0] == '.') continue;
            const std::string filename = de->d_name;
            if (!ends_with_case_insensitive(filename, extension)) continue;
            if (!has_supported_image_extension(filename)) continue;
            if (artwork_match_key(filename) == target) {
                result = join_path(dir, filename);
                break;
            }
        }
        if (!result.empty()) break;
    }

    closedir(d);
    return result;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string resolve_menu_file_path(const std::string& menu_root, const std::string& fullpath,
                                   const std::string& currentpath) {
    const std::string full_base = basename_of(fullpath);
    if (has_known_content_extension(full_base) && file_exists(fullpath)) return fullpath;

    const std::string current_base = basename_of(currentpath);
    if (current_base.empty() || is_menu_directory_stem(current_base)) return {};

    if (has_known_content_extension(current_base)) {
        const std::string path = join_path(menu_root, current_base);
        return file_exists(path) ? path : "";
    }

    const std::string mra = join_path(menu_root, current_base + ".mra");
    if (file_exists(mra)) return mra;

    const std::string mgl = join_path(menu_root, current_base + ".mgl");
    if (file_exists(mgl)) return mgl;

    return {};
}

std::string resolve_default_image(const Options& opts) {
    if (!opts.default_image.empty()) return file_exists(opts.default_image) ? opts.default_image : "";
    if (file_exists("/media/fat/menu.png")) return "/media/fat/menu.png";
    if (file_exists("/media/fat/menu.jpg")) return "/media/fat/menu.jpg";
    if (file_exists("/media/fat/menu.jpeg")) return "/media/fat/menu.jpeg";
    return "";
}

std::string resolve_wallpaper(const std::string& wallpaper_dir, const std::string& fullpath,
                              const std::string& currentpath, const std::string& menu_root = "/media/fat") {
    const std::string full_stem = strip_extension(fullpath);
    const std::string current_stem = strip_extension(currentpath);
    const bool current_is_directory = is_menu_directory_stem(current_stem);

    const std::vector<std::string> primary =
        current_is_directory ? wallpaper_candidates(current_stem) : wallpaper_candidates(full_stem);
    const std::vector<std::string> secondary =
        current_is_directory ? wallpaper_candidates(full_stem) : wallpaper_candidates(current_stem);

    if (!current_is_directory) {
        const std::string menu_file = resolve_menu_file_path(menu_root, fullpath, currentpath);
        const std::string rom_stem = rom_stem_from_menu_file(menu_file);
        if (!rom_stem.empty()) {
            std::string path = find_case_insensitive_image(wallpaper_dir, rom_stem);
            if (!path.empty()) return path;
        }
    }

    for (const std::string& candidate : primary) {
        std::string path = find_case_insensitive_image(wallpaper_dir, candidate);
        if (!path.empty()) return path;
    }

    for (const std::string& candidate : secondary) {
        std::string path = find_case_insensitive_image(wallpaper_dir, candidate);
        if (!path.empty()) return path;
    }

    for (const std::string& candidate : primary) {
        std::string path = find_fuzzy_image(wallpaper_dir, candidate);
        if (!path.empty()) return path;
    }

    for (const std::string& candidate : secondary) {
        std::string path = find_fuzzy_image(wallpaper_dir, candidate);
        if (!path.empty()) return path;
    }

    return {};
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool inflate_zlib(const std::vector<uint8_t>& input, std::vector<uint8_t>* output, size_t expected_size) {
    output->assign(expected_size, 0);
    z_stream zs{};
    zs.next_in = const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = output->data();
    zs.avail_out = static_cast<uInt>(output->size());

    if (inflateInit(&zs) != Z_OK) return false;
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return ret == Z_STREAM_END && zs.total_out == expected_size;
}

bool decode_png_file(const std::string& path, Image* image, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "failed to open PNG";
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n) data.insert(data.end(), buf, buf + n);
        if (n < sizeof(buf)) break;
    }
    std::fclose(f);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data.size() < 33 || std::memcmp(data.data(), sig, sizeof(sig)) != 0) {
        if (error) *error = "invalid PNG signature";
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    std::vector<uint8_t> idat;

    size_t pos = 8;
    while (pos + 12 <= data.size()) {
        uint32_t len = read_be32(&data[pos]);
        pos += 4;
        if (pos + 4 + len + 4 > data.size()) {
            if (error) *error = "truncated PNG chunk";
            return false;
        }
        const uint8_t* type = &data[pos];
        pos += 4;
        const uint8_t* chunk = &data[pos];
        pos += len;
        pos += 4;

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) {
                if (error) *error = "invalid IHDR length";
                return false;
            }
            width = read_be32(chunk);
            height = read_be32(chunk + 4);
            bit_depth = chunk[8];
            color_type = chunk[9];
            if (chunk[10] != 0 || chunk[11] != 0 || chunk[12] != 0) {
                if (error) *error = "unsupported PNG compression/filter/interlace";
                return false;
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), chunk, chunk + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    if (!width || !height || idat.empty() || bit_depth != 8) {
        if (error) *error = "unsupported or empty PNG";
        return false;
    }
    if (width > 8192 || height > 8192) {
        if (error) *error = "PNG dimensions are too large";
        return false;
    }

    int channels = 0;
    if (color_type == 6)
        channels = 4;
    else if (color_type == 2)
        channels = 3;
    else if (color_type == 0)
        channels = 1;
    else {
        if (error) *error = "unsupported PNG color type";
        return false;
    }

    const size_t row_bytes = static_cast<size_t>(width) * channels;
    const size_t inflated_size = static_cast<size_t>(height) * (row_bytes + 1);
    std::vector<uint8_t> raw;
    if (!inflate_zlib(idat, &raw, inflated_size)) {
        if (error) *error = "failed to inflate PNG data";
        return false;
    }

    std::vector<uint8_t> recon(static_cast<size_t>(height) * row_bytes, 0);
    size_t src = 0;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t filter = raw[src++];
        uint8_t* row = recon.data() + static_cast<size_t>(y) * row_bytes;
        const uint8_t* prev = y ? row - row_bytes : nullptr;
        for (size_t x = 0; x < row_bytes; ++x) {
            uint8_t v = raw[src++];
            int left = x >= static_cast<size_t>(channels) ? row[x - channels] : 0;
            int up = prev ? prev[x] : 0;
            int up_left = (prev && x >= static_cast<size_t>(channels)) ? prev[x - channels] : 0;
            switch (filter) {
            case 0:
                break;
            case 1:
                v = static_cast<uint8_t>(v + left);
                break;
            case 2:
                v = static_cast<uint8_t>(v + up);
                break;
            case 3:
                v = static_cast<uint8_t>(v + ((left + up) >> 1));
                break;
            case 4:
                v = static_cast<uint8_t>(v + paeth(left, up, up_left));
                break;
            default:
                if (error) *error = "invalid PNG filter";
                return false;
            }
            row[x] = v;
        }
    }

    image->width = width;
    image->height = height;
    image->rgba.assign(static_cast<size_t>(width) * height * 4, 255);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = recon.data() + static_cast<size_t>(y) * row_bytes;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* out = image->rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
            const uint8_t* in = row + static_cast<size_t>(x) * channels;
            if (channels == 4) {
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out[3] = in[3];
            } else if (channels == 3) {
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
                out[3] = 255;
            } else {
                out[0] = out[1] = out[2] = in[0];
                out[3] = 255;
            }
        }
    }

    return true;
}

bool decode_jpeg_file(const std::string& path, Image* image, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "failed to open JPEG";
        return false;
    }

    std::vector<uint8_t> data;
    uint8_t buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n) data.insert(data.end(), buf, buf + n);
        if (n < sizeof(buf)) break;
    }
    std::fclose(f);

    tjhandle handle = tjInitDecompress();
    if (!handle) {
        if (error) *error = tjGetErrorStr();
        return false;
    }

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(handle, data.data(), static_cast<unsigned long>(data.size()), &width, &height, &subsamp,
                            &colorspace) != 0) {
        if (error) *error = tjGetErrorStr2(handle);
        tjDestroy(handle);
        return false;
    }

    if (width <= 0 || height <= 0) {
        if (error) *error = "unsupported JPEG dimensions";
        tjDestroy(handle);
        return false;
    }
    if (width > 8192 || height > 8192) {
        if (error) *error = "JPEG dimensions are too large";
        tjDestroy(handle);
        return false;
    }

    image->width = static_cast<uint32_t>(width);
    image->height = static_cast<uint32_t>(height);
    image->rgba.assign(static_cast<size_t>(image->width) * image->height * 4, 255);

    if (tjDecompress2(handle, data.data(), static_cast<unsigned long>(data.size()), image->rgba.data(), width, 0,
                      height, TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
        if (error) *error = tjGetErrorStr2(handle);
        tjDestroy(handle);
        return false;
    }

    tjDestroy(handle);
    return true;
}

bool decode_image_file(const std::string& path, Image* image, std::string* error) {
    if (ends_with_case_insensitive(path, ".png")) return decode_png_file(path, image, error);
    if (ends_with_case_insensitive(path, ".jpg") || ends_with_case_insensitive(path, ".jpeg")) {
        return decode_jpeg_file(path, image, error);
    }
    if (error) *error = "unsupported image type";
    return false;
}

std::vector<uint32_t> render_to_frame(const Image& image, const Mode& mode) {
    std::vector<uint32_t> frame(static_cast<size_t>(mode.width) * mode.height, 0);
    if (!image.width || !image.height || image.rgba.empty()) return frame;

    for (int y = 0; y < mode.height; ++y) {
        uint32_t sy = static_cast<uint32_t>((static_cast<uint64_t>(y) * image.height) / mode.height);
        if (sy >= image.height) sy = image.height - 1;
        for (int x = 0; x < mode.width; ++x) {
            uint32_t sx = static_cast<uint32_t>((static_cast<uint64_t>(x) * image.width) / mode.width);
            if (sx >= image.width) sx = image.width - 1;
            const uint8_t* px = image.rgba.data() + (static_cast<size_t>(sy) * image.width + sx) * 4;
            uint32_t a = px[3];
            uint32_t r = (static_cast<uint32_t>(px[0]) * a + 127) / 255;
            uint32_t g = (static_cast<uint32_t>(px[1]) * a + 127) / 255;
            uint32_t b = (static_cast<uint32_t>(px[2]) * a + 127) / 255;
            frame[static_cast<size_t>(y) * mode.width + x] = (r << 16) | (g << 8) | b;
        }
    }
    return frame;
}

void write_frame_to_slot(uint8_t* map, size_t slot_offset, const Mode& mode, const std::vector<uint32_t>& frame) {
    uint8_t* dst = map + slot_offset;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(frame.data());
    size_t row_bytes = static_cast<size_t>(mode.width) * sizeof(uint32_t);
    for (int y = 0; y < mode.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * mode.stride, src + static_cast<size_t>(y) * row_bytes, row_bytes);
        if (mode.stride > static_cast<int>(row_bytes)) {
            std::memset(dst + static_cast<size_t>(y) * mode.stride + row_bytes, 0,
                        static_cast<size_t>(mode.stride) - row_bytes);
        }
    }
}

class Framebuffer {
  public:
    bool open_mem(std::string* error) {
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (fd_ < 0) {
            if (error) *error = std::string("failed to open /dev/mem: ") + std::strerror(errno);
            return false;
        }
        map_ = static_cast<uint8_t*>(
            mmap(nullptr, kFramebufferMapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, kFramebufferAddr));
        if (map_ == MAP_FAILED) {
            if (error) *error = std::string("failed to mmap framebuffer: ") + std::strerror(errno);
            map_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    ~Framebuffer() {
        if (map_) munmap(map_, kFramebufferMapBytes);
        if (fd_ >= 0) ::close(fd_);
    }

    bool write_all(const Mode& mode, const std::vector<uint32_t>& frame, std::string* error) {
        if (!map_ && !open_mem(error)) return false;
        write_frame_to_slot(map_, kFramebufferSlot1, mode, frame);
        write_frame_to_slot(map_, kFramebufferSlot2, mode, frame);
        write_frame_to_slot(map_, kFramebufferSlot0Offset, mode, frame);
        msync(map_, kFramebufferMapBytes, MS_SYNC);
        return true;
    }

  private:
    int fd_ = -1;
    uint8_t* map_ = nullptr;
};

bool send_mister_fb_cmd(const Mode& mode, std::string* error) {
    int fd = open(kMisterCmdPath, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = std::string("failed to open ") + kMisterCmdPath + ": " + std::strerror(errno);
        return false;
    }

    char cmd[128];
    int n = std::snprintf(cmd, sizeof(cmd), "fb_cmd1 8888 1 %d %d\n", mode.width, mode.height);
    bool ok = n > 0 && n < static_cast<int>(sizeof(cmd)) && write(fd, cmd, static_cast<size_t>(n)) == n;
    if (!ok && error) *error = std::string("failed to write ") + kMisterCmdPath + ": " + std::strerror(errno);
    close(fd);
    return ok;
}

bool apply_frame(Framebuffer* framebuffer, const Mode& mode, const std::vector<uint32_t>& frame, bool verbose) {
    std::string error;
    if (!framebuffer->write_all(mode, frame, &error)) {
        if (verbose) std::fprintf(stderr, "splashd: %s\n", error.c_str());
        return false;
    }

    if (!send_mister_fb_cmd(mode, &error) && verbose) {
        std::fprintf(stderr, "splashd: %s\n", error.c_str());
    }
    return true;
}

bool process_state(const Options& opts, const TmpState& state, Framebuffer* framebuffer, LastFrame* last,
                   bool verbose) {
    if (state.osd_visible == "0") {
        return true;
    }

    if (state.file_select != "active") {
        return true;
    }

    Mode mode;
    if (!read_mode_file(opts.mode_file, &mode)) {
        if (verbose) std::fprintf(stderr, "splashd: framebuffer mode is unavailable or unsupported\n");
        return false;
    }

    std::string wallpaper = resolve_wallpaper(opts.wallpaper_dir, state.fullpath, state.currentpath, opts.menu_root);
    bool using_default = false;
    if (wallpaper.empty()) {
        wallpaper = resolve_default_image(opts);
        using_default = !wallpaper.empty();
    }

    std::vector<uint32_t> frame(static_cast<size_t>(mode.width) * mode.height, 0);
    if (!wallpaper.empty()) {
        Image image;
        std::string error;
        if (!decode_image_file(wallpaper, &image, &error)) {
            if (verbose) std::fprintf(stderr, "splashd: %s: %s\n", wallpaper.c_str(), error.c_str());
        } else {
            frame = render_to_frame(image, mode);
            if (verbose) {
                std::fprintf(stderr, "splashd: showing %s%s\n", wallpaper.c_str(), using_default ? " (default)" : "");
            }
        }
    } else if (verbose) {
        std::fprintf(stderr, "splashd: no wallpaper or default image for %s; clearing\n",
                     strip_extension(state.fullpath).c_str());
    }

    if (!apply_frame(framebuffer, mode, frame, verbose)) return false;

    last->mode = mode;
    last->frame = std::move(frame);
    last->valid = true;
    return true;
}

bool redraw_after_osd_show(const Options& opts, const TmpState& state, Framebuffer* framebuffer, LastFrame* last,
                           bool verbose) {
    usleep(150000);
    bool ok = process_state(opts, state, framebuffer, last, verbose);
    usleep(350000);
    ok = process_state(opts, read_tmp_state(opts), framebuffer, last, verbose) && ok;
    return ok;
}

[[maybe_unused]] bool daemonize_process() {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid > 0) std::exit(0);
    if (setsid() < 0) return false;
    pid = fork();
    if (pid < 0) return false;
    if (pid > 0) std::exit(0);
    chdir("/");
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    return true;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s [--wallpaper-dir DIR] [--default-image FILE] [--menu-root DIR] [--mode-file FILE] "
                 "[--watch-dir DIR] [--foreground] [--once]\n",
                 argv0);
}

[[maybe_unused]] bool parse_args(int argc, char** argv, Options* opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--wallpaper-dir") {
            const char* v = need_value("--wallpaper-dir");
            if (!v) return false;
            opts->wallpaper_dir = v;
        } else if (arg == "--default-image") {
            const char* v = need_value("--default-image");
            if (!v) return false;
            opts->default_image = v;
        } else if (arg == "--menu-root") {
            const char* v = need_value("--menu-root");
            if (!v) return false;
            opts->menu_root = v;
        } else if (arg == "--mode-file") {
            const char* v = need_value("--mode-file");
            if (!v) return false;
            opts->mode_file = v;
        } else if (arg == "--watch-dir") {
            const char* v = need_value("--watch-dir");
            if (!v) return false;
            opts->watch_dir = v;
        } else if (arg == "--foreground") {
            opts->foreground = true;
        } else if (arg == "--once") {
            opts->once = true;
            opts->foreground = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

[[maybe_unused]] int watch_loop(const Options& opts) {
    Framebuffer framebuffer;
    LastFrame last;
    TmpState last_seen = read_tmp_state(opts);
    process_state(opts, last_seen, &framebuffer, &last, opts.foreground);

    int inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inofd >= 0) {
        int wd = inotify_add_watch(inofd, opts.watch_dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ATTRIB);
        if (wd >= 0) {
            char buf[4096];
            while (!g_stop) {
                ssize_t n = read(inofd, buf, sizeof(buf));
                bool relevant = false;
                if (n > 0) {
                    for (char* p = buf; p < buf + n;) {
                        auto* ev = reinterpret_cast<inotify_event*>(p);
                        std::string name = ev->len ? ev->name : "";
                        if (name == "FILESELECT" || name == "FULLPATH" || name == "CURRENTPATH" ||
                            name == "OSD_VISIBLE") {
                            relevant = true;
                        }
                        p += sizeof(inotify_event) + ev->len;
                    }
                }

                TmpState current = read_tmp_state(opts);
                if (relevant || state_changed(current, last_seen)) {
                    bool osd_became_visible = last_seen.osd_visible == "0" && current.osd_visible == "1";
                    last_seen = current;
                    process_state(opts, last_seen, &framebuffer, &last, opts.foreground);
                    if (osd_became_visible) {
                        redraw_after_osd_show(opts, last_seen, &framebuffer, &last, opts.foreground);
                    }
                }

                usleep(100000);
            }
            close(inofd);
            return 0;
        }
        close(inofd);
    }

    last_seen = TmpState{};
    while (!g_stop) {
        TmpState current = read_tmp_state(opts);
        if (state_changed(current, last_seen)) {
            bool osd_became_visible = last_seen.osd_visible == "0" && current.osd_visible == "1";
            last_seen = current;
            process_state(opts, last_seen, &framebuffer, &last, opts.foreground);
            if (osd_became_visible) {
                redraw_after_osd_show(opts, last_seen, &framebuffer, &last, opts.foreground);
            }
        }
        usleep(250000);
    }
    return 0;
}

} // namespace

#ifndef SPLASHD_TEST
int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, &opts)) {
        print_usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!opts.foreground && !daemonize_process()) {
        std::fprintf(stderr, "splashd: failed to daemonize\n");
        return 1;
    }

    if (opts.once) {
        Framebuffer framebuffer;
        LastFrame last;
        return process_state(opts, read_tmp_state(opts), &framebuffer, &last, opts.foreground) ? 0 : 1;
    }

    return watch_loop(opts);
}
#endif
