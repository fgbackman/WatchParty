#ifndef WP_APP_H
#define WP_APP_H

#define WP_APP_NAME    "WatchParty"
#define WP_APP_VERSION "1.1"

int wp_app_main(int argc, char **argv);
int wp_selftest_main(void);
int wp_robot_main(int argc, char **argv);
int wp_search_main(const char *query);
int wp_lobbytest_main(void);
int wp_asktest_main(int argc, char **argv);

#endif
