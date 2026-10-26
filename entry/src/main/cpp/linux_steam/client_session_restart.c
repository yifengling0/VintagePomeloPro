// HAP-owned small supervisor update. Keep the pinned GNU component untouched.
// The original supervisor continues to validate the real Steam source PID and
// propagate signals. Its outer desktop remains alive across explicit restarts,
// preserving Xwayland, the Wine window host and Deck's one-shot command sequence.
#define main SteamOhosClientAttempt
#include "gnu_fixture/steamclient_session.c"
#undef main

static void remove_startup_play(char **args) {
    size_t output = 1;
    for (size_t i = 1; args[i]; ++i) {
        if (!strcmp(args[i], "-applaunch")) { if (args[i + 1]) ++i; continue; }
        if (!strncmp(args[i], "steam://rungameid/", 18) || !strncmp(args[i], "steam://run/", 12)) continue;
        args[output++] = args[i];
    }
    args[output] = NULL;
}

int main(int argc, char **argv) {
    const char *generation = getenv("HMOSDECK_SESSION_GENERATION");
    const char *source = getenv("STEAMOHOS_STEAMCLIENT_SOURCE_TEST");
    const bool product = generation && *generation && strlen(generation) <= 64 &&
        strspn(generation, "0123456789abcdef-") == strlen(generation) && (!source || strcmp(source, "1"));
    for (unsigned restarts = 0;;) {
        int result = SteamOhosClientAttempt(argc, argv);
        if (!product || result != 42 || restarts >= 2) return result;
        remove_startup_play(argv + 1);
        fprintf(stderr, "HMOSDECK_STEAM_RESTART reason=client_requested exit=42 attempt=%u\n", ++restarts);
    }
}
