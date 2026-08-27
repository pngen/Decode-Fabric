#pragma once
#include <windows.h>
#include <string>
#include <memory>
inline std::string tools_dir() {
  char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p(buf, n);
  std::size_t slash = p.find_last_of("\\/");
  if (slash != std::string::npos) p = p.substr(0, slash);
  // test exe is in <build>/tests; tools are in <build>/tools.
  p += "\\..\\tools";
  return p;
}
struct SpawnedProcess {
  HANDLE h = nullptr; DWORD pid = 0; bool ok = false;
  bool kill() { if (!h) return false; BOOL r = TerminateProcess(h, 1); WaitForSingleObject(h, 3000); CloseHandle(h); h = nullptr; return r != 0; }
};
inline SpawnedProcess spawn_process(const std::string& exe, const std::string& args, const std::string& cwd) {
  SpawnedProcess sp;
  std::string cmd = "\"" + exe + "\" " + args; STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
  char* cmd_buf = new char[cmd.size() + 1]; std::memcpy(cmd_buf, cmd.c_str(), cmd.size() + 1);
  BOOL ok = CreateProcessA(nullptr, cmd_buf, nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, cwd.c_str(), &si, &pi);
  delete[] cmd_buf;
  if (ok) { sp.h = pi.hProcess; sp.pid = pi.dwProcessId; sp.ok = true; }
  return sp;
}
