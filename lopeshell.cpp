// ============================================================
// CST-315 Project 4: lopeShell + Virtual Memory Manager
//

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <cstdlib>
#include <limits.h>
#include <iomanip>
#include <vector>
#include <queue>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>

using namespace std;

// ============================================================
// VMM
// ============================================================

// ── Constants ────────────────────────────────────────────────
static const int PAGE_SIZE       = 4096;   // 4 KB pages
static const int NUM_FRAMES      = 16;     // 64 KB physical RAM
static const int PAGES_PER_PROC  = 64;     // 256 KB virtual per process
static const int MAX_PROCS       = 8;
static const int PHYS_MEM_SIZE   = NUM_FRAMES   * PAGE_SIZE;
static const int VIRT_MEM_SIZE   = PAGES_PER_PROC * PAGE_SIZE;
static const int SWAP_SLOTS      = MAX_PROCS * PAGES_PER_PROC;
static const char SWAP_FILE[]    = "/tmp/lope_swap.bin";

// ── Page Table Entry ─────────────────────────────────────────
struct PageTableEntry {
    int  frame;      // physical frame (-1 = not in RAM)
    bool valid;      // in physical memory
    bool dirty;      // written since loaded
    bool read_only;
    int  swap_slot;  // disk slot (-1 = never evicted)
    PageTableEntry(): frame(-1), valid(false), dirty(false),
                      read_only(false), swap_slot(-1) {}
};

// ── Frame Table Entry ────────────────────────────────────────
struct FrameTableEntry {
    int  pid;
    int  page;
    bool free;
    FrameTableEntry(): pid(-1), page(-1), free(true) {}
};

// ── Process Memory ───────────────────────────────────────────
struct ProcMem {
    int            pid;
    bool           active;
    PageTableEntry pt[PAGES_PER_PROC];
    char           data[PAGES_PER_PROC * PAGE_SIZE];
    ProcMem(): pid(-1), active(false) {
        for (int i = 0; i < PAGES_PER_PROC * PAGE_SIZE; i++)
            data[i] = (char)(i % 256);
    }
};

// ── VMM Class ────────────────────────────────────────────────
class VMM {
public:
    VMM();
    ~VMM();

    int  createProcess();
    void terminateProcess(int pid);
    char readByte(int pid, int va);
    void writeByte(int pid, int va, char val);
    void loadFileIntoProcess(int pid, const string& filename);
    void printMemInfo()      const;
    void printStats()        const;
    void printTranslation(int pid, int va) const;

private:
    char            ram[PHYS_MEM_SIZE];
    FrameTableEntry ft[NUM_FRAMES];
    ProcMem         procs[MAX_PROCS];
    int             swap_fd;
    bool            swap_used[SWAP_SLOTS];
    queue<int>      fifo;
    long            tick;
    long            stat_faults, stat_evictions,
                    stat_swap_writes, stat_swap_reads;
    int             active_count;

    void initSwap();
    int  allocSwapSlot();
    void freeSwapSlot(int s);
    void writeSwap(int s, const char* data);
    void readSwap(int s, char* data);
    int  allocFrame(int pid, int page);
    int  evict();
    void loadPage(int pid, int page, int frame);
    void pageFault(int pid, int page);
    void validate(int pid, int va) const;
};

VMM::VMM(): swap_fd(-1), tick(0),
            stat_faults(0), stat_evictions(0),
            stat_swap_writes(0), stat_swap_reads(0),
            active_count(0) {
    memset(ram, 0, sizeof(ram));
    memset(swap_used, false, sizeof(swap_used));
    initSwap();
}

VMM::~VMM() {
    if (swap_fd >= 0) close(swap_fd);
    unlink(SWAP_FILE);
}

void VMM::initSwap() {
    swap_fd = open(SWAP_FILE, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (swap_fd < 0) { perror("swap open"); return; }
    char zero[PAGE_SIZE] = {};
    for (int i = 0; i < SWAP_SLOTS; i++) write(swap_fd, zero, PAGE_SIZE);
}

int  VMM::allocSwapSlot() {
    for (int i = 0; i < SWAP_SLOTS; i++)
        if (!swap_used[i]) { swap_used[i]=true; return i; }
    throw runtime_error("swap space full");
}
void VMM::freeSwapSlot(int s) { if (s>=0&&s<SWAP_SLOTS) swap_used[s]=false; }

void VMM::writeSwap(int s, const char* d) {
    lseek(swap_fd, (off_t)s*PAGE_SIZE, SEEK_SET);
    write(swap_fd, d, PAGE_SIZE);
    stat_swap_writes++;
}
void VMM::readSwap(int s, char* d) {
    lseek(swap_fd, (off_t)s*PAGE_SIZE, SEEK_SET);
    read(swap_fd, d, PAGE_SIZE);
    stat_swap_reads++;
}

int VMM::createProcess() {
    if (active_count >= MAX_PROCS) return -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].active) {
            procs[i].active = true;
            procs[i].pid    = i;
            for (int p = 0; p < PAGES_PER_PROC; p++)
                procs[i].pt[p] = PageTableEntry();
            active_count++;
            return i;
        }
    }
    return -1;
}

void VMM::terminateProcess(int pid) {
    if (pid<0||pid>=MAX_PROCS||!procs[pid].active) return;
    for (int p = 0; p < PAGES_PER_PROC; p++) {
        auto& pte = procs[pid].pt[p];
        if (pte.valid) {
            ft[pte.frame] = FrameTableEntry();
            queue<int> q;
            while (!fifo.empty()) {
                int f = fifo.front(); fifo.pop();
                if (f != pte.frame) q.push(f);
            }
            fifo = q;
        }
        if (pte.swap_slot >= 0) freeSwapSlot(pte.swap_slot);
    }
    procs[pid].active = false;
    procs[pid].pid    = -1;
    active_count--;
}

void VMM::validate(int pid, int va) const {
    if (pid<0||pid>=MAX_PROCS||!procs[pid].active)
        throw runtime_error("invalid or inactive PID " + to_string(pid));
    if (va<0||va>=VIRT_MEM_SIZE)
        throw runtime_error("segmentation fault — address " +
                            to_string(va) + " out of range");
}

int VMM::allocFrame(int pid, int page) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (ft[i].free) {
            ft[i].free = false;
            ft[i].pid  = pid;
            ft[i].page = page;
            fifo.push(i);
            return i;
        }
    }
    return evict();
}

int VMM::evict() {
    if (fifo.empty()) throw runtime_error("no frame to evict");
    int vf = fifo.front(); fifo.pop();
    stat_evictions++;
    auto& fte = ft[vf];
    auto& pte = procs[fte.pid].pt[fte.page];
    cout << "[VMM EVICT] frame " << vf
         << " (PID=" << fte.pid << " page=" << fte.page << ")";
    if (pte.dirty && !pte.read_only) {
        cout << " → writing to swap\n";
        if (pte.swap_slot < 0) pte.swap_slot = allocSwapSlot();
        writeSwap(pte.swap_slot, ram + vf * PAGE_SIZE);
    } else {
        cout << " → clean, discarded\n";
    }
    pte.valid = false; pte.frame = -1; pte.dirty = false;
    ft[vf].free = false;
    fifo.push(vf);
    return vf;
}

void VMM::loadPage(int pid, int page, int frame) {
    char* dst = ram + frame * PAGE_SIZE;
    auto& pte = procs[pid].pt[page];
    if (pte.swap_slot >= 0) {
        readSwap(pte.swap_slot, dst);
        freeSwapSlot(pte.swap_slot);
        pte.swap_slot = -1;
        cout << "[VMM LOAD] page " << page << " PID=" << pid
             << " ← swap → frame " << frame << "\n";
    } else {
        memcpy(dst, procs[pid].data + page * PAGE_SIZE, PAGE_SIZE);
        cout << "[VMM LOAD] page " << page << " PID=" << pid
             << " → frame " << frame << "\n";
    }
    pte.valid = true; pte.dirty = false; pte.frame = frame;
    ft[frame].pid = pid; ft[frame].page = page;
}

void VMM::pageFault(int pid, int page) {
    stat_faults++;
    cout << "[VMM PAGE FAULT] PID=" << pid << " page=" << page << "\n";
    int frame = allocFrame(pid, page);
    loadPage(pid, page, frame);
}

char VMM::readByte(int pid, int va) {
    validate(pid, va);
    int page = va / PAGE_SIZE, offset = va % PAGE_SIZE;
    if (!procs[pid].pt[page].valid) pageFault(pid, page);
    return ram[procs[pid].pt[page].frame * PAGE_SIZE + offset];
}

void VMM::writeByte(int pid, int va, char val) {
    validate(pid, va);
    int page = va / PAGE_SIZE, offset = va % PAGE_SIZE;
    if (procs[pid].pt[page].read_only)
        throw runtime_error("segmentation fault — write to read-only page");
    if (!procs[pid].pt[page].valid) pageFault(pid, page);
    ram[procs[pid].pt[page].frame * PAGE_SIZE + offset] = val;
    procs[pid].pt[page].dirty = true;
}

void VMM::loadFileIntoProcess(int pid, const string& filename) {
    validate(pid, 0);
    ifstream f(filename, ios::binary);
    if (!f) throw runtime_error("cannot open file: " + filename);
    int va = 0;
    char c;
    while (f.get(c) && va < VIRT_MEM_SIZE) {
        int page = va / PAGE_SIZE;
        procs[pid].data[va] = c;
        // invalidate if already loaded so it reloads fresh
        procs[pid].pt[page].valid = false;
        va++;
    }
    cout << "[VMM] Loaded " << va << " bytes from \""
         << filename << "\" into PID=" << pid << "\n";
}

void VMM::printMemInfo() const {
    cout << "\n=== PHYSICAL FRAME TABLE ===\n";
    cout << left << setw(8) << "Frame"
         << setw(8) << "Free"
         << setw(8) << "PID"
         << setw(10) << "Page" << "\n";
    cout << string(34, '-') << "\n";
    for (int i = 0; i < NUM_FRAMES; i++) {
        cout << setw(8) << i
             << setw(8) << (ft[i].free ? "yes" : "no")
             << setw(8) << (ft[i].free ? -1 : ft[i].pid)
             << setw(10) << (ft[i].free ? -1 : ft[i].page) << "\n";
    }
    cout << "\n=== PAGE TABLES (active processes) ===\n";
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].active) continue;
        cout << "PID=" << i << ":\n";
        cout << left << setw(8) << "Page"
             << setw(8) << "Valid"
             << setw(8) << "Frame"
             << setw(8) << "Dirty"
             << setw(8) << "Swap" << "\n";
        cout << string(40, '-') << "\n";
        bool any = false;
        for (int p = 0; p < PAGES_PER_PROC; p++) {
            const auto& pte = procs[i].pt[p];
            if (!pte.valid && pte.swap_slot < 0) continue;
            cout << setw(8) << p
                 << setw(8) << (pte.valid ? "yes" : "no")
                 << setw(8) << pte.frame
                 << setw(8) << (pte.dirty ? "yes" : "no")
                 << setw(8) << pte.swap_slot << "\n";
            any = true;
        }
        if (!any) cout << "  (no pages loaded yet)\n";
    }
    cout << "\n";
}

void VMM::printStats() const {
    cout << "\n=== VMM STATISTICS ===\n";
    cout << "  Page faults  : " << stat_faults       << "\n";
    cout << "  Evictions    : " << stat_evictions     << "\n";
    cout << "  Swap writes  : " << stat_swap_writes   << "\n";
    cout << "  Swap reads   : " << stat_swap_reads    << "\n";
    cout << "  Active procs : " << active_count       << "\n\n";
}

void VMM::printTranslation(int pid, int va) const {
    if (pid<0||pid>=MAX_PROCS||!procs[pid].active) {
        cout << "Invalid PID\n"; return;
    }
    if (va<0||va>=VIRT_MEM_SIZE) {
        cout << "Segmentation fault — address out of range\n"; return;
    }
    int page   = va / PAGE_SIZE;
    int offset = va % PAGE_SIZE;
    const auto& pte = procs[pid].pt[page];
    cout << "\n  Virtual address : " << va        << "\n";
    cout << "  Page number     : " << page       << "\n";
    cout << "  Offset          : " << offset     << "\n";
    if (pte.valid) {
        int pa = pte.frame * PAGE_SIZE + offset;
        cout << "  Frame number    : " << pte.frame << "\n";
        cout << "  Physical address: " << pa
             << "  (formula: " << pte.frame << " × " << PAGE_SIZE
             << " + " << offset << " = " << pa << ")\n\n";
    } else {
        cout << "  Status: page NOT in RAM";
        if (pte.swap_slot >= 0)
            cout << " (in swap slot " << pte.swap_slot << ")";
        cout << "\n\n";
    }
}

// Global VMM instance (lives for the shell's lifetime)
static VMM g_vmm;

// ============================================================
// VMM COMMAND HANDLER
// ============================================================
void vmmHelp() {
    cout << "\nVMM built-in commands:\n";
    cout << "  vmm create               Create new VMM process\n";
    cout << "  vmm terminate <pid>      Free all memory for process\n";
    cout << "  vmm read  <pid> <va>     Read byte at virtual address\n";
    cout << "  vmm write <pid> <va> <v> Write byte to virtual address\n";
    cout << "  vmm translate <pid> <va> Show address translation\n";
    cout << "  vmm loadfile <pid> <f>   Load file into process memory\n";
    cout << "  vmm stats                Show page fault statistics\n";
    cout << "  vmm help                 Show this help\n";
    cout << "  meminfo                  Show frame table + page tables\n\n";
}

void handleVmmCommand(char** args, int argc) {
    if (argc < 2 || strcmp(args[1], "help") == 0) {
        vmmHelp(); return;
    }
    try {
        if (strcmp(args[1], "create") == 0) {
            int pid = g_vmm.createProcess();
            if (pid < 0) cout << "VMM: max processes reached\n";
            else         cout << "VMM: created process PID=" << pid << "\n";

        } else if (strcmp(args[1], "terminate") == 0) {
            if (argc < 3) { cout << "Usage: vmm terminate <pid>\n"; return; }
            int pid = atoi(args[2]);
            g_vmm.terminateProcess(pid);
            cout << "VMM: terminated PID=" << pid << "\n";

        } else if (strcmp(args[1], "read") == 0) {
            if (argc < 4) { cout << "Usage: vmm read <pid> <va>\n"; return; }
            int pid = atoi(args[2]), va = atoi(args[3]);
            char val = g_vmm.readByte(pid, va);
            cout << "VMM: PID=" << pid << " va=" << va
                 << " → value=" << (int)(unsigned char)val
                 << " (0x" << hex << (int)(unsigned char)val << dec << ")\n";

        } else if (strcmp(args[1], "write") == 0) {
            if (argc < 5) { cout << "Usage: vmm write <pid> <va> <value>\n"; return; }
            int pid = atoi(args[2]), va = atoi(args[3]);
            char val = (char)atoi(args[4]);
            g_vmm.writeByte(pid, va, val);
            cout << "VMM: wrote " << (int)(unsigned char)val
                 << " to PID=" << pid << " va=" << va << "\n";

        } else if (strcmp(args[1], "translate") == 0) {
            if (argc < 4) { cout << "Usage: vmm translate <pid> <va>\n"; return; }
            int pid = atoi(args[2]), va = atoi(args[3]);
            g_vmm.printTranslation(pid, va);

        } else if (strcmp(args[1], "loadfile") == 0) {
            if (argc < 4) { cout << "Usage: vmm loadfile <pid> <filename>\n"; return; }
            int pid = atoi(args[2]);
            g_vmm.loadFileIntoProcess(pid, args[3]);

        } else if (strcmp(args[1], "stats") == 0) {
            g_vmm.printStats();

        } else {
            cout << "VMM: unknown subcommand '" << args[1]
                 << "' — try: vmm help\n";
        }
    } catch (const exception& e) {
        cout << "[VMM ERROR] " << e.what() << "\n";
    }
}

// ============================================================
// SHELL CODE
// ============================================================

void exitShell(int sig) {
    cout << "\nbye!\n";
    exit(0);
}

void showHelp() {
    cout << "Built-in commands:\n";
    cout << "  cd <folder>       change directory\n";
    cout << "  pwd               show current directory\n";
    cout << "  help              show this help\n";
    cout << "  exit / quit       exit shell\n";
    cout << "  meminfo           show VMM frame + page tables\n";
    cout << "  vmm <cmd>         virtual memory manager (vmm help)\n";
    cout << "Use Linux commands like ls, date, cat, mkdir, etc.\n";
    cout << "Use & at end to run command in background.\n";
    cout << "Separate multiple commands with ;\n";
}

void runCommand(string command) {
    // trim leading/trailing whitespace
    while (!command.empty() &&
           (command[0] == ' ' || command[0] == '\t'))
        command.erase(0, 1);
    while (!command.empty() &&
           (command.back() == ' ' || command.back() == '\t'))
        command.pop_back();
    if (command.empty()) return;

    // background flag
    bool background = false;
    if (command.back() == '&') {
        background = true;
        command.pop_back();
        while (!command.empty() &&
               (command.back() == ' ' || command.back() == '\t'))
            command.pop_back();
    }

    // tokenize
    char buf[2048];
    strncpy(buf, command.c_str(), sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char* args[200];
    int argc = 0;
    char* tok = strtok(buf, " \t");
    while (tok && argc < 199) { args[argc++] = tok; tok = strtok(nullptr, " \t"); }
    args[argc] = nullptr;
    if (!args[0]) return;

    // ── VMM built-in ─────────────────────────────────────
    if (strcmp(args[0], "vmm") == 0) {
        handleVmmCommand(args, argc);
        return;
    }
    if (strcmp(args[0], "meminfo") == 0) {
        g_vmm.printMemInfo();
        return;
    }

    if (strcmp(args[0], "cd") == 0) {
        if (!args[1]) cout << "cd: missing folder name\n";
        else if (chdir(args[1]) != 0) perror("cd failed");
        return;
    }
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) cout << cwd << "\n";
        else perror("pwd failed");
        return;
    }
    if (strcmp(args[0], "help") == 0) { showHelp(); return; }
    if (strcmp(args[0], "exit") == 0 || strcmp(args[0], "quit") == 0) {
        cout << "bye!\n"; exit(0);
    }

    // ── External command ──────────────────────────────────
    pid_t pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        perror("execvp failed");
        exit(1);
    } else if (pid > 0) {
        if (background) cout << "Background PID=" << pid << "\n";
        else            waitpid(pid, nullptr, 0);
    } else {
        perror("fork failed");
    }
}

void runLine(string line) {
    char buf[2048];
    strncpy(buf, line.c_str(), sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char* cmd = strtok(buf, ";");
    while (cmd) { runCommand(cmd); cmd = strtok(nullptr, ";"); }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

// ============================================================
// MAIN
// ============================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, exitShell);

    cout << "===========================================\n";
    cout << " lopeShell — CST-315 Project 4\n";
    cout << " Shell + Virtual Memory Manager\n";
    cout << " Type 'help' for commands, 'vmm help' for VMM\n";
    cout << "===========================================\n";

    // batch mode
    if (argc == 2) {
        ifstream fin(argv[1]);
        if (!fin) { cout << "Cannot open: " << argv[1] << "\n"; return 1; }
        string line;
        while (getline(fin, line)) {
            cout << line << "\n";
            if (line == "exit" || line == "quit") break;
            if (!line.empty()) runLine(line);
        }
        return 0;
    }

    // interactive mode
    string line;
    while (true) {
        cout << "$lopeShell> ";
        if (!getline(cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (!line.empty()) runLine(line);
    }
    cout << "bye!\n";
    return 0;
}
