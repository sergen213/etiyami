#define YAMI_REMOVE_NO_MAIN
#include "uninstall_windows.cpp"

#include <objbase.h>
#include <shobjidl.h>
#include <cstdio>

namespace fs = std::filesystem;
namespace {
void put(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out << value;
    removal::require(bool(out), "Cannot write uninstall fixture");
}
std::string get(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void shortcut(const fs::path& path, const fs::path& root) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    removal::require(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE, "Cannot initialize shortcut test");
    struct Com { bool active; ~Com() { if (active) CoUninitialize(); } } com{SUCCEEDED(initialized)};
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    removal::require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
                     "Cannot create shortcut fixture");
    struct Release { IShellLinkW* link; IPersistFile*& file; ~Release() { if (file) file->Release(); if (link) link->Release(); } } release{link, file};
    removal::require(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))), "Cannot persist shortcut fixture");
    const auto target = root/yami::ownership::helperDirectory/yami::ownership::helperName;
    const auto arguments = L"--root \"" + root.native() + L"\"";
    removal::require(SUCCEEDED(link->SetPath(target.c_str())) && SUCCEEDED(link->SetWorkingDirectory(root.c_str())) &&
                     SUCCEEDED(link->SetIconLocation((root/"eti-yami.ico").c_str(), 0)) &&
                     SUCCEEDED(link->SetArguments(arguments.c_str())) && SUCCEEDED(file->Save(path.c_str(), TRUE)),
                     "Cannot save shortcut fixture");
}
void fixture(const fs::path& root, const fs::path& shortcuts, const fs::path& helper = {}) {
    put(root/"game/data/owned", "original asset");
    put(root/"yami-native.exe", "engine"); put(root/"yami-launcher.exe", "launcher"); put(root/"yami-updater.exe", "updater");
    put(root/"new-check.dll", "updated library"); put(root/"eti-yami.ico", "icon");
    if (helper.empty()) put(root/yami::ownership::helperDirectory/yami::ownership::helperName, "helper");
    else {
        fs::create_directories(root/yami::ownership::helperDirectory);
        removal::copy_file(helper, root/yami::ownership::helperDirectory/yami::ownership::helperName, 512ULL * 1024 * 1024);
    }
    put(root/yami::ownership::markerName, yami::ownership::marker);
    put(root/yami::ownership::engineName, yami::ownership::serialize_engine(
        {"new-check.dll", "yami-launcher.exe", "yami-native.exe", "yami-updater.exe"}));
    fs::create_directories(shortcuts);
    const auto link = shortcuts/yami::ownership::uninstallShortcut;
    shortcut(link, root);
    put(root/yami::ownership::installedName, yami::ownership::serialize_installed({
        {".eti-yami-engine-files", ".eti-yami-install", ".eti-yami-installed-files",
         ".eti-yami-uninstall/yami-remove.exe", "eti-yami.ico", "game/data/owned"},
        {yami::windows_install::text(link)}}));
    put(root/"game.ini", "settings"); put(root/"game/checkpoints/slot1", "save");
    put(root/"unknown", "custom file"); put(root/"unknown.dll", "unproven library");
}
void rejected(const fs::path& root) {
    try { removal::uninstall(root, true); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Unsafe uninstall was accepted");
}
void queue_console(std::string_view value) {
    auto standardInput = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(standardInput, &mode)) {
        (void)FreeConsole();
        removal::require(AllocConsole() != 0, "Cannot allocate uninstall test console");
    }
    static removal::Handle input;
    input = removal::Handle(CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, 0, nullptr));
    removal::require(bool(input) && SetStdHandle(STD_INPUT_HANDLE, input.value) != 0 &&
                     std::freopen("CONIN$", "r", stdin) != nullptr, "Console input unavailable");
    std::cin.clear();
    for (const char c : value) {
        for (const bool down : {true, false}) {
            INPUT_RECORD record{}; record.EventType = KEY_EVENT;
            record.Event.KeyEvent.bKeyDown = down ? TRUE : FALSE;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.wVirtualKeyCode = c == '\n' ? VK_RETURN : static_cast<WORD>(c);
            record.Event.KeyEvent.uChar.AsciiChar = c == '\n' ? '\r' : c;
            DWORD written = 0;
            removal::require(WriteConsoleInputW(input.value, &record, 1, &written) != 0 && written == 1,
                               "Cannot queue console response");
        }
    }
}
void run_detached(const fs::path& root) {
    const auto helper = root/yami::ownership::helperDirectory/yami::ownership::helperName;
    std::wstring command = L"\"" + helper.native() + L"\" --root \"" + root.native() + L"\" --yes";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    removal::require(CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                                    nullptr, root.c_str(), &startup, &process) != 0, "Cannot launch detached removal check");
    CloseHandle(process.hThread);
    removal::require(WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0, "Removal parent did not exit");
    DWORD code = 1; removal::require(GetExitCodeProcess(process.hProcess, &code) != 0 && code == 0, "Removal parent failed");
    CloseHandle(process.hProcess);
    for (unsigned attempt = 0; attempt < 300 && (fs::exists(helper) || fs::exists(root/".yami-update-lock")); ++attempt) Sleep(100);
    removal::require(!fs::exists(helper) && !fs::exists(root/".yami-update-lock"), "Detached helper did not remove itself cleanly");
}
void active_process_guard(const fs::path& root) {
    const auto commandInterpreter = fs::path(_wgetenv(L"COMSPEC"));
    fs::copy_file(commandInterpreter, root/"yami-native.exe", fs::copy_options::overwrite_existing);
    std::wstring command = L"\"" + (root/"yami-native.exe").native() + L"\" /c \"ping -n 10 127.0.0.1 >nul\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    removal::require(CreateProcessW((root/"yami-native.exe").c_str(), command.data(), nullptr, nullptr, FALSE,
                                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, root.c_str(), &startup, &process) != 0,
                     "Cannot start active-process fixture");
    CloseHandle(process.hThread);
    Sleep(250);
    rejected(root);
    removal::require(get(root/"game/data/owned") == "original asset", "Active process guard removed owned data");
    TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE); CloseHandle(process.hProcess);
}
int console_child(const fs::path& root) {
    queue_console("no\n");
    removal::require(removal::uninstall(root, false) == 0 && get(root/"game/data/owned") == "original asset",
                     "Console 'no' did not cancel removal");
    queue_console("y\n");
    removal::require(removal::uninstall(root, false) == 0 && !fs::exists(root/"game/data/owned") &&
                     get(root/"game.ini") == "settings", "Console 'y' did not authorize removal");
    return 0;
}
}
int main() {
    try {
        int argc = 0;
        const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        removal::require(argv != nullptr, "Cannot read Windows command line");
        struct Arguments { wchar_t** value; ~Arguments() { LocalFree(value); } } arguments{argv};
        removal::require(argc >= 3, "Usage: check-uninstall-windows REMOVER ISOLATED_CHECK_DIRECTORY [--console-child ROOT]");
        if (argc == 5 && std::wstring(argv[3]) == L"--console-child") return console_child(fs::path(argv[4]));
        const auto helper = fs::absolute(fs::path(argv[1])), base = fs::absolute(fs::path(argv[2]));
        removal::require(!fs::exists(base), "Check directory must not exist");
        fs::create_directories(base);
        const auto root = base/L"Türkçe space %"/"etiyami", links = base/"Start Menu";
        fixture(root, links);
        std::cerr << "stage: direct uninstall\n";
        const auto installed = get(root/yami::ownership::installedName);
        auto claims = yami::ownership::parse_installed(installed); claims.files.push_back("game.ini");
        put(root/yami::ownership::installedName, yami::ownership::serialize_installed(claims));
        rejected(root); removal::require(get(root/"game.ini") == "settings", "Unsafe inventory claimed settings");
        put(root/yami::ownership::installedName, installed);
        removal::require(CreateHardLinkW((root/"ownership-hardlink").c_str(), (root/yami::ownership::engineName).c_str(), nullptr) != 0,
                         "Cannot create hard-link fixture");
        rejected(root); fs::remove(root/"ownership-hardlink");
        fs::create_directory(root/".yami-update-lock"); rejected(root); fs::remove(root/".yami-update-lock");
        put(links/yami::ownership::uninstallShortcut, "unowned shortcut");
        rejected(root); fs::remove(links/yami::ownership::uninstallShortcut); shortcut(links/yami::ownership::uninstallShortcut, root);
        if (CreateSymbolicLinkW((base/"linked-root").c_str(), root.c_str(),
                                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
            rejected(base/"linked-root"); fs::remove(base/"linked-root");
        }
        active_process_guard(root);
        std::cerr << "stage: active process and direct uninstall\n";
        removal::require(removal::uninstall(root, true) == 0 && !fs::exists(root/"game/data/owned") &&
                         !fs::exists(root/"yami-native.exe") && !fs::exists(links/yami::ownership::uninstallShortcut) &&
                         get(root/"game.ini") == "settings" && get(root/"game/checkpoints/slot1") == "save" &&
                         get(root/"unknown") == "custom file" && get(root/"unknown.dll") == "unproven library",
                         "Uninstall preservation or owned removal failed");
        const auto consoleRoot = base/"console-install";
        std::cerr << "stage: console confirmation\n";
        fixture(consoleRoot, base/"console-menu");
        console_child(consoleRoot);
        const auto detachedRoot = base/"detached-install";
        std::cerr << "stage: detached self-removal\n";
        fixture(detachedRoot, base/"detached-menu", helper);
        run_detached(detachedRoot);
        removal::require(!fs::exists(detachedRoot/"yami-native.exe") && get(detachedRoot/"game.ini") == "settings" &&
                         get(detachedRoot/"unknown") == "custom file", "Detached removal lost preservation");
        std::error_code cleanup; fs::remove_all(base, cleanup);
        std::cout << "Windows uninstall confirmation, process, ownership, preservation and self-removal checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
