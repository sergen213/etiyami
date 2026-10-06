/* Executes the release's actual 32-bit WM_ACTIVATE handler, without Wine.
 * Build: gcc -m32 -no-pie -Wl,-Ttext-segment=0x10000000 -o /tmp/yami-activation-check check_activation.c
 * Run: /tmp/yami-activation-check game/eti-reference.exe
 * The original game/eti.exe must fail on WA_CLICKACTIVE (2).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/mman.h>

typedef int (__attribute__((stdcall)) *window_proc)(int, unsigned, unsigned, int);
int main(int argc, char **argv) {
    if (argc != 2) return 10;
    void *image = mmap((void *)0x400000, 0x100000, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (image == MAP_FAILED) { perror("mmap"); return 11; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror("fopen"); return 12; }
    size_t size = fread(image, 1, 0x100000, file);
    fclose(file);
    if (size != 434176 || *(unsigned short *)image != 0x5a4d) return 13;
    /* This release maps .text virtual addresses directly to file offsets. */
    window_proc handler = (window_proc)0x432fa0;
    volatile unsigned char *lost = (void *)0x4ff694;
    volatile unsigned char *acquire = (void *)0x4ff695;
    volatile unsigned char *active = (void *)0x4ff698;
    for (unsigned state = 0; state <= 2; ++state) {
        *lost = *acquire = *active = 0;
        *(volatile unsigned char *)0x467254 = 0;
        handler(0, 6, state, 0);
        if (*active != (state != 0) || *acquire != (state != 0) || *lost != (state == 0)) {
            fprintf(stderr, "FAIL activation %u: active=%u reacquire=%u lost=%u\n",
                    state, *active, *acquire, *lost);
            return 1;
        }
    }
    puts("PASS: inactive, keyboard activation, and mouse-click activation");
    return 0;
}
