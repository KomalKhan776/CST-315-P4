# CST-315-P4
# Virtual memory manager

Custom shell lopeshell built in C++ that runs Linux commands in both interactive and batch mode. Supports concurrent command execution using `fork()`, `execvp()`, and `waitpid()`. Extended in this project with fully integrated VMM that simulates demand paging, FIFO page replacement, dirty page tracking, and real on-disk swap file

## How to Run

### Prerequisites
- Linux
- g++ compiler
- Kate text editor

### Steps
1. Write code **[lopeShell.cpp](lopeShell.cpp)**
2. Compile the program:
   ```
   Or using the Makefile:
   ```
   make
   ```
3. Run in **interactive mode**:
   ```
   ./lopeShell
   ```
4. Run in **batch mode**:
   ```
   ./lopeShell batch.txt
   ```
5. To exit interactive mode type `exit` or `quit`, or press `Ctrl + C`

## Usage

### Interactive Mode
Type commands at the prompt one at a time:
```
$lopeShell> help
$lopeShell> ls
$lopeShell> pwd
$lopeShell> ls -l; touch file; pwd
$lopeShell> vmm help
$lopeShell> vmm create
$lopeShell> vmm read 0 0
$lopeShell> vmm write 0 4096 65
$lopeShell> vmm translate 0 4096
$lopeShell> meminfo
$lopeShell> vmm stats
$lopeShell> vmm terminate 0
$lopeShell> quit
```

### Batch Mode
The shell reads commands from **[batch.txt](batch.txt)**, echoes each line, and executes them automatically:
```
help
vmm help
vmm create
vmm create
vmm create
vmm read 0 0
vmm read 0 4096
vmm write 0 8192 65
vmm read 0 8192
vmm translate 0 8192
meminfo
vmm stats
vmm terminate 0
quit
```

## VMM Commands

| Command | Description |
| --- | --- |
| `vmm create` | create new process in the VMM, returns PID |
| `vmm terminate <pid>` | Free all frames and swap slots held by process |
| `vmm read <pid> <va>` | read byte at virtual address, triggers page fault if not in RAM |
| `vmm write <pid> <va> <value>` | write byte to virtual address, marks page dirty |
| `vmm translate <pid> <va>` | show full address translation: virtual → page → frame → physical |
| `vmm loadfile <pid> <file>` | load a file contents into a process virtual memory |
| `vmm stats` | show page faults, evictions, swap reads and writes |
| `vmm help` | show all VMM commands |
| `meminfo` | show physical frame table and all active page tables |

## Key Controls

| Input | Action |
| --- | --- |
| `quit` or `exit` | exit shell |
| `Ctrl + C` | exit shell via signal handler |
| `ls -l; pwd; date` | run multiple commands separated by `;` |
| `sleep 5 &` | run command in background |

## Memory Configuration

| Parameter | Value |
| --- | --- |
| Page size | 4096 bytes (4 KB) |
| Physical memory | 16 frames = 65,536 bytes (64 KB) |
| Virtual memory per process | 64 pages = 262,144 bytes (256 KB) |
| Max processes | 8 |
| Page replacement | FIFO (First In First Out) |
| Swap file | /tmp/lope_swap.bin (on-disk file) |

## Output

- All Linux commands execute and display output directly in terminal
- In batch mode, each line is echoed before execution
- Multiple commands separated by `;` run concurrently and output together
- VMM page faults, loads, evictions, and swap operations are logged to terminal in real time
- `meminfo` displays the full frame table and per-process page tables
- `vmm stats` displays total page faults, evictions, swap writes, and swap reads

