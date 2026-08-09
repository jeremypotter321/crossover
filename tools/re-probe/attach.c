/*
 * Attach a probe DLL to an ALREADY-RUNNING Fable.exe.
 *
 * `inject.exe` starts the game itself, which means every iteration costs a full
 * relaunch -- and with no save games that means replaying the intro. This one
 * finds the running process by image name and injects into it, so the game can
 * be left where it is.
 *
 *   usage: attach.exe <probe.dll> [process.exe]      (default Fable.exe)
 *
 * Note LoadLibrary is a no-op if the DLL is already mapped in the target: it
 * returns the existing handle without re-running DllMain. So copy the probe to
 * a fresh filename between attaches (probe2.dll, probe3.dll, ...) or nothing
 * will appear to happen.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static DWORD find_pid(const char *image)
{
    PROCESSENTRY32 pe = { sizeof pe };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;

    if (snap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return 0;
    }
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, image) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char **argv)
{
    const char *image = (argc > 2) ? argv[2] : "Fable.exe";
    char dllPath[MAX_PATH];
    HANDLE proc, thread;
    LPVOID remote;
    SIZE_T len;
    DWORD pid, exitCode = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <probe.dll> [process.exe]\n", argv[0]);
        return 2;
    }
    if (!GetFullPathNameA(argv[1], sizeof dllPath, dllPath, NULL)) {
        fprintf(stderr, "GetFullPathName failed: %lu\n", GetLastError());
        return 1;
    }

    pid = find_pid(image);
    if (!pid) {
        fprintf(stderr, "[!] no running process named %s\n", image);
        return 1;
    }
    printf("[*] %s is pid %lu\n", image, pid);
    printf("[*] dll: %s\n", dllPath);

    proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                       PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                       FALSE, pid);
    if (!proc) {
        fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    len = strlen(dllPath) + 1;
    remote = VirtualAllocEx(proc, NULL, len, MEM_COMMIT, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(proc, remote, dllPath, len, NULL)) {
        fprintf(stderr, "writing the path into the target failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }

    thread = CreateRemoteThread(proc, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                               "LoadLibraryA"),
        remote, 0, NULL);
    if (!thread) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (!exitCode) {
        fprintf(stderr, "[!] LoadLibrary returned NULL -- not loaded "
                        "(already mapped? use a fresh filename)\n");
        return 1;
    }
    printf("[+] attached, remote HMODULE = 0x%08lX\n", exitCode);
    return 0;
}
