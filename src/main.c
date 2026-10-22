#include "wp_app.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return wp_selftest_main();
    if (argc > 1 && strcmp(argv[1], "--selftest-lobby") == 0) return wp_lobbytest_main();
    if (argc > 1 && strcmp(argv[1], "--ask-test") == 0) return wp_asktest_main(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--join") == 0) return wp_robot_main(argc, argv);
    if (argc > 2 && strcmp(argv[1], "--search") == 0) return wp_search_main(argv[2]);
    if (argc > 1 && strcmp(argv[1], "--version") == 0) { printf("%s %s\n", WP_APP_NAME, WP_APP_VERSION); return 0; }
    return wp_app_main(argc, argv);
}
