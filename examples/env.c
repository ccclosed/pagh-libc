#include <stdio.h>
#include <stdlib.h>
int main(void) {
    const char *pwd = getenv("PWD");
    printf("PWD=%s\n", pwd ? pwd : "(unset)");
    return pwd ? 0 : 1;
}
