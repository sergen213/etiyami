#define YAMI_REMOVE_NO_MAIN
#include "uninstall_main.cpp"
#include <cassert>
#include <fstream>
#include <sys/wait.h>
#include <csignal>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>

namespace fs = std::filesystem;
namespace {
void put(const fs::path& path, std::string_view value, mode_t mode = 0600) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path); out << value; out.close();
    assert(out && ::chmod(path.c_str(), mode) == 0);
}
std::string get(const fs::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void fixture(const fs::path& root, const fs::path& shortcuts) {
    put(root/"game/data/owned", "original asset");
    put(root/"yami-native", "updated engine");
    put(root/"yami-launcher", "updated launcher");
    put(root/"yami-updater", "updated updater");
    put(root/"libnew.so.1", "updated library");
    put(root/"uninstall.sh", "script");
    put(root/".eti-yami-uninstall/yami-remove", "helper");
    put(root/".eti-yami-install", "ETI Yami native Linux installation\nformat=1\n");
    put(root/".eti-yami-engine-files", "ETI Yami engine ownership\nformat=1\nlibnew.so.1\nyami-launcher\nyami-native\nyami-updater\n");
    const auto shortcut = shortcuts/"eti-yami-uninstall.desktop";
    put(shortcut, "[Desktop Entry]\nX-ETIYami-Setup=true\nX-ETIYami-InstallRoot=" +
        removal::desktop_value(root.string()) + "\n");
    put(root/".eti-yami-installed-files", "ETI Yami installed ownership\nformat=1\n"
        "F\t.eti-yami-engine-files\nF\t.eti-yami-install\nF\t.eti-yami-installed-files\n"
        "F\t.eti-yami-uninstall/yami-remove\nF\tgame/data/owned\nF\tuninstall.sh\nS\t" + shortcut.string() + "\n");
    put(root/"game.ini", "settings");
    put(root/"game/checkpoints/slot1", "save");
    put(root/"unknown", "custom file");
    put(root/"libunknown.so.7", "unproven library");
}
int run(const fs::path& helper, const fs::path& root, bool yes, std::string input = {}, bool terminal = false, bool script = false) {
    int master = -1, childInput = -1;
    if (terminal) {
        master = ::posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
        assert(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0);
        childInput = ::open(::ptsname(master), O_RDWR|O_NOCTTY|O_CLOEXEC);
        assert(childInput >= 0);
    } else childInput = ::open("/dev/null", O_RDONLY|O_CLOEXEC);
    const auto pid = ::fork(); assert(pid >= 0);
    if (pid == 0) {
        ::dup2(childInput, STDIN_FILENO);
        if (terminal) ::dup2(childInput, STDOUT_FILENO);
        ::close(childInput);
        if (master >= 0) ::close(master);
        ::unsetenv("SUDO_USER"); ::unsetenv("SUDO_UID"); ::unsetenv("SUDO_COMMAND");
        if (script) {
            ::execl("/bin/sh", "/bin/sh", (root/"uninstall.sh").c_str(), nullptr);
            ::_exit(127);
        }
        if (yes) ::execl(helper.c_str(), helper.c_str(), "--root", root.c_str(), "--yes", nullptr);
        else ::execl(helper.c_str(), helper.c_str(), "--root", root.c_str(), nullptr);
        ::_exit(127);
    }
    ::close(childInput);
    if (terminal) assert(::write(master, input.data(), input.size()) == static_cast<ssize_t>(input.size()));
    int status = 0; assert(::waitpid(pid, &status, 0) == pid);
    if (master >= 0) ::close(master);
    assert(WIFEXITED(status)); return WEXITSTATUS(status);
}
void protected_process(const fs::path& helper, const fs::path& root, const char* name, bool candidate) {
    int ready[2]; assert(::pipe2(ready, O_CLOEXEC) == 0);
    const auto pid = ::fork(); assert(pid >= 0);
    if (pid == 0) {
        ::close(ready[0]);
        assert(::prctl(PR_SET_NAME, name) == 0 && ::prctl(PR_SET_DUMPABLE, 0) == 0);
        assert(::write(ready[1], "R", 1) == 1);
        ::close(ready[1]);
        for (;;) ::pause();
    }
    ::close(ready[1]); char acknowledged = 0;
    assert(::read(ready[0], &acknowledged, 1) == 1 && acknowledged == 'R'); ::close(ready[0]);
    const auto result = run(helper, root, false, "no\n", true);
    assert((candidate && result != 0) || (!candidate && result == 0));
    assert(get(root/"game/data/owned") == "original asset");
    assert(::kill(pid, SIGTERM) == 0);
    int status; assert(::waitpid(pid, &status, 0) == pid);
}
void detachment_boundaries(const fs::path& base) {
    using namespace removal;
    const auto root = base/"install";
    put(root/"data/asset", "owned");
    Tree tree;
    auto original = snapshot(tree, root/"data/asset");
    Recovery recovery(tree, root);
    auto rejects = [](auto action) {
        bool caught = false;
        try { action(); } catch (const std::exception&) { caught = true; }
        assert(caught);
    };
    fs::rename(root/"data/asset", root/"original");
    put(root/"data/asset", "unrelated replacement");
    rejects([&] { auto file = detach(tree, original, recovery, "leaf"); file.erase(tree); });
    assert(get(root/"data/asset") == "unrelated replacement" && get(root/"original") == "owned");
    fs::remove(root/"data/asset"); fs::rename(root/"original", root/"data/asset");
    fs::rename(root/"data", root/"displaced-data"); put(root/"data/asset", "replacement parent");
    rejects([&] { auto file = detach(tree, original, recovery, "parent"); file.erase(tree); });
    assert(get(root/"data/asset") == "replacement parent" && get(root/"displaced-data/asset") == "owned");
    fs::remove_all(root/"data"); fs::rename(root/"displaced-data", root/"data");
    fs::rename(root, base/"displaced-root"); put(root/"data/asset", "replacement root");
    rejects([&] { auto file = detach(tree, original, recovery, "root"); file.erase(tree); });
    assert(get(root/"data/asset") == "replacement root" && get(base/"displaced-root/data/asset") == "owned");
    fs::remove_all(root); fs::rename(base/"displaced-root", root);
    {
        auto file = detach(tree, original, recovery, "recover");
        put(root/"data/asset", "do not overwrite");
        file.restore();
        assert(file.active && recovery.retained);
        assert(get(recovery.path/"recover") == "owned" && get(root/"data/asset") == "do not overwrite");
    }
    fs::remove(root/"data/asset");
    assert(rename_new(recovery.directory.value, "recover", AT_FDCWD, (root/"data/asset").string()) == 0);
    recovery.retained = false;
    {
        auto file = detach(tree, original, recovery, "private-leaf");
        fs::rename(recovery.path/"private-leaf", root/"original");
        put(recovery.path/"private-leaf", "private replacement");
        put(root/"data/asset", "original-path replacement");
        rejects([&] { file.erase(tree); });
        file.restore();
        assert(get(root/"original") == "owned" && get(root/"data/asset") == "original-path replacement");
        assert(get(recovery.path/"private-leaf") == "private replacement");
    }
    const auto meta = root/".eti-yami-install";
    put(meta, "marker");
    auto m = snapshot(tree, meta, true);
    write_private(recovery.directory.value, "backup", "marker");
    auto copy = snapshot(tree, recovery.path/"backup", true);
    fs::remove(meta);
    std::size_t serial = 20;
    restore_metadata(tree, recovery, {{m, copy}}, serial);
    assert(get(meta) == "marker");
    auto restored = snapshot(tree, meta, true);
    assert(restored.id.links == 1 && (restored.id.mode & 0077) == 0);
    fs::create_directory(root/"container");
    auto container = tree.open_directory(root/"container", true);
    const auto containerId = identify(container.value);
    fs::rename(root/"container", root/"original-container");
    fs::create_directory(root/"container");
    auto rootFd = tree.open_directory(root);
    assert(!remove_created_directory(rootFd.value, "container", containerId));
    assert(fs::is_directory(root/"container") && fs::is_directory(root/"original-container"));
}
void mount_boundary(const fs::path& base) {
    fs::create_directories(base);
    const auto uid = ::getuid(), gid = ::getgid();
    const auto pid = ::fork(); assert(pid >= 0);
    if (pid == 0) {
        if (::unshare(CLONE_NEWUSER|CLONE_NEWNS) != 0) ::_exit(77);
        auto map = [](const char* path, const std::string& text) {
            std::ofstream out(path); out << text; out.close(); return bool(out);
        };
        if (!map("/proc/self/setgroups", "deny\n") ||
            !map("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n") ||
            !map("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n")) ::_exit(77);
        removal::Fd before(::open(base.c_str(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        assert(before.value >= 0);
        const auto expected = removal::identify(before.value);
        if (::mount(base.c_str(), base.c_str(), nullptr, MS_BIND, nullptr) != 0) ::_exit(77);
        removal::Fd after(::open(base.c_str(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        assert(after.value >= 0);
        const auto mounted = removal::identify(after.value);
        assert(expected.device == mounted.device && expected.inode == mounted.inode &&
               expected.uid == mounted.uid && expected.mount != mounted.mount &&
               !removal::same(expected, mounted));
        ::_exit(0);
    }
    int status; assert(::waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 77);
    if (WEXITSTATUS(status) == 77)
        std::cout << "Bind-mount boundary skipped: kernel disallows private user/mount namespace\n";
}
}
int main(int argc, char** argv) {
    assert(argc == 3 && ::geteuid() != 0);
    const auto helper = fs::absolute(argv[1]);
    const char* home = ::getenv("HOME"); assert(home && home[0] == '/');
    auto cache = fs::path(home)/".cache"; fs::create_directories(cache);
    std::string pattern = (cache/"eti-yami-uninstall-check-XXXXXX").string();
    assert(::mkdtemp(pattern.data())); const fs::path base(pattern);
    ::setenv("HOME", base.c_str(), 1); ::setenv("XDG_DATA_HOME", (base/"data").c_str(), 1);
    const auto root = base/"Türkçe space % quote\" slash\\ dollar$ tick`"/"etiyami";
    const auto shortcuts = base/"Desktop";
    fixture(root, shortcuts);
    assert(run(helper, root, false) != 0); // A pipe never authorizes removal.
    assert(run(helper, root, false, "\n", true) == 0);
    assert(run(helper, root, false, "no\n", true) == 0);
    assert(get(root/"game/data/owned") == "original asset");
    protected_process(helper, root, "unrelated-check", false);
    protected_process(helper, root, "yami-updater", true);
    const auto native = get(root/"yami-native");
    fs::copy_file("/bin/sleep", root/"yami-native", fs::copy_options::overwrite_existing);
    assert(::chmod((root/"yami-native").c_str(), 0700) == 0);
    const auto active = ::fork(); assert(active >= 0);
    if (active == 0) { ::execl((root/"yami-native").c_str(), "yami-native", "60", nullptr); ::_exit(127); }
    bool started = false;
    for (int i = 0; i < 100; ++i) {
        std::array<char, 8192> path{};
        const auto n = ::readlink(("/proc/" + std::to_string(active) + "/exe").c_str(), path.data(), path.size());
        if (n > 0 && std::string(path.data(), static_cast<std::size_t>(n)) == (root/"yami-native").string()) {
            started = true; break;
        }
        ::usleep(10000);
    }
    assert(started && run(helper, root, true) != 0 && get(root/"game/data/owned") == "original asset");
    assert(::kill(active, SIGTERM) == 0);
    int activeStatus; assert(::waitpid(active, &activeStatus, 0) == active);
    put(root/"yami-native", native);
    assert(!fs::exists(root/".yami-update-lock"));
    auto original = get(root/".eti-yami-installed-files");
    for (const auto bad : {"F\t../outside\n", "F\tgame.ini\n", "F\tgame//owned\n", "S\t/etc/eti-yami.desktop\n"}) {
        put(root/".eti-yami-installed-files", original + bad);
        assert(run(helper, root, true) != 0);
        assert(get(root/"game/data/owned") == "original asset");
    }
    auto claims = yami::ownership::parse_installed(original);
    claims.files.push_back("game.ini");
    put(root/".eti-yami-installed-files", yami::ownership::serialize_installed(claims));
    assert(run(helper, root, true) != 0 && get(root/"game.ini") == "settings");
    fs::create_symlink(root, base/"symlink-root");
    assert(run(helper, base/"symlink-root", true) != 0);
    fs::remove(base/"symlink-root");
    put(root/".eti-yami-installed-files", original);
    fs::rename(root/"game/data/owned", root/"saved-asset");
    fs::create_symlink(root/"unknown", root/"game/data/owned");
    assert(run(helper, root, true) != 0 && get(root/"unknown") == "custom file");
    fs::remove(root/"game/data/owned");
    fs::create_hard_link(root/"saved-asset", root/"game/data/owned");
    assert(run(helper, root, true) != 0 && get(root/"saved-asset") == "original asset");
    fs::remove(root/"game/data/owned"); fs::rename(root/"saved-asset", root/"game/data/owned");
    fs::create_directory(root/".yami-update-lock");
    assert(run(helper, root, true) != 0);
    fs::remove(root/".yami-update-lock");
    auto owner = get(root/".eti-yami-install");
    fs::rename(root/".eti-yami-install", root/"saved-marker");
    fs::create_symlink(root/"saved-marker", root/".eti-yami-install");
    assert(run(helper, root, true) != 0);
    fs::remove(root/".eti-yami-install"); fs::rename(root/"saved-marker", root/".eti-yami-install");
    ::chmod((root/".eti-yami-install").c_str(), 0644);
    assert(run(helper, root, true) != 0); ::chmod((root/".eti-yami-install").c_str(), 0600);
    fs::create_hard_link(root/".eti-yami-install", root/"hardlink-marker");
    assert(run(helper, root, true) != 0);
    fs::remove(root/"hardlink-marker");
    put(shortcuts/"eti-yami-uninstall.desktop", "[Desktop Entry]\nX-ETIYami-Setup=true\nX-ETIYami-InstallRoot=/another/root\n");
    assert(run(helper, root, true) != 0);
    put(shortcuts/"eti-yami-uninstall.desktop", "[Desktop Entry]\nX-ETIYami-Setup=true\nX-ETIYami-InstallRoot=" + removal::desktop_value(root.string()) + "\n");
    fs::remove(root/"libnew.so.1"); // An interrupted previous removal resumes.
    assert(run(helper, root, false, "yes\n", true) == 0);
    assert(!fs::exists(root/"game/data/owned") && !fs::exists(root/"yami-native"));
    assert(!fs::exists(root/".eti-yami-installed-files") && !fs::exists(root/".eti-yami-uninstall"));
    assert(!fs::exists(shortcuts/"eti-yami-uninstall.desktop"));
    assert(get(root/"game.ini") == "settings" && get(root/"game/checkpoints/slot1") == "save");
    assert(get(root/"unknown") == "custom file" && get(root/"libunknown.so.7") == "unproven library");
    detachment_boundaries(base/"boundaries");
    mount_boundary(base/"mount-boundary");
    // Shell must finish reading after its own pathname and helper are removed.
    const auto shellRoot = base/"script-install";
    fixture(shellRoot, base/"script-Desktop");
    fs::copy_file(argv[2], shellRoot/"uninstall.sh", fs::copy_options::overwrite_existing);
    fs::copy_file(helper, shellRoot/".eti-yami-uninstall/yami-remove", fs::copy_options::overwrite_existing);
    ::chmod((shellRoot/".eti-yami-uninstall/yami-remove").c_str(), 0700);
    const auto pid = ::fork(); assert(pid >= 0);
    if (pid == 0) {
        int master = ::open("/dev/null", O_RDONLY); ::dup2(master, STDIN_FILENO); ::close(master);
        ::execl("/bin/sh", "/bin/sh", (shellRoot/"uninstall.sh").c_str(), nullptr); ::_exit(127);
    }
    int status; assert(::waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) != 0);
    assert(fs::exists(shellRoot/".eti-yami-installed-files"));
    assert(run(helper, shellRoot, false, "yes\n\n", true, true) == 0);
    assert(!fs::exists(shellRoot/"uninstall.sh") && !fs::exists(shellRoot/".eti-yami-uninstall/yami-remove"));
    assert(!fs::exists(shellRoot/".eti-yami-installed-files") && get(shellRoot/"game.ini") == "settings");
    // Only this mkdtemp-owned fixture is recursively cleaned; never an installation.
    fs::remove_all(base);
    std::cout << "Uninstall confirmation, preservation, unsafe ownership and recovery checks passed\n";
}
