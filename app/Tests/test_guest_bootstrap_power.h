/* Native fixture replaces only the guest's power-management API. */
#include <stdlib.h>
static void event(const char *message) {
    FILE *file = fopen("fixture/power.log", "a");
    if (file) { fprintf(file, "%s\n", message); fclose(file); }
}
static int power_begin(void) {
    event("acquire");
    return getenv("S5LBOX_TEST_POWER_FAIL") == NULL;
}
static void power_end(void) { event("release"); }
