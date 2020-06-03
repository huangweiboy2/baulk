///
#include <bela/terminal.hpp>
#include <bela/parseargv.hpp>
#include <bela/process.hpp>
#include <bela/subsitute.hpp>
#include <bela/pe.hpp>
#include <baulkenv.hpp>
#include <baulkrev.hpp>

namespace baulk::exec {
bool IsDebugMode = false;
template <typename... Args> bela::ssize_t DbgPrint(const wchar_t *fmt, Args... args) {
  if (!IsDebugMode) {
    return 0;
  }
  const bela::format_internal::FormatArg arg_array[] = {args...};
  std::wstring str;
  str.append(L"\x1b[33m* ");
  bela::format_internal::StrAppendFormatInternal(&str, fmt, arg_array, sizeof...(args));
  if (str.back() == '\n') {
    str.pop_back();
  }
  str.append(L"\x1b[0m\n");
  return bela::terminal::WriteAuto(stderr, str);
}
inline bela::ssize_t DbgPrint(const wchar_t *fmt) {
  if (!IsDebugMode) {
    return 0;
  }
  std::wstring_view msg(fmt);
  if (!msg.empty() && msg.back() == '\n') {
    msg.remove_suffix(1);
  }
  return bela::terminal::WriteAuto(stderr, bela::StringCat(L"\x1b[33m* ", msg, L"\x1b[0m\n"));
}

template <typename... Args>
bela::ssize_t DbgPrintEx(char32_t prefix, const wchar_t *fmt, Args... args) {
  if (!IsDebugMode) {
    return 0;
  }
  const bela::format_internal::FormatArg arg_array[] = {args...};
  auto str = bela::StringCat(L"\x1b[32m* ", prefix, L" ");
  bela::format_internal::StrAppendFormatInternal(&str, fmt, arg_array, sizeof...(args));
  if (str.back() == '\n') {
    str.pop_back();
  }
  str.append(L"\x1b[0m\n");
  return bela::terminal::WriteAuto(stderr, str);
}
inline bela::ssize_t DbgPrintEx(char32_t prefix, const wchar_t *fmt) {
  if (!IsDebugMode) {
    return 0;
  }
  std::wstring_view msg(fmt);
  if (!msg.empty() && msg.back() == '\n') {
    msg.remove_suffix(1);
  }
  return bela::terminal::WriteAuto(stderr,
                                   bela::StringCat(L"\x1b[32m", prefix, L" ", msg, L"\x1b[0m\n"));
}

using argv_t = std::vector<std::wstring_view>;
constexpr const wchar_t *string_nullable(std::wstring_view str) {
  return str.empty() ? nullptr : str.data();
}
constexpr wchar_t *string_nullable(std::wstring &str) { return str.empty() ? nullptr : str.data(); }

bool IsSubsytemConsole(std::wstring_view exe) {
  std::wstring target;
  if (!bela::ExecutableExistsInPath(exe, target)) {
    bela::FPrintF(stderr, L"unable found target: '%s'\n", target);
    return false;
  }
  bela::error_code ec;
  auto realexe = bela::RealPathEx(target, ec);
  if (!realexe) {
    DbgPrint(L"resolve realpath %s %s\n", target, ec.message);
    return false;
  }
  DbgPrint(L"resolve realpath %s\n", *realexe);
  auto pe = bela::pe::Expose(*realexe, ec);
  if (!pe) {
    return true;
  }
  return pe->subsystem == bela::pe::Subsystem::CUI;
}

struct baulkcommand_t {
  argv_t argv;
  std::wstring env;
  std::wstring cwd;
  int operator()() {
    auto arg0 = argv[0];
    auto isconsole = IsSubsytemConsole(arg0);
    DbgPrint(L"Arg0 %s subsystem is console: %b", arg0, isconsole);
    bela::EscapeArgv ea;
    ea.Assign(arg0);
    for (size_t i = 1; i < argv.size(); i++) {
      ea.Append(argv[i]);
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SecureZeroMemory(&si, sizeof(si));
    SecureZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (CreateProcessW(nullptr, ea.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                       string_nullable(env), string_nullable(cwd), &si, &pi) != TRUE) {
      auto ec = bela::make_system_error_code();
      bela::FPrintF(stderr, L"unable detect launcher target: %s\n", ec.message);
      return -1;
    }
    CloseHandle(pi.hThread);
    SetConsoleCtrlHandler(nullptr, TRUE);
    auto closer = bela::finally([&] {
      SetConsoleCtrlHandler(nullptr, FALSE);
      CloseHandle(pi.hProcess);
    });
    if (!isconsole) {
      return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    return exitCode;
  }
};
void Usage() {
  constexpr std::wstring_view usage = LR"(baulkexec - Baulk extend executor
Usage: baulkexec [option] command args ...
  -h|--help        Show usage text and quit
  -v|--version     Show version number and quit
  -V|--verbose     Make the operation more talkative
  -C|--cleanup     Create clean environment variables to avoid interference
  -W|--cwd         Set the command startup directory
  --vs             Load Visual Studio related environment variables
  --clang          Add Visual Studio's built-in clang to the PATH environment variable

example:
  baulkexec -V --vs TUNNEL_DEBUG=1 pwsh

)";
  bela::terminal::WriteAuto(stderr, usage);
}

void Version() {
  bela::FPrintF(stdout, L"baulk %s\nRelease:    %s\nCommit:     %s\nBuild Time: %s\n",
                BAULK_RELEASE_VERSION, BAULK_RELEASE_NAME, BAULK_RELEASE_COMMIT, BAULK_BUILD_TIME);
}

// ParseArgv todo
bool ParseArgv(int argc, wchar_t **argv, baulk::exec::baulkcommand_t &cmd) {
  bela::ParseArgv pa(argc, argv, true);
  pa.Add(L"help", bela::no_argument, L'h')
      .Add(L"version", bela::no_argument, L'v')
      .Add(L"verbose", bela::no_argument, L'V')
      .Add(L"cleanup", bela::no_argument, L'C') // cleanup environment
      .Add(L"cwd", bela::required_argument, L'W')
      .Add(L"vs", bela::no_argument, 1001) // load visual studio environment
      .Add(L"clang", bela::no_argument, 1002);
  bool cleanup = false;
  bool usevs = false;
  bool clang = false;
  bela::error_code ec;
  auto ret = pa.Execute(
      [&](int val, const wchar_t *oa, const wchar_t *) {
        switch (val) {
        case 'h':
          Usage();
          exit(0);
        case 'v':
          Version();
          exit(0);
        case 'C':
          cleanup = true;
          break;
        case 'V':
          IsDebugMode = true;
          break;
        case 'W':
          cmd.cwd = oa;
          break;
        case 1001:
          usevs = true;
          break;
        case 1002:
          clang = true;
          break;
        default:
          break;
        }
        return true;
      },
      ec);
  if (!ret) {
    bela::FPrintF(stderr, L"ParseArgv: %s\n", ec.message);
    return false;
  }
  const auto &Argv = pa.UnresolvedArgs();
  if (pa.UnresolvedArgs().empty()) {
    bela::FPrintF(stderr, L"no command input\n");
    Usage();
    exit(1);
  }
  bela::env::Derivator dev;
  for (size_t i = 0; i < Argv.size(); i++) {
    const auto arg = Argv[i];
    auto pos = arg.find(L'=');
    if (pos == std::wstring::npos) {
      for (size_t j = i; j < Argv.size(); j++) {
        cmd.argv.emplace_back(Argv[j]);
      }
      break;
    }
    dev.SetEnv(arg.substr(0, pos), arg.substr(pos + 1));
  }
  baulk::env::Searcher searcher(dev);
  if (!searcher.InitializeBaulk(ec)) {
    bela::FPrintF(stderr, L"InitializeBaulk failed %s\n", ec.message);
    return false;
  }
  searcher.InitializeGit(cleanup, ec);
  if (usevs) {
    if (!searcher.InitializeVisualStudioEnv(clang, ec)) {
      bela::FPrintF(stderr, L"InitializeVisualStudioEnv failed %s\n", ec.message);
      return false;
    }
    if (!searcher.InitializeWindowsKitEnv(ec)) {
      bela::FPrintF(stderr, L"InitializeWindowsKitEnv failed %s\n", ec.message);
      return false;
    }
  }

  if (cleanup) {
    cmd.env = searcher.CleanupEnv();
    return true;
  }
  cmd.env = searcher.MakeEnv();
  return true;
}
} // namespace baulk::exec

int wmain(int argc, wchar_t **argv) {
  baulk::exec::baulkcommand_t cmd;
  if (!baulk::exec::ParseArgv(argc, argv, cmd)) {
    return 1;
  }
  return cmd();
}