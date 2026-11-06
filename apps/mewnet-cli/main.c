#include "mewnet_subs.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct Cli {
    mewnet_subs* db;
    mewnet_instance* milib;
    mewnet_agent* agent;
    int64_t group_id;
    int actual_port;
    bool headless;
    int run_seconds;
    char* db_path;

    pthread_mutex_t lock;
    mewnet_agent_snapshot snapshot;
    char logs[16][1024];
    int log_count;
    int log_next;
} Cli;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_switch = 0;

static void on_signal(int sig) {
    if (sig == SIGUSR1) {
        g_switch = 1;
    } else {
        g_stop = 1;
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void timestamp(char* out, size_t size) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(out, size, "%H:%M:%S", &local);
}

static void on_log(const char* message, void* user) {
    Cli* cli = (Cli*)user;
    if (cli->headless) {
        char stamp[32];
        timestamp(stamp, sizeof(stamp));
        printf("%s  %s\n", stamp, message);
        fflush(stdout);
        return;
    }
    pthread_mutex_lock(&cli->lock);
    snprintf(cli->logs[cli->log_next], sizeof(cli->logs[0]), "%s", message);
    cli->log_next = (cli->log_next + 1) % 16;
    if (cli->log_count < 16) cli->log_count++;
    pthread_mutex_unlock(&cli->lock);
}

static void on_state(const mewnet_agent_snapshot* snapshot, void* user) {
    Cli* cli = (Cli*)user;
    pthread_mutex_lock(&cli->lock);
    cli->snapshot = *snapshot;
    pthread_mutex_unlock(&cli->lock);
    if (cli->headless) {
        printf("STATE id=%lld name=%s alive=%lld proxies=%lld untested=%lld ready=%d\n",
               (long long)snapshot->proxy_id, snapshot->name,
               (long long)snapshot->alive, (long long)snapshot->proxies,
               (long long)snapshot->untested, snapshot->ready ? 1 : 0);
        fflush(stdout);
    }
}

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [options] [SUBSCRIPTION_URL...]\n"
            "  --db PATH              sqlite database (default data/mewnet-cli.db)\n"
            "  --proxy-port N         mixed proxy port (0 = free)\n"
            "  --batch N              fast discovery batch (70)\n"
            "  --concurrency N        concurrent checks (70)\n"
            "  --timeout MS           per-proxy timeout (4000)\n"
            "  --fetch-interval S     subscription refresh interval (3600)\n"
            "  --slow-interval S      discovery step when pool is full (24)\n"
            "  --pool-target N        verified proxies to keep (4)\n"
            "  --health-interval S    active proxy health period (6)\n"
            "  --health-timeout MS    health timeout (2500)\n"
            "  --health-url URL       health URL\n"
            "  --test-url URL         discovery test URL\n"
            "  --headless             no TUI, plain log lines\n"
            "  --run-seconds N        stop after N seconds (headless tests)\n",
            argv0);
}

typedef struct Options {
    char* db_path;
    int proxy_port;
    size_t batch;
    size_t concurrency;
    int timeout_ms;
    int fetch_interval_sec;
    int slow_interval_sec;
    int pool_target;
    int health_interval_sec;
    int health_timeout_ms;
    char* health_url;
    char* health_expected;
    char* test_url;
    char* test_expected;
    bool headless;
    int run_seconds;
} Options;

static void options_default(Options* options) {
    memset(options, 0, sizeof(*options));
    options->db_path = strdup("data/mewnet-cli.db");
    options->batch = 70;
    options->concurrency = 70;
    options->timeout_ms = 6000;
    options->fetch_interval_sec = 3600;
    options->slow_interval_sec = 24;
    options->pool_target = 12;
    options->health_interval_sec = 6;
    options->health_timeout_ms = 2500;
    options->health_url = strdup("https://lua.org");
    options->health_expected = strdup("200");
    options->test_url = strdup("https://www.gstatic.com/generate_204");
    options->test_expected = strdup("204");
}

static void options_free(Options* options) {
    free(options->db_path);
    free(options->health_url);
    free(options->health_expected);
    free(options->test_url);
    free(options->test_expected);
}

static bool take_value(int argc, char** argv, int* index, const char** value) {
    if (*index + 1 >= argc) {
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

static bool parse_options(int argc, char** argv, Options* options, int* first_url) {
    *first_url = argc;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char* value = NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--db") == 0 && take_value(argc, argv, &i, &value)) {
            free(options->db_path);
            options->db_path = strdup(value);
        } else if (strcmp(arg, "--proxy-port") == 0 && take_value(argc, argv, &i, &value)) {
            options->proxy_port = atoi(value);
        } else if (strcmp(arg, "--batch") == 0 && take_value(argc, argv, &i, &value)) {
            options->batch = (size_t)atol(value);
        } else if (strcmp(arg, "--concurrency") == 0 && take_value(argc, argv, &i, &value)) {
            options->concurrency = (size_t)atol(value);
        } else if (strcmp(arg, "--timeout") == 0 && take_value(argc, argv, &i, &value)) {
            options->timeout_ms = atoi(value);
        } else if (strcmp(arg, "--fetch-interval") == 0 && take_value(argc, argv, &i, &value)) {
            options->fetch_interval_sec = atoi(value);
        } else if (strcmp(arg, "--slow-interval") == 0 && take_value(argc, argv, &i, &value)) {
            options->slow_interval_sec = atoi(value);
        } else if (strcmp(arg, "--pool-target") == 0 && take_value(argc, argv, &i, &value)) {
            options->pool_target = atoi(value);
        } else if (strcmp(arg, "--health-interval") == 0 && take_value(argc, argv, &i, &value)) {
            options->health_interval_sec = atoi(value);
        } else if (strcmp(arg, "--health-timeout") == 0 && take_value(argc, argv, &i, &value)) {
            options->health_timeout_ms = atoi(value);
        } else if (strcmp(arg, "--health-url") == 0 && take_value(argc, argv, &i, &value)) {
            free(options->health_url);
            options->health_url = strdup(value);
        } else if (strcmp(arg, "--health-expected") == 0 && take_value(argc, argv, &i, &value)) {
            free(options->health_expected);
            options->health_expected = strdup(value);
        } else if (strcmp(arg, "--test-url") == 0 && take_value(argc, argv, &i, &value)) {
            free(options->test_url);
            options->test_url = strdup(value);
        } else if (strcmp(arg, "--test-expected") == 0 && take_value(argc, argv, &i, &value)) {
            free(options->test_expected);
            options->test_expected = strdup(value);
        } else if (strcmp(arg, "--headless") == 0) {
            options->headless = true;
        } else if (strcmp(arg, "--run-seconds") == 0 && take_value(argc, argv, &i, &value)) {
            options->run_seconds = atoi(value);
        } else if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option %s\n", arg);
            return false;
        } else {
            *first_url = i;
            break;
        }
    }
    return true;
}

static void render(Cli* cli) {
    mewnet_agent_snapshot snapshot;
    char logs[16][1024];
    int log_count = 0;
    int log_next = 0;
    pthread_mutex_lock(&cli->lock);
    snapshot = cli->snapshot;
    log_count = cli->log_count;
    log_next = cli->log_next;
    memcpy(logs, cli->logs, sizeof(logs));
    pthread_mutex_unlock(&cli->lock);

    printf("\033[2J\033[H");
    printf("mewnet agent\n");
    printf("local proxy : http://127.0.0.1:%d\n", cli->actual_port);
    if (snapshot.proxy_id != 0) {
        printf("active      : id=%lld  %s\n", (long long)snapshot.proxy_id, snapshot.name);
        printf("              %s %s:%d %u ms\n", snapshot.protocol, snapshot.server,
               snapshot.port, snapshot.delay_ms);
    } else {
        printf("active      : (none)\n");
    }
    printf("pool        : subscriptions=%lld proxies=%lld alive=%lld untested=%lld\n",
           (long long)snapshot.subscriptions, (long long)snapshot.proxies,
           (long long)snapshot.alive, (long long)snapshot.untested);
    printf("state       : %s\n\n", snapshot.ready ? "ready" : "discovering");
    printf("log:\n");
    int start = (log_next - log_count + 16) % 16;
    for (int i = 0; i < log_count; i++) {
        int index = (start + i) % 16;
        printf("  %s\n", logs[index]);
    }
    printf("\n[s] switch proxy   [q] quit\n");
    fflush(stdout);
}

static int run_tui(Cli* cli) {
    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        return 1;
    }
    struct termios raw = original;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int64_t last_render = 0;
    int result = 0;
    while (!g_stop) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int rc = poll(&pfd, 1, 200);
        if (rc > 0) {
            char key = 0;
            if (read(STDIN_FILENO, &key, 1) == 1) {
                if (key == 'q' || key == 'Q') {
                    break;
                }
                if (key == 's' || key == 'S') {
                    mewnet_agent_request_switch(cli->agent);
                }
            }
        }
        if (g_switch) {
            g_switch = 0;
            mewnet_agent_request_switch(cli->agent);
        }
        if (now_ms() - last_render >= 500) {
            render(cli);
            last_render = now_ms();
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    printf("\033[2J\033[H");
    return result;
}

int main(int argc, char** argv) {
    Options options;
    options_default(&options);
    int first_url = argc;
    if (!parse_options(argc, argv, &options, &first_url)) {
        options_free(&options);
        return 2;
    }
    bool headless = options.headless || !isatty(STDIN_FILENO);

    Cli cli;
    memset(&cli, 0, sizeof(cli));
    cli.headless = headless;
    cli.run_seconds = options.run_seconds;
    cli.db_path = options.db_path;
    pthread_mutex_init(&cli.lock, NULL);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = options.db_path;
    config.test_url = options.test_url;
    config.test_expected = options.test_expected;
    config.test_timeout_ms = options.timeout_ms;
    config.request_limit = 10;
    config.request_window_seconds = 240;
    config.default_fetch_interval_sec = options.fetch_interval_sec;

    char* path_copy = strdup(options.db_path);
    char* slash = strrchr(path_copy, '/');
    if (slash != NULL) {
        *slash = '\0';
        mkdir(path_copy, 0755);
    }
    free(path_copy);

    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open db: %s\n", opened.error.message);
        options_free(&options);
        return 1;
    }
    cli.db = opened.db;

    mewnet_error err = mewnet_subs_default_group(cli.db, &cli.group_id);
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "default group: %s\n", err.message);
        return 1;
    }
    for (int i = first_url; i < argc; i++) {
        err = mewnet_subs_add_subscription_in_group(cli.db, cli.group_id, argv[i], 0, NULL);
        if (err.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "add %s: %s\n", argv[i], err.message);
        }
    }

    mewnet_config milib_config = mewnet_config_default();
    milib_config.mixed_port = options.proxy_port;
    milib_config.log_level = "warning";
    mewnet_init_result init = mewnet_init(&milib_config);
    if (init.instance == NULL) {
        fprintf(stderr, "milib init: %s\n", init.error.message);
        return 1;
    }
    cli.milib = init.instance;
    cli.actual_port = mewnet_get_status(cli.milib).mixed_port;

    mewnet_agent_config agent_config = mewnet_agent_config_default();
    agent_config.db = cli.db;
    agent_config.milib = cli.milib;
    agent_config.group_id = cli.group_id;
    agent_config.proxy_port = cli.actual_port;
    agent_config.batch = options.batch;
    agent_config.concurrency = options.concurrency;
    agent_config.fetch_interval_sec = options.fetch_interval_sec;
    agent_config.slow_interval_sec = options.slow_interval_sec;
    agent_config.pool_target = options.pool_target;
    agent_config.health_interval_sec = options.health_interval_sec;
    agent_config.health_timeout_ms = options.health_timeout_ms;
    agent_config.health_url = options.health_url;
    agent_config.health_expected = options.health_expected;
    agent_config.run_seconds = options.run_seconds;
    mewnet_agent_callbacks callbacks = {on_log, on_state, &cli};
    cli.agent = mewnet_agent_start(&agent_config, callbacks);
    if (cli.agent == NULL) {
        fprintf(stderr, "agent start failed\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);

    printf("DB %s\nPROXY http://127.0.0.1:%d\n", options.db_path, cli.actual_port);
    fflush(stdout);

    int result = 0;
    if (headless) {
        int64_t started = now_ms();
        while (!g_stop) {
            if (g_switch) {
                g_switch = 0;
                mewnet_agent_request_switch(cli.agent);
            }
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000000};
            nanosleep(&pause, NULL);
            if (options.run_seconds > 0 &&
                now_ms() - started >= (int64_t)options.run_seconds * 1000) {
                break;
            }
        }
    } else {
        result = run_tui(&cli);
    }

    mewnet_agent_stop(cli.agent);
    mewnet_close(cli.milib);
    mewnet_subs_close(cli.db);
    pthread_mutex_destroy(&cli.lock);
    options_free(&options);
    return result;
}
