#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    openlog(NULL, 0, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Error: Invalid number of arguments. Expected 2, got %d.", argc - 1);
        printf("Usage: %s <writefile> <writestr>\n", argv[0]);
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    FILE *file = fopen(writefile, "w");
    if (file == NULL) {
        syslog(LOG_ERR, "Error opening file %s: %s", writefile, strerror(errno)); 
        return 1;
    }

    if (fputs(writestr, file) == EOF) {
        syslog(LOG_ERR, "Error writing to file %s: %s", writefile, strerror(errno));
        fclose(file);
        return 1;
    }

    fclose(file);
    closelog();

    return 0;
}
