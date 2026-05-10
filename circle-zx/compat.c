/* Bare-metal shim for tzx.c debug prints. */
int printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
