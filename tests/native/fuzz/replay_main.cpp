// Runs fuzz inputs without libFuzzer, so the corpus is a regression test that builds with plain g++ (make run).
// Each input runs in a fork()ed child of a process whose firmware RAM is untouched, so no state leaks between
// inputs; a child that dies (ASan, UBSan, an invariant trap) is a crash.
//
//   replay_<target> [--expect-crash] [-v] <dir or file>...
//
// --expect-crash: every input must crash on an invariant trap (fuzz/crashes/: reproducers of known, unfixed bugs,
// like XFAIL_TEST). One that no longer crashes fails the run, so whoever fixes the bug moves it into the corpus; one
// that crashes some other way (a harness abort, a sanitizer report) fails it too. -v shows each input and the
// children's output (with --expect-crash it's hidden otherwise).
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv);
// Present in the coverage build (-fprofile-instr-generate): the child leaves with _exit, which skips the atexit
// handler that would write its profile.
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));

static bool readFile(const std::string &path, std::vector<uint8_t> &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[4096];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return true;
}

static void collect(const std::string &path, std::vector<std::string> &files) {
  struct stat st;
  if (stat(path.c_str(), &st)) {
    fprintf(stderr, "replay: no such file or directory: %s\n", path.c_str());
    exit(2);
  }
  if (!S_ISDIR(st.st_mode)) {
    files.push_back(path);
    return;
  }
  DIR *d = opendir(path.c_str());
  if (!d) return;
  std::vector<std::string> found;
  while (struct dirent *e = readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.' || name.rfind("README", 0) == 0) continue;
    std::string full = path + "/" + name;
    if (!stat(full.c_str(), &st) && S_ISREG(st.st_mode)) found.push_back(full);
  }
  closedir(d);
  std::sort(found.begin(), found.end());
  files.insert(files.end(), found.begin(), found.end());
}

int main(int argc, char **argv) {
  bool expectCrash = false, verbose = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--expect-crash")) expectCrash = true;
    else if (!strcmp(argv[i], "-v")) verbose = true;
    else collect(argv[i], files);
  }
  LLVMFuzzerInitialize(&argc, &argv);
  int bad = 0;
  for (const std::string &path : files) {
    std::vector<uint8_t> data;
    if (!readFile(path, data)) {
      fprintf(stderr, "replay: can't read %s\n", path.c_str());
      bad++;
      continue;
    }
    fflush(stdout);
    fflush(stderr);
    // A known bug's reproducer must still trip its invariant, not crash some other way (a harness abort, a new
    // bug): its child's stderr comes back through a pipe and must hold the invariant report.
    int out[2] = { -1, -1 };
    if (expectCrash && pipe(out)) {
      perror("pipe");
      return 2;
    }
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return 2;
    }
    if (pid == 0) {
      if (expectCrash) {
        close(out[0]);
        dup2(out[1], 2);
        close(out[1]);
      }
      LLVMFuzzerTestOneInput(data.data(), data.size());
      if (__llvm_profile_write_file) __llvm_profile_write_file();
      _exit(0);
    }
    std::string err;
    if (expectCrash) {
      close(out[1]);
      char buf[4096];
      ssize_t n;
      while ((n = read(out[0], buf, sizeof(buf))) > 0) err.append(buf, (size_t)n);
      close(out[0]);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool crashed = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char how[64] = "ok";
    if (WIFSIGNALED(status)) snprintf(how, sizeof(how), "signal %d", WTERMSIG(status));
    else if (crashed) snprintf(how, sizeof(how), "exit %d", WEXITSTATUS(status));
    bool invariant = err.find("==GATELINK INVARIANT==") != std::string::npos;
    if (verbose && expectCrash) fputs(err.c_str(), stderr);
    if (crashed != expectCrash) {
      bad++;
      if (expectCrash) printf("NO LONGER CRASHES (fixed? move it to the corpus): %s\n", path.c_str());
      else printf("CRASH (%s): %s\n", how, path.c_str());
    } else if (expectCrash && !invariant) {
      bad++;
      printf("CRASHES, BUT NOT ON AN INVARIANT (%s): %s\n%s", how, path.c_str(),
             err.size() > 2000 ? err.substr(err.size() - 2000).c_str() : err.c_str());
    } else if (verbose) {
      printf("%s (%s): %s\n", expectCrash ? "still crashes" : "ok", how, path.c_str());
    }
  }
  printf("%s: %zu input%s%s, %d unexpected\n", argv[0], files.size(), files.size() == 1 ? "" : "s",
         expectCrash ? " expected to crash" : "", bad);
  return bad ? 1 : 0;
}
