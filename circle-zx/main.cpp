#include "kernel.h"
#include <circle/startup.h>

int main(void)
{
    CKernel kernel;
    if (!kernel.Initialize()) {
        halt();
        return EXIT_HALT;
    }

    TShutdownMode shutdown_mode = kernel.Run();

    switch (shutdown_mode) {
    case ShutdownReboot:
        reboot();
        return EXIT_REBOOT;
    case ShutdownHalt:
    default:
        halt();
        return EXIT_HALT;
    }
}
