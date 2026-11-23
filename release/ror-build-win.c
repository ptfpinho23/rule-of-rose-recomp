/* ror-build.exe: starts buildkit\python\python.exe -I -B buildkit\ror_build.py
 * with this program's own arguments, passes its exit code on, and takes the
 * Python process (and the compilers it starts) down with it when it is
 * killed: the child is put in a job object that is closed when this process
 * ends. See buildkit\ror_build.py for what the tool does. */
#include <windows.h>
#include <wchar.h>

static const wchar_t *skip_argv0(const wchar_t *c) {
    if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c) c++; }
    else while (*c && *c != L' ' && *c != L'\t') c++;
    while (*c == L' ' || *c == L'\t') c++;
    return c;
}
int wmain(void) {
    static wchar_t dir[32768], cmd[65536];
    DWORD n = GetModuleFileNameW(0, dir, 32000);
    while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    _snwprintf(cmd, 65535, L"\"%lsbuildkit\\python\\python.exe\" -I -B \"%lsbuildkit\\ror_build.py\" %ls", dir, dir, skip_argv0(GetCommandLineW()));
    HANDLE job = CreateJobObjectW(0, 0);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(0, cmd, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi)) {
        DWORD w; const char *m = "ERROR the bundled Python (buildkit\\python\\python.exe) could not be started\n";
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), m, (DWORD)lstrlenA(m), &w, 0);
        return 1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
