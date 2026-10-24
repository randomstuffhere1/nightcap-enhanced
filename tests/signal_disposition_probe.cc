// Writes this process's ignored-signal mask to the file named by argv[1], in
// /proc/<pid>/status spelling: "SigIgn:\t0000000000000000".
//
// The running-instance tests need to know what a program started by
// StartDetached inherits. A shell script cannot answer that, because the shell
// sets signals of its own before the command it runs (bash invoked as sh
// ignores SIGINT on itself, so a wrapper reports the shell's choice, not what
// the exec'd process was given). Reading the mask from this process measures
// exactly what StartDetached passed on, with nothing in between.
//
// The file is written under a temporary name and renamed, so a parent that
// polls for it never reads a half-written mask.

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// The first "SigIgn:" line of /proc/self/status, or empty when /proc cannot be
// read. std::getline stops at the mask, so the whole file is only read once.
std::string IgnoredSignalLine() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.compare(0, 7, "SigIgn:") == 0) {
      return line;
    }
  }
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 || argv[1][0] == '\0') {
    return 64;
  }
  const std::string line = IgnoredSignalLine();
  if (line.empty()) {
    return 65;
  }
  const std::string temporary = std::string(argv[1]) + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      return 66;
    }
    output << line << '\n';
    if (!output) {
      return 67;
    }
  }
  if (std::rename(temporary.c_str(), argv[1]) != 0) {
    return 68;
  }
  return 0;
}
