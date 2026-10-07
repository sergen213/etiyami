// Compile this source alone: it exercises the real private parsers, not a fake archiver.
#include "setup_assets.cpp"

#include <cassert>
#include <fstream>
#include <iostream>

namespace {
using namespace yami::setup;
struct Member { std::string name, bytes; bool link = false; };
void put(std::string& bytes, std::uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<char>(value >> (8 * i)));
}
std::string table(const std::vector<std::vector<std::uint32_t>>& rows, const std::vector<unsigned>& widths) {
    std::string bytes;
    for (std::size_t column = 0; column < widths.size(); ++column)
        for (const auto& row : rows) put(bytes, row[column], widths[column]);
    return bytes;
}
struct Msi {
    std::vector<std::string> strings{""};
    std::vector<std::vector<std::uint32_t>> directories, components, files;
    std::uint32_t text(std::string value) { strings.push_back(std::move(value)); return strings.size() - 1; }
    Msi() {
        const auto root = text("TARGETDIR"), install = text("INSTALLDIR"), data = text("DATA"), menu = text("MENU");
        directories = {{root, 0, text("SourceDir")}, {install, root, text(".")},
                       {data, install, text("data")}, {menu, data, text("menu")}};
        auto file = [&](const char* id, std::uint32_t directory, std::string name, std::uint32_t size) {
            const auto component = text(std::string("component") + id);
            components.push_back({component, 0, directory, 0x8000, 0, 0});
            files.push_back({text(id), component, text(std::move(name)), size + 0x80000000u, 0, 0, 0x8000,
                             static_cast<std::uint32_t>(0x8000 + files.size() + 1)});
        };
        file("_MENU", menu, "menulist.xml", 3);
        file("_DATA", data, std::string("KISA|ba") + char(0xf0) + ".xml", 4);
        file("_ICON", install, "yami.ico", 1);
        file("_WINDOWS", install, "eti.exe", 5);
        file("_SAVE", install, "son.eti", 6);
    }
    std::array<std::string, 5> streams() const {
        std::array<std::string, 5> result;
        put(result[0], 1254, 4);
        for (std::size_t i = 1; i < strings.size(); ++i) {
            put(result[0], strings[i].size(), 2); put(result[0], 1, 2); result[1] += strings[i];
        }
        result[2] = table(directories, {2, 2, 2});
        result[3] = table(components, {2, 2, 2, 2, 2, 2});
        result[4] = table(files, {2, 2, 2, 4, 2, 2, 2, 2});
        return result;
    }
};
void save(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); assert(out.good());
}
void iso(const fs::path& path, const std::vector<Member>& members) {
    auto* out = archive_write_new();
    assert(out && archive_write_set_format_iso9660(out) == ARCHIVE_OK);
    assert(archive_write_open_filename(out, path.c_str()) == ARCHIVE_OK);
    for (const auto& member : members) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, member.name.c_str());
        archive_entry_set_filetype(entry, member.link ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, member.link ? 0 : member.bytes.size());
        if (member.link) archive_entry_set_symlink(entry, "../outside");
        assert(archive_write_header(out, entry) == ARCHIVE_OK);
        if (!member.link) assert(archive_write_data(out, member.bytes.data(), member.bytes.size()) == static_cast<la_ssize_t>(member.bytes.size()));
        archive_entry_free(entry);
    }
    assert(archive_write_close(out) == ARCHIVE_OK); assert(archive_write_free(out) == ARCHIVE_OK);
}
void cab(const fs::path& path, const std::vector<Member>& members) {
    std::string entries, payload;
    for (const auto& member : members) {
        put(entries, member.bytes.size(), 4); put(entries, payload.size(), 4);
        put(entries, 0, 2); put(entries, 0, 2); put(entries, 0, 2); put(entries, 0x20, 2);
        entries += member.name; entries.push_back('\0'); payload += member.bytes;
    }
    assert(payload.size() <= 65535);
    std::string bytes = "MSCF";
    put(bytes, 0, 4); put(bytes, 44 + entries.size() + 8 + payload.size(), 4);
    put(bytes, 0, 4); put(bytes, 44, 4); put(bytes, 0, 4);
    put(bytes, 3, 1); put(bytes, 1, 1); put(bytes, 1, 2); put(bytes, members.size(), 2);
    put(bytes, 0, 2); put(bytes, 0, 2); put(bytes, 0, 2);
    put(bytes, 44 + entries.size(), 4); put(bytes, 1, 2); put(bytes, 0, 2);
    bytes += entries; put(bytes, 0, 4); put(bytes, payload.size(), 2); put(bytes, payload.size(), 2); bytes += payload;
    save(path, bytes);
}
template<class F> void rejected(F&& action, std::string_view reason = {}) {
    bool failed = false;
    try { action(); } catch (const std::exception& error) {
        failed = true;
        if (!reason.empty()) assert(std::string_view(error.what()).find(reason) != std::string_view::npos);
    }
    assert(failed);
}
}

int main(int argc, char** argv) {
    using namespace yami::setup;
    const auto root = fs::temp_directory_path() / ("yami-setup-assets-check-" + std::to_string(SDL_GetTicksNS()));
    assert(fs::create_directory(root));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    unsigned counter = 0;
    const auto fresh = [&] { const auto path = root / std::to_string(++counter); assert(fs::create_directory(path)); return path; };
    const auto mapping = parse_msi(Msi{}.streams(), {});
    assert(mapping.size() == 3 && mapping.at("_MENU").path == "data/menu/menulist.xml");
    assert(mapping.at("_DATA").path == "data/ba\xc4\x9f.xml" && mapping.at("_DATA").size == 4);
    assert(!mapping.contains("_WINDOWS") && !mapping.contains("_SAVE"));
    for (int variant = 0; variant < 14; ++variant) {
        Msi msi;
        switch (variant) {
        case 0: msi.files[0][2] = msi.text("../outside"); break;
        case 1: msi.directories[2][1] = msi.directories[3][0]; break;
        case 2: msi.files[0][0] = msi.files[1][0]; break;
        case 3: msi.files[0][3] = 0x7fffffffu; break;
        case 4: msi.files[0][1] = msi.text("missing-component"); break;
        case 5: msi.files[0][2] = 65535; break;
        case 6: msi.directories.push_back(msi.directories[2]); break;
        case 7: msi.files[1][2] = msi.text("data.exe"); break;
        case 8: msi.files[0][2] = msi.text("missing-marker.xml"); break;
        case 9: msi.components[1][2] = msi.directories[3][0]; msi.files[1][2] = msi.text("MENULIST.XML"); break;
        case 10: msi.files[1][7] = msi.files[0][7]; break;
        case 11: msi.files[1][2] = msi.text("menu"); break;
        case 12: msi.text(std::string(1, char(0x81))); break;
        case 13: msi.directories[2][2] = msi.text("data:../outside"); break;
        }
        rejected([&] { parse_msi(msi.streams(), {}); });
    }
    auto damaged = Msi{}.streams(); damaged[0][0] = 0; rejected([&] { parse_msi(damaged, {}); });
    damaged = Msi{}.streams(); damaged[1].pop_back(); rejected([&] { parse_msi(damaged, {}); });
    damaged = Msi{}.streams(); damaged[4].push_back('x'); rejected([&] { parse_msi(damaged, {}); });
    rejected([&] { parse_msi(Msi{}.streams(), [](auto, auto, auto) { return false; }); }, "cancel");
    for (const auto* unsafe : {"../outside", "/outside", "data/../outside", "C:/outside", "data\\outside", "data/./outside", "data/a:stream", "data/file ", "data/file."})
        rejected([&] { validate_path(unsafe); });

    const auto isoPath = root / "fixture.iso";
    iso(isoPath, {{"Yami.msi", "msi"}, {"Data1.cab", "cab"}, {"setup.exe", "never run"}});
    const auto scratch = fresh(); auto scratchFd = directory_fd(scratch);
    read_iso(isoPath, scratchFd.value, {});
    assert(fs::file_size(scratch / "Yami.msi") == 3 && fs::file_size(scratch / "Data1.cab") == 3);
    assert(!fs::exists(scratch / "setup.exe"));
    rejected([&] { read_iso(isoPath, scratchFd.value, {}); }); // O_EXCL never replaces scratch files.
    auto cancelledDir = fresh(); auto cancelledFd = directory_fd(cancelledDir);
    rejected([&] { read_iso(isoPath, cancelledFd.value, [](auto, auto, auto) { return false; }); }, "cancel");
    assert(fs::is_empty(cancelledDir));
    auto midIsoDir = fresh(); auto midIsoFd = directory_fd(midIsoDir);
    rejected([&] { read_iso(isoPath, midIsoFd.value, [](auto, auto done, auto) { return done == 0; }); }, "cancel");
    fs::resize_file(isoPath, fs::file_size(isoPath) - 1);
    rejected([&] { auto dir = directory_fd(fresh()); read_iso(isoPath, dir.value, {}); });
    iso(isoPath, {{"Yami.msi", "one"}, {"nested/yami.msi", "two"}, {"Data1.cab", "cab"}});
    rejected([&] { auto dir = directory_fd(fresh()); read_iso(isoPath, dir.value, {}); });
    iso(isoPath, {{"Yami.msi", "one"}, {"Data1.cab", "cab"}, {"unsafe", "", true}});
    rejected([&] { auto dir = directory_fd(fresh()); read_iso(isoPath, dir.value, {}); });
    save(isoPath, "not an ISO");
    rejected([&] { auto dir = directory_fd(fresh()); read_iso(isoPath, dir.value, {}); });

    const auto cabPath = root / "fixture.cab";
    const std::vector<Member> members{{"_MENU", "xml"}, {"_DATA", "data"}, {"_ICON", "i"}, {"_WINDOWS", "never"}, {"unused", "skip"}};
    cab(cabPath, members);
    const auto game = fresh(); auto gameFd = directory_fd(game); auto assets = mapping;
    read_cab(cabPath, gameFd.value, assets, {});
    assert(fs::file_size(game / "data/menu/menulist.xml") == 3 && fs::file_size(game / "data/ba\xc4\x9f.xml") == 4);
    assert(!fs::exists(game / "eti.exe") && !fs::exists(game / "unused"));
    for (int variant = 0; variant < 5; ++variant) {
        auto entries = members;
        switch (variant) {
        case 0: entries[0].name = "../outside"; break;
        case 1: entries[0].bytes = "too long"; break;
        case 2: entries.push_back(entries[0]); break;
        case 3: entries.erase(entries.begin()); break;
        case 4: entries.back().name = "_menu"; break;
        }
        cab(cabPath, entries);
        rejected([&] { auto dir = directory_fd(fresh()); auto expected = mapping; read_cab(cabPath, dir.value, expected, {}); });
    }
    cab(cabPath, members);
    auto interrupted = fresh(); auto interruptedFd = directory_fd(interrupted);
    rejected([&] { auto expected = mapping; read_cab(cabPath, interruptedFd.value, expected, [](auto, auto, auto) { return false; }); }, "cancel");
    assert(fs::is_empty(interrupted));
    auto midCabDir = fresh(); auto midCabFd = directory_fd(midCabDir);
    rejected([&] { auto expected = mapping; read_cab(cabPath, midCabFd.value, expected, [](auto, auto done, auto) { return done == 0; }); }, "cancel");
    assert(fs::is_regular_file(midCabDir / "data/menu/menulist.xml") && !fs::exists(midCabDir / "yami.ico"));
    const auto hostile = fresh(), outside = fresh(); fs::create_directory_symlink(outside, hostile / "data");
    auto hostileFd = directory_fd(hostile);
    rejected([&] { auto expected = mapping; read_cab(cabPath, hostileFd.value, expected, {}); });
    assert(fs::is_empty(outside));
    fs::resize_file(cabPath, fs::file_size(cabPath) - 1);
    rejected([&] { auto dir = directory_fd(fresh()); auto expected = mapping; read_cab(cabPath, dir.value, expected, {}); });

    if (argc == 3) {
        const auto realGame = fresh(), realScratch = fresh();
        extract_iso_assets(argv[1], realGame, realScratch, argv[2], {});
        std::uint64_t files = 0, bytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(realGame)) if (entry.is_regular_file()) { ++files; bytes += entry.file_size(); }
        assert(files == 9032 && bytes == 637554001);
        assert(fs::is_regular_file(realGame / "data/menu/menulist.xml"));
        assert(!fs::exists(realGame / "eti.exe") && !fs::exists(realGame / "son.eti") && !fs::exists(realGame / "drivers"));
        const std::array<std::string_view,2> selected{"data/menu/menulist.xml","data/fonts/title.xml"};
        const auto preview = fresh(), previewScratch = fresh();
        extract_iso_artwork(argv[1], preview, previewScratch, argv[2], selected, {});
        std::set<std::string> previewFiles;
        for (const auto& entry : fs::recursive_directory_iterator(preview))
            if (entry.is_regular_file()) previewFiles.insert(entry.path().lexically_relative(preview).generic_string());
        assert((previewFiles == std::set<std::string>{selected.begin(), selected.end()}));
        const std::array<std::string_view,2> duplicates{selected[0],selected[0]};
        rejected([&] { extract_iso_artwork(argv[1],fresh(),fresh(),argv[2],duplicates,{}); }, "duplicate");
        const std::array<std::string_view,1> missing{"data/menu/not-original-artwork.xml"};
        rejected([&] { extract_iso_artwork(argv[1],fresh(),fresh(),argv[2],missing,{}); }, "missing");
        unsigned readerCallbacks = 0;
        rejected([&] {
            msi_stream(fs::absolute(argv[2]), realScratch / "Yami.msi", "_StringPool", 0,
                       [&](auto, auto, auto) { return ++readerCallbacks == 1; });
        }, "cancel"); // First callback precedes spawn; the second cancels the owned live reader.
        rejected([&] { msi_stream(fs::absolute(argv[2]), realScratch / "Yami.msi", "missing-required-table", 0, {}); });
    } else assert(argc == 1);
    std::cout << "setup asset parser/archive checks passed\n";
}
